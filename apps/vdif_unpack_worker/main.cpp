#include "rdma_dada/config/packet_format_config.h"
#include "rdma_dada/config/pipeline_config.h"
#include "rdma_dada/modules/vdif_unpack/vdif_unpack_config.h"
#include "rdma_dada/modules/vdif_unpack/vdif_unpack_engine.h"
#include "rdma_dada/modules/vdif_unpack/vdif_unpack_header.h"
#include "rdma_dada/pipeline/ascii_metadata.h"
#include "rdma_dada/pipeline/group_block_writer.h"

#include <dada_client.h>
#include <dada_hdu.h>
#include <ipcbuf.h>
#include <ipcio.h>
#include <multilog.h>

#include <algorithm>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace unpack = rdma_dada::modules::vdif_unpack;

volatile std::sig_atomic_t stop_requested = 0;

void HandleSignal(int) { stop_requested = 1; }

bool FitsSizeT(std::uint64_t value) {
    return value <= static_cast<std::uint64_t>(
                        std::numeric_limits<std::size_t>::max());
}

bool LockHduRead(dada_hdu_t* hdu) {
    if (ipcbuf_lock_read(hdu->header_block) < 0) return false;
    if (ipcio_open(hdu->data_block, 'R') >= 0) return true;
    ipcbuf_unlock_read(hdu->header_block);
    return false;
}

bool LockHduWrite(dada_hdu_t* hdu) {
    if (ipcbuf_lock_write(hdu->header_block) < 0) return false;
    if (ipcio_open(hdu->data_block, 'W') >= 0) return true;
    ipcbuf_unlock_write(hdu->header_block);
    return false;
}

class PsrdadaBlockSink : public rdma_dada::pipeline::WritableBlockSink {
public:
    PsrdadaBlockSink()
        : data_block_(NULL), expected_capacity_(0), block_open_(false) {}

    void Configure(ipcio_t* data_block, std::uint64_t expected_capacity) {
        data_block_ = data_block;
        expected_capacity_ = expected_capacity;
        block_open_ = false;
    }

    bool Acquire(std::uint8_t** data, std::uint64_t* capacity,
                 std::string* error) {
        if (!data || !capacity) return Fail("block output pointer is null", error);
        if (!data_block_) return Fail("PSRDADA data block is not configured", error);
        if (block_open_) return Fail("PSRDADA output block is already open", error);
        std::uint64_t block_id = 0;
        char* block = ipcio_open_block_write(data_block_, &block_id);
        if (!block) return Fail("cannot acquire output DADA data block", error);
        block_open_ = true;
        *data = reinterpret_cast<std::uint8_t*>(block);
        *capacity = ipcbuf_get_bufsz(reinterpret_cast<ipcbuf_t*>(data_block_));
        if (*capacity != expected_capacity_) {
            Abort();
            return Fail("output DADA block capacity changed during transfer", error);
        }
        return true;
    }

    bool Commit(std::uint64_t bytes, std::string* error) {
        if (!block_open_) return Fail("no PSRDADA output block is open", error);
        if (bytes > expected_capacity_) {
            Abort();
            return Fail("output byte count exceeds DADA block capacity", error);
        }
        if (ipcio_close_block_write(data_block_, bytes) < 0) {
            return Fail("cannot commit output DADA data block", error);
        }
        block_open_ = false;
        return true;
    }

    bool Abort() {
        if (!block_open_) return true;
        if (ipcio_close_block_write(data_block_, 0) < 0) return false;
        block_open_ = false;
        return true;
    }

private:
    static bool Fail(const std::string& message, std::string* error) {
        if (error) *error = message;
        return false;
    }

    ipcio_t* data_block_;
    std::uint64_t expected_capacity_;
    bool block_open_;
};

struct WorkerRuntime {
    WorkerRuntime()
        : log(NULL), output_hdu(NULL), output_locked(false), failed(false),
          input_block_capacity(0), output_block_capacity(0),
          raw_block_sequence(0), output_eod_sent(false) {}

    unpack::VdifUnpackConfig config;
    rdma_dada::PipelineConfig pipeline_config;
    rdma_dada::PipelineLayout pipeline_layout;
    unpack::VdifUnpackLayout unpack_layout;
    unpack::VdifUnpackEngine engine;
    rdma_dada::pipeline::GroupBlockWriter writer;
    PsrdadaBlockSink sink;
    multilog_t* log;
    dada_hdu_t* output_hdu;
    bool output_locked;
    bool failed;
    std::string error;
    std::uint64_t input_block_capacity;
    std::uint64_t output_block_capacity;
    std::uint64_t raw_block_sequence;
    bool output_eod_sent;
};

void SetFailure(WorkerRuntime* runtime, const std::string& message) {
    if (!runtime) return;
    if (!runtime->failed) runtime->error = message;
    runtime->failed = true;
    if (runtime->log) multilog(runtime->log, LOG_ERR, "%s\n", message.c_str());
}

bool EndOutputTransfer(WorkerRuntime* runtime) {
    bool ok = true;
    if (!runtime || !runtime->output_locked) return ok;
    if (!runtime->sink.Abort()) {
        SetFailure(runtime, "cannot discard open output DADA block");
        ok = false;
    }
    if (!runtime->output_eod_sent) {
        if (ipcio_stop(runtime->output_hdu->data_block) < 0) {
            SetFailure(runtime, "cannot publish output DADA data EOD");
            ok = false;
        } else {
            runtime->output_eod_sent = true;
        }
    }
    if (dada_hdu_unlock_write(runtime->output_hdu) < 0) {
        SetFailure(runtime, "cannot unlock output DADA ring");
        ok = false;
    }
    runtime->output_locked = false;
    return ok;
}

bool EmitGroup(WorkerRuntime* runtime, const unpack::VdifGroupKey&,
               const std::uint8_t* data, std::uint64_t bytes,
               std::string* error) {
    return runtime->writer.Append(data, bytes, error);
}

int OpenTransfer(dada_client_t* client) {
    WorkerRuntime* runtime =
        static_cast<WorkerRuntime*>(client ? client->context : NULL);
    if (!client || !runtime) return -1;
    runtime->failed = false;
    runtime->error.clear();
    runtime->raw_block_sequence = 0;
    runtime->output_eod_sent = false;

    try {
        rdma_dada::pipeline::Metadata input_header;
        std::string error;
        if (!rdma_dada::pipeline::ParseAsciiMetadata(
                client->header, client->header_size, &input_header, &error)) {
            SetFailure(runtime, "cannot parse input DADA header: " + error);
            return -1;
        }

        runtime->input_block_capacity = ipcbuf_get_bufsz(
            reinterpret_cast<ipcbuf_t*>(client->data_block));
        runtime->output_block_capacity = ipcbuf_get_bufsz(
            reinterpret_cast<ipcbuf_t*>(runtime->output_hdu->data_block));
        if (runtime->input_block_capacity !=
            runtime->pipeline_layout.raw_block_bytes) {
            std::ostringstream message;
            message << "input ring block capacity must be "
                    << runtime->pipeline_layout.raw_block_bytes
                    << " bytes; got " << runtime->input_block_capacity;
            SetFailure(runtime, message.str());
            return -1;
        }
        if (runtime->output_block_capacity !=
            runtime->unpack_layout.compute_block_bytes) {
            std::ostringstream message;
            message << "output ring block capacity must be "
                    << runtime->unpack_layout.compute_block_bytes
                    << " bytes; got " << runtime->output_block_capacity;
            SetFailure(runtime, message.str());
            return -1;
        }

        rdma_dada::pipeline::Metadata output_header;
        if (!unpack::BuildVdifUnpackOutputHeader(
                input_header, runtime->config, runtime->pipeline_config,
                runtime->pipeline_layout, runtime->unpack_layout,
                &output_header, &error)) {
            SetFailure(runtime, "cannot build output DADA header: " + error);
            return -1;
        }
        output_header.SetUint64("INPUT_BLOCK_BYTES",
                                runtime->input_block_capacity);
        output_header.SetUint64("OUTPUT_BLOCK_BYTES",
                                runtime->output_block_capacity);

        const std::uint64_t header_capacity =
            ipcbuf_get_bufsz(runtime->output_hdu->header_block);
        if (!FitsSizeT(header_capacity)) {
            SetFailure(runtime, "output header block exceeds host size_t range");
            return -1;
        }
        std::vector<char> serialized_header(
            static_cast<std::size_t>(header_capacity));
        if (!rdma_dada::pipeline::SerializeAsciiMetadata(
                output_header, serialized_header.data(), header_capacity,
                &error)) {
            SetFailure(runtime, "cannot serialize output DADA header: " + error);
            return -1;
        }

        if (!runtime->engine.Configure(
                runtime->config, runtime->pipeline_config,
                runtime->unpack_layout, &error)) {
            SetFailure(runtime, "cannot configure VDIF unpack engine: " + error);
            return -1;
        }
        runtime->sink.Configure(runtime->output_hdu->data_block,
                                runtime->output_block_capacity);
        if (!runtime->writer.Configure(
                runtime->unpack_layout.group_bytes,
                runtime->output_block_capacity, &runtime->sink, &error)) {
            SetFailure(runtime, "cannot configure compute block writer: " + error);
            return -1;
        }

        if (!LockHduWrite(runtime->output_hdu)) {
            SetFailure(runtime, "cannot lock output DADA ring for writing");
            return -1;
        }
        runtime->output_locked = true;
        char* header_block =
            ipcbuf_get_next_write(runtime->output_hdu->header_block);
        if (!header_block) {
            SetFailure(runtime, "cannot acquire output DADA header block");
            EndOutputTransfer(runtime);
            return -1;
        }
        std::copy(serialized_header.begin(), serialized_header.end(),
                  header_block);
        if (ipcbuf_enable_eod(runtime->output_hdu->header_block) < 0 ||
            ipcbuf_mark_filled(runtime->output_hdu->header_block,
                               header_capacity) < 0) {
            SetFailure(runtime, "cannot publish output DADA header block");
            EndOutputTransfer(runtime);
            return -1;
        }

        client->transfer_bytes = 0;
        client->optimal_bytes = runtime->input_block_capacity;
        client->header_transfer = 0;
        multilog(runtime->log, LOG_INFO,
                 "VDIF unpack transfer opened: input=%#x output=%#x "
                 "raw_block=%llu compute_block=%llu group=%llu\n",
                 runtime->config.input_key, runtime->config.output_key,
                 static_cast<unsigned long long>(
                     runtime->input_block_capacity),
                 static_cast<unsigned long long>(
                     runtime->output_block_capacity),
                 static_cast<unsigned long long>(
                     runtime->unpack_layout.group_bytes));
        return 0;
    } catch (const std::exception& exception) {
        SetFailure(runtime, std::string("exception while opening transfer: ") +
                                exception.what());
        EndOutputTransfer(runtime);
        return -1;
    } catch (...) {
        SetFailure(runtime, "unknown exception while opening transfer");
        EndOutputTransfer(runtime);
        return -1;
    }
}

int64_t ProcessBlock(dada_client_t* client, void* data,
                     std::uint64_t data_size, std::uint64_t) {
    WorkerRuntime* runtime =
        static_cast<WorkerRuntime*>(client ? client->context : NULL);
    if (!client || !runtime || runtime->failed || !data) return -1;
    if (data_size > static_cast<std::uint64_t>(
                        std::numeric_limits<int64_t>::max())) {
        SetFailure(runtime, "raw block byte count exceeds int64 callback range");
        return -1;
    }
    try {
        std::string error;
        const unpack::VdifGroupEmitter emitter =
            [runtime](const unpack::VdifGroupKey& key,
                      const std::uint8_t* group, std::uint64_t bytes,
                      std::string* emit_error) {
                return EmitGroup(runtime, key, group, bytes, emit_error);
            };
        if (!runtime->engine.ConsumeRawBlock(
                static_cast<const std::uint8_t*>(data), data_size,
                runtime->raw_block_sequence++, emitter, &error)) {
            SetFailure(runtime, "raw block unpack failed: " + error);
            return -1;
        }
        return static_cast<int64_t>(data_size);
    } catch (const std::exception& exception) {
        SetFailure(runtime, std::string("exception while unpacking block: ") +
                                exception.what());
    } catch (...) {
        SetFailure(runtime, "unknown exception while unpacking block");
    }
    runtime->sink.Abort();
    return -1;
}

int CloseTransferBody(dada_client_t* client) {
    WorkerRuntime* runtime =
        static_cast<WorkerRuntime*>(client ? client->context : NULL);
    if (!runtime) return -1;

    std::string error;
    if (!runtime->failed) {
        const unpack::VdifGroupEmitter emitter =
            [runtime](const unpack::VdifGroupKey& key,
                      const std::uint8_t* group, std::uint64_t bytes,
                      std::string* emit_error) {
                return EmitGroup(runtime, key, group, bytes, emit_error);
            };
        if (!runtime->engine.Finish(emitter, &error)) {
            SetFailure(runtime, "cannot flush VDIF reorder window: " + error);
        } else if (!runtime->writer.Finish(&error)) {
            SetFailure(runtime, "cannot flush final compute block: " + error);
        }
    }

    const unpack::VdifUnpackStatistics& statistics =
        runtime->engine.statistics();
    const double loss_percent =
        statistics.expected_station_packets_for_observed_groups == 0
            ? 0.0
            : 100.0 * static_cast<double>(statistics.missing_station_packets) /
                  static_cast<double>(
                      statistics.expected_station_packets_for_observed_groups);
    multilog(runtime->log, LOG_INFO,
             "VDIF unpack statistics: records=%llu accepted=%llu "
             "bad_header=%llu invalid_data=%llu unknown_station=%llu "
             "duplicate=%llu late=%llu complete_groups=%llu "
             "incomplete_groups=%llu missing_station=%llu/%llu (%.6f%%)\n",
             static_cast<unsigned long long>(statistics.received_records),
             static_cast<unsigned long long>(statistics.accepted_packets),
             static_cast<unsigned long long>(
                 statistics.invalid_header_packets),
             static_cast<unsigned long long>(
                 statistics.invalid_data_packets),
             static_cast<unsigned long long>(
                 statistics.unknown_station_packets),
             static_cast<unsigned long long>(statistics.duplicate_packets),
             static_cast<unsigned long long>(statistics.late_packets),
             static_cast<unsigned long long>(statistics.completed_groups),
             static_cast<unsigned long long>(statistics.incomplete_groups),
             static_cast<unsigned long long>(statistics.missing_station_packets),
             static_cast<unsigned long long>(
                 statistics.expected_station_packets_for_observed_groups),
             loss_percent);

    if (!EndOutputTransfer(runtime)) return -1;
    if (!runtime->failed) {
        multilog(runtime->log, LOG_INFO, "VDIF unpack transfer completed\n");
    }
    return runtime->failed ? -1 : 0;
}

int CloseTransfer(dada_client_t* client, std::uint64_t) {
    WorkerRuntime* runtime =
        static_cast<WorkerRuntime*>(client ? client->context : NULL);
    if (!runtime) return -1;
    try {
        return CloseTransferBody(client);
    } catch (const std::exception& exception) {
        SetFailure(runtime, std::string("exception while closing transfer: ") +
                                exception.what());
    } catch (...) {
        SetFailure(runtime, "unknown exception while closing transfer");
    }
    runtime->sink.Abort();
    EndOutputTransfer(runtime);
    return -1;
}

void PrintUsage(const char* program) {
    std::cerr << "Usage: " << program << " CONFIG.json\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        PrintUsage(argv[0]);
        return EXIT_FAILURE;
    }

    WorkerRuntime runtime;
    rdma_dada::PacketFormatConfig packet_config;
    std::string error;
    if (!unpack::LoadVdifUnpackConfig(argv[1], &runtime.config, &error) ||
        !rdma_dada::LoadPipelineConfig(
            runtime.config.pipeline_config_path,
            &runtime.pipeline_config, &error) ||
        !rdma_dada::LoadPacketFormatConfig(
            runtime.config.packet_format_path, &packet_config, &error) ||
        !rdma_dada::ComputePipelineLayout(
            runtime.pipeline_config, &runtime.pipeline_layout, &error) ||
        !unpack::ComputeVdifUnpackLayout(
            runtime.config, runtime.pipeline_config, packet_config,
            &runtime.unpack_layout, &error)) {
        std::cerr << "vdif_unpack_worker: " << error << '\n';
        return EXIT_FAILURE;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    runtime.log = multilog_open("vdif_unpack_worker", 0);
    if (!runtime.log || multilog_add(runtime.log, stderr) < 0) {
        std::cerr << "vdif_unpack_worker: cannot initialize logging\n";
        if (runtime.log) multilog_close(runtime.log);
        return EXIT_FAILURE;
    }

    dada_hdu_t* input_hdu = dada_hdu_create(runtime.log);
    runtime.output_hdu = dada_hdu_create(runtime.log);
    dada_client_t* client = NULL;
    bool input_connected = false;
    bool output_connected = false;
    bool input_locked = false;
    int result = EXIT_FAILURE;

    if (!input_hdu || !runtime.output_hdu) {
        SetFailure(&runtime, "cannot create DADA HDU handles");
    } else {
        dada_hdu_set_key(input_hdu,
                         static_cast<key_t>(runtime.config.input_key));
        dada_hdu_set_key(runtime.output_hdu,
                         static_cast<key_t>(runtime.config.output_key));
        if (dada_hdu_connect(input_hdu) < 0) {
            SetFailure(&runtime, "cannot connect input DADA ring");
        } else {
            input_connected = true;
        }
        if (!runtime.failed && dada_hdu_connect(runtime.output_hdu) < 0) {
            SetFailure(&runtime, "cannot connect output DADA ring");
        } else if (!runtime.failed) {
            output_connected = true;
        }
        if (!runtime.failed && !LockHduRead(input_hdu)) {
            SetFailure(&runtime, "cannot lock input DADA ring for reading");
        } else if (!runtime.failed) {
            input_locked = true;
        }
    }

    if (!runtime.failed) {
        client = dada_client_create();
        if (!client) {
            SetFailure(&runtime, "cannot create DADA reader client");
        } else {
            client->log = runtime.log;
            client->data_block = input_hdu->data_block;
            client->header_block = input_hdu->header_block;
            client->open_function = OpenTransfer;
            client->io_function = NULL;
            client->io_block_function = ProcessBlock;
            client->close_function = CloseTransfer;
            client->direction = dada_client_reader;
            client->context = &runtime;
            client->header_transfer = 0;
            client->quiet = 1;

            bool keep_running = true;
            while (keep_running && !stop_requested) {
                runtime.failed = false;
                runtime.error.clear();
                if (dada_client_read(client) < 0 || runtime.failed) {
                    if (!runtime.failed) {
                        SetFailure(&runtime,
                                   "PSRDADA input transfer failed");
                    }
                    break;
                }
                if (dada_hdu_unlock_read(input_hdu) < 0) {
                    input_locked = false;
                    SetFailure(&runtime, "cannot unlock input DADA ring");
                    break;
                }
                input_locked = false;
                if (runtime.config.run_once || stop_requested) {
                    keep_running = false;
                    break;
                }
                if (!LockHduRead(input_hdu)) {
                    SetFailure(&runtime, "cannot relock input DADA ring");
                    break;
                }
                input_locked = true;
            }
            if (!runtime.failed) result = EXIT_SUCCESS;
        }
    }

    if (runtime.output_locked) EndOutputTransfer(&runtime);
    if (input_locked && dada_hdu_unlock_read(input_hdu) < 0) {
        result = EXIT_FAILURE;
    }
    if (client) dada_client_destroy(client);
    if (output_connected && dada_hdu_disconnect(runtime.output_hdu) < 0) {
        result = EXIT_FAILURE;
    }
    if (input_connected && dada_hdu_disconnect(input_hdu) < 0) {
        result = EXIT_FAILURE;
    }
    if (runtime.output_hdu) dada_hdu_destroy(runtime.output_hdu);
    if (input_hdu) dada_hdu_destroy(input_hdu);
    multilog_close(runtime.log);
    return result;
}
