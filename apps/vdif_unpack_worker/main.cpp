#include "rdma_dada/config/resolved_plan_json.h"
#include "rdma_dada/modules/vdif_unpack/atfp_block_writer.h"
#include "rdma_dada/modules/vdif_unpack/vdif_atfp_engine.h"
#include "rdma_dada/modules/vdif_unpack/vdif_timeline.h"
#include "rdma_dada/modules/vdif_unpack/vdif_unpack_config.h"
#include "rdma_dada/modules/vdif_unpack/vdif_unpack_header.h"
#include "rdma_dada/pipeline/ascii_metadata.h"

#include <dada_client.h>
#include <dada_hdu.h>
#include <ipcbuf.h>
#include <ipcio.h>
#include <multilog.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <set>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace {

namespace unpack = rdma_dada::modules::vdif_unpack;

volatile std::sig_atomic_t stop_requested = 0;

void HandleSignal(int) { stop_requested = 1; }

bool FitsSizeT(std::uint64_t value) {
    return value <= static_cast<std::uint64_t>(
                        std::numeric_limits<std::size_t>::max());
}

bool ParsePositiveUint64(const char* text, std::uint64_t* value) {
    if (!text || !value || text[0] == '\0' || text[0] == '-') return false;
    errno = 0;
    char* end = NULL;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || parsed == 0U)
        return false;
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

bool ParseThreadCpus(const std::string& text, std::vector<int>* cpus) {
    if (!cpus || text.empty()) return false;
    std::vector<int> parsed;
    std::set<int> unique;
    std::size_t begin = 0U;
    while (begin < text.size()) {
        const std::size_t end = text.find(',', begin);
        const std::string token = text.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        errno = 0;
        char* parse_end = NULL;
        const unsigned long long value =
            std::strtoull(token.c_str(), &parse_end, 10);
        if (token.empty() || token[0] == '-' || errno == ERANGE ||
            parse_end == token.c_str() || *parse_end != '\0' ||
            value > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
            !unique.insert(static_cast<int>(value)).second) {
            return false;
        }
        parsed.push_back(static_cast<int>(value));
        if (end == std::string::npos) break;
        begin = end + 1U;
    }
    if (parsed.size() < 3U) return false;
    cpus->swap(parsed);
    return true;
}

bool BindCurrentThread(int cpu, std::string* error) {
#if defined(__linux__)
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) != 0) {
        if (error) *error = "cannot bind unpack coordinator CPU";
        return false;
    }
#else
    (void)cpu;
    (void)error;
#endif
    return true;
}

std::uint64_t ClockNanoseconds(clockid_t clock_id) {
    struct timespec value = {};
    if (clock_gettime(clock_id, &value) != 0) return 0U;
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
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
          raw_block_sequence(0), output_eod_sent(false), transfers_started(0),
          collect_missing_per_second(false),
          discard_before_timeline_start(false), groups_per_second(0U) {}

    unpack::VdifUnpackConfig config;
    rdma_dada::PipelineConfig pipeline_config;
    rdma_dada::PipelineLayout pipeline_layout;
    unpack::VdifUnpackLayout unpack_layout;
    unpack::VdifTimeline timeline;
    unpack::VdifAtfpUnpackEngine engine;
    unpack::AsyncAtfpBlockWriter writer;
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
    std::uint64_t transfers_started;
    bool collect_missing_per_second;
    bool discard_before_timeline_start;
    std::uint64_t groups_per_second;
    std::vector<int> thread_cpus;
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
    runtime->writer.Abort();
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

bool EmitAtfpBlock(WorkerRuntime* runtime,
                   const unpack::AtfpBlockView& view,
                   std::string* error) {
    return runtime->writer.Enqueue(view, error);
}

bool ConfigureAsyncWriter(WorkerRuntime* runtime, std::string* error) {
    const int writer_cpu = runtime->thread_cpus.empty()
                               ? -1
                               : runtime->thread_cpus.back();
    return runtime->writer.Configure(
        runtime->output_block_capacity, runtime->config.window_blocks,
        writer_cpu, &runtime->sink,
        [runtime](std::uint64_t lease, std::string* release_error) {
            return runtime->engine.ReleasePublishedBlock(lease, release_error);
        },
        error);
}

bool ValidateRingCapacities(WorkerRuntime* runtime, ipcio_t* input_data,
                            std::string* error) {
    runtime->input_block_capacity = ipcbuf_get_bufsz(
        reinterpret_cast<ipcbuf_t*>(input_data));
    runtime->output_block_capacity = ipcbuf_get_bufsz(
        reinterpret_cast<ipcbuf_t*>(runtime->output_hdu->data_block));
    if (runtime->input_block_capacity != runtime->pipeline_layout.raw_block_bytes) {
        std::ostringstream message;
        message << "input ring block capacity must be "
                << runtime->pipeline_layout.raw_block_bytes << " bytes; got "
                << runtime->input_block_capacity;
        if (error) *error = message.str();
        return false;
    }
    if (runtime->output_block_capacity !=
        runtime->unpack_layout.compute_block_bytes) {
        std::ostringstream message;
        message << "output ring block capacity must be "
                << runtime->unpack_layout.compute_block_bytes << " bytes; got "
                << runtime->output_block_capacity;
        if (error) *error = message.str();
        return false;
    }
    return true;
}

bool WriteReadyFile(const std::string& path,
                    const rdma_dada::ResolvedObservationPlan& plan,
                    const WorkerRuntime& runtime,
                    std::uint64_t prepare_duration_ns,
                    std::string* error) {
    if (path.empty()) return true;
    const std::string temporary = path + ".tmp." +
        std::to_string(static_cast<unsigned long long>(getpid()));
    std::ofstream output(temporary.c_str(), std::ios::out | std::ios::trunc);
    if (!output) {
        if (error) *error = "cannot open worker ready temporary file";
        return false;
    }
    output << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"pid\": " << static_cast<long long>(getpid()) << ",\n"
           << "  \"config_id\": \"" << plan.config_id << "\",\n"
           << "  \"geometry_id\": \"" << plan.geometry_id << "\",\n"
           << "  \"prepared_unix_ns\": "
           << ClockNanoseconds(CLOCK_REALTIME) << ",\n"
           << "  \"prepare_duration_ns\": " << prepare_duration_ns << ",\n"
           << "  \"window_bytes\": "
           << runtime.engine.prepared_window_bytes() << ",\n"
           << "  \"window_groups\": "
           << runtime.unpack_layout.window_capacity_groups << ",\n"
           << "  \"raw_record_bytes\": "
           << runtime.unpack_layout.raw_record_bytes << ",\n"
           << "  \"records_per_raw_block\": "
           << runtime.unpack_layout.records_per_raw_block << ",\n"
           << "  \"raw_block_bytes\": "
           << runtime.pipeline_layout.raw_block_bytes << ",\n"
           << "  \"compute_block_bytes\": "
           << runtime.unpack_layout.compute_block_bytes << ",\n"
           << "  \"thread_cpus\": [";
    for (std::size_t index = 0; index < runtime.thread_cpus.size(); ++index) {
        if (index != 0U) output << ", ";
        output << runtime.thread_cpus[index];
    }
    output << "]\n"
           << "}\n";
    output.close();
    if (!output) {
        std::remove(temporary.c_str());
        if (error) *error = "cannot write worker ready temporary file";
        return false;
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        if (error) *error = "cannot publish worker ready file";
        return false;
    }
    return true;
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

        if (!ValidateRingCapacities(runtime, client->data_block, &error)) {
            SetFailure(runtime, error);
            return -1;
        }

        if (!unpack::ParseVdifTimeline(
                input_header, runtime->pipeline_config,
                &runtime->timeline, &error)) {
            SetFailure(runtime, "cannot parse VDIF observation timeline: " +
                                    error);
            return -1;
        }
        runtime->timeline.groups_per_second = runtime->groups_per_second;

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

        if (!runtime->engine.BeginTransfer(
                runtime->timeline, runtime->collect_missing_per_second,
                runtime->discard_before_timeline_start,
                &error)) {
            SetFailure(runtime, "cannot begin VDIF unpack transfer: " + error);
            return -1;
        }
        if (runtime->transfers_started != 0U &&
            !ConfigureAsyncWriter(runtime, &error)) {
            SetFailure(runtime, "cannot configure compute block writer: " + error);
            return -1;
        }
        ++runtime->transfers_started;

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
                 "raw_block=%llu compute_block=%llu group=%llu "
                 "expected_groups=%llu order=ATFP\n",
                 runtime->config.input_key, runtime->config.output_key,
                 static_cast<unsigned long long>(
                     runtime->input_block_capacity),
                 static_cast<unsigned long long>(
                     runtime->output_block_capacity),
                 static_cast<unsigned long long>(
                     runtime->unpack_layout.group_bytes),
                 static_cast<unsigned long long>(
                     runtime->timeline.expected_groups));
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
        const unpack::VdifAtfpBlockEmitter emitter =
            [runtime](const unpack::AtfpBlockView& view,
                      std::string* emit_error) {
                return EmitAtfpBlock(runtime, view, emit_error);
            };
        if (!runtime->engine.ConsumeRawBlockAsync(
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
    runtime->writer.Abort();
    runtime->sink.Abort();
    return -1;
}

int CloseTransferBody(dada_client_t* client) {
    WorkerRuntime* runtime =
        static_cast<WorkerRuntime*>(client ? client->context : NULL);
    if (!runtime) return -1;

    std::string error;
    if (!runtime->failed) {
        const unpack::VdifAtfpBlockEmitter emitter =
            [runtime](const unpack::AtfpBlockView& view,
                      std::string* emit_error) {
                return EmitAtfpBlock(runtime, view, emit_error);
            };
        if (!runtime->engine.FinishAsync(emitter, &error)) {
            SetFailure(runtime, "cannot flush VDIF reorder window: " + error);
        } else if (!runtime->writer.Finish(&error)) {
            SetFailure(runtime, "cannot flush final compute block: " + error);
        }
    }

    const unpack::VdifAtfpStatistics& statistics =
        runtime->engine.statistics();
    const unpack::AtfpBlockWriterStatistics& writer_statistics =
        runtime->writer.writer_statistics();
    const unpack::AsyncAtfpBlockWriterStatistics& async_statistics =
        runtime->writer.statistics();
    const double loss_percent =
        statistics.expected_station_packets == 0
            ? 0.0
            : 100.0 * static_cast<double>(statistics.missing_station_packets) /
                  static_cast<double>(statistics.expected_station_packets);
    multilog(runtime->log, LOG_INFO,
             "VDIF unpack statistics: records=%llu accepted=%llu "
             "bad_header=%llu invalid_data=%llu unknown_station=%llu "
             "duplicate=%llu late=%llu out_of_range=%llu "
             "complete_groups=%llu incomplete_groups=%llu "
             "fully_missing_groups=%llu missing_station=%llu/%llu "
             "(%.6f%%) large_gap_advances=%llu/%llu "
             "max_station_ordinal_skew=%llu raw_blocks_single=%llu "
             "raw_blocks_mixed=%llu max_station_records_per_raw_block=%llu "
             "max_consecutive_station_records=%llu "
             "payload_copies=%llu/%llu "
             "emitted_blocks=%llu emitted_bytes=%llu "
             "writer_acquire=%llu commit=%llu blocks=%llu bytes=%llu "
             "acquire_wait_ns=%llu writer_queue_hwm=%llu "
             "writer_enqueue_wait_ns=%llu\n",
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
             static_cast<unsigned long long>(statistics.out_of_range_packets),
             static_cast<unsigned long long>(statistics.completed_groups),
             static_cast<unsigned long long>(statistics.incomplete_groups),
             static_cast<unsigned long long>(statistics.fully_missing_groups),
             static_cast<unsigned long long>(statistics.missing_station_packets),
             static_cast<unsigned long long>(statistics.expected_station_packets),
             loss_percent,
             static_cast<unsigned long long>(statistics.large_gap_advances),
             static_cast<unsigned long long>(
                 statistics.large_gap_advanced_groups),
             static_cast<unsigned long long>(
                 statistics.max_station_ordinal_skew),
             static_cast<unsigned long long>(
                 statistics.single_station_raw_blocks),
             static_cast<unsigned long long>(
                 statistics.mixed_station_raw_blocks),
             static_cast<unsigned long long>(
                 statistics.max_station_records_per_raw_block),
             static_cast<unsigned long long>(
                 statistics.max_consecutive_station_records),
             static_cast<unsigned long long>(statistics.payload_copy_calls),
             static_cast<unsigned long long>(statistics.payload_copy_bytes),
             static_cast<unsigned long long>(statistics.emitted_blocks),
             static_cast<unsigned long long>(statistics.emitted_bytes),
             static_cast<unsigned long long>(writer_statistics.acquire_calls),
             static_cast<unsigned long long>(writer_statistics.commit_calls),
             static_cast<unsigned long long>(writer_statistics.committed_blocks),
             static_cast<unsigned long long>(writer_statistics.committed_bytes),
             static_cast<unsigned long long>(writer_statistics.acquire_wait_ns),
             static_cast<unsigned long long>(
                 async_statistics.queue_high_watermark),
             static_cast<unsigned long long>(
                 async_statistics.enqueue_wait_ns));

    for (std::size_t antenna = 0;
         antenna < runtime->config.antenna_map.size(); ++antenna) {
        multilog(runtime->log, LOG_INFO,
                 "VDIF unpack station statistics: antenna=%llu station=%u "
                 "observed=%llu accepted=%llu late=%llu "
                 "highest_ordinal=%llu\n",
                 static_cast<unsigned long long>(antenna),
                 static_cast<unsigned int>(
                     runtime->config.antenna_map[antenna]),
                 static_cast<unsigned long long>(
                     statistics.station_observed_packets[antenna]),
                 static_cast<unsigned long long>(
                     statistics.station_accepted_packets[antenna]),
                 static_cast<unsigned long long>(
                     statistics.station_late_packets[antenna]),
                 static_cast<unsigned long long>(
                     statistics.station_highest_ordinals[antenna]));
    }

    if (runtime->collect_missing_per_second) {
        for (std::size_t second_index = 0;
             second_index <
                 statistics.missing_station_packets_per_second.size();
             ++second_index) {
            multilog(runtime->log, LOG_INFO,
                     "VDIF missing per second: second_index=%llu "
                     "vdif_seconds=%llu missing=%llu\n",
                     static_cast<unsigned long long>(second_index),
                     static_cast<unsigned long long>(
                         runtime->timeline.start_seconds + second_index),
                     static_cast<unsigned long long>(
                         statistics.missing_station_packets_per_second[
                             second_index]));
        }
    }

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
    runtime->writer.Abort();
    runtime->sink.Abort();
    EndOutputTransfer(runtime);
    return -1;
}

void PrintUsage(const char* program) {
    std::cerr << "Usage: " << program
              << " --plan resolved_observation.json"
              << " [--reorder-horizon-groups GROUPS]"
              << " [--ready-file PATH]"
              << " [--diagnostics missing-per-second]"
              << " [--pre-timeline-policy discard]"
              << " [--groups-per-second GROUPS]"
              << " [--thread-cpus COORDINATOR,WORKER...,WRITER]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string plan_path;
    std::string ready_path;
    std::uint64_t reorder_horizon_groups = 0U;
    bool collect_missing_per_second = false;
    bool discard_before_timeline_start = false;
    std::uint64_t groups_per_second = 0U;
    std::vector<int> thread_cpus;
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc) {
            PrintUsage(argv[0]);
            return EXIT_FAILURE;
        }
        const std::string option(argv[index]);
        const std::string value(argv[index + 1]);
        if (option == "--plan" && plan_path.empty()) {
            plan_path = value;
        } else if (option == "--ready-file" && ready_path.empty() &&
                   !value.empty()) {
            ready_path = value;
        } else if (option == "--reorder-horizon-groups" &&
                   reorder_horizon_groups == 0U &&
                   ParsePositiveUint64(value.c_str(),
                                       &reorder_horizon_groups)) {
        } else if (option == "--diagnostics" &&
                   value == "missing-per-second" &&
                   !collect_missing_per_second) {
            collect_missing_per_second = true;
        } else if (option == "--pre-timeline-policy" &&
                   value == "discard" &&
                   !discard_before_timeline_start) {
            discard_before_timeline_start = true;
        } else if (option == "--groups-per-second" &&
                   groups_per_second == 0U &&
                   ParsePositiveUint64(value.c_str(), &groups_per_second)) {
        } else if (option == "--thread-cpus" && thread_cpus.empty() &&
                   ParseThreadCpus(value, &thread_cpus)) {
        } else {
            PrintUsage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (plan_path.empty()) {
        PrintUsage(argv[0]);
        return EXIT_FAILURE;
    }

    WorkerRuntime runtime;
    rdma_dada::ResolvedObservationPlan resolved_plan;
    std::string error;
    if (!rdma_dada::LoadResolvedObservationPlan(
            plan_path, &resolved_plan, &error) ||
        !unpack::BuildVdifUnpackRuntimeFromResolvedPlan(
            resolved_plan, &runtime.config, &runtime.pipeline_config,
            &runtime.pipeline_layout, &runtime.unpack_layout, &error)) {
        std::cerr << "vdif_unpack_worker: " << error << '\n';
        return EXIT_FAILURE;
    }
    if (reorder_horizon_groups != 0U)
        runtime.config.reorder_horizon_groups = reorder_horizon_groups;
    runtime.collect_missing_per_second = collect_missing_per_second;
    runtime.discard_before_timeline_start = discard_before_timeline_start;
    runtime.groups_per_second = groups_per_second;
    runtime.thread_cpus = thread_cpus;

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
        if (!runtime.failed && !ValidateRingCapacities(
                &runtime, input_hdu->data_block, &error))
            SetFailure(&runtime, error);
        const std::uint64_t prepare_start = ClockNanoseconds(CLOCK_MONOTONIC_RAW);
        if (!runtime.failed && !runtime.thread_cpus.empty() &&
            (!BindCurrentThread(runtime.thread_cpus.front(), &error) ||
             !runtime.engine.ConfigureThreadCpus(runtime.thread_cpus,
                                                 &error)))
            SetFailure(&runtime, error);
        if (!runtime.failed && !runtime.engine.Prepare(
                runtime.config, runtime.pipeline_config,
                runtime.unpack_layout, &error))
            SetFailure(&runtime, "cannot prepare VDIF unpack engine: " + error);
        if (!runtime.failed) {
            runtime.sink.Configure(runtime.output_hdu->data_block,
                                   runtime.output_block_capacity);
            if (!ConfigureAsyncWriter(&runtime, &error))
                SetFailure(&runtime,
                           "cannot configure compute block writer: " + error);
        }
        const std::uint64_t prepare_end = ClockNanoseconds(CLOCK_MONOTONIC_RAW);
        const std::uint64_t prepare_duration =
            prepare_end >= prepare_start ? prepare_end - prepare_start : 0U;
        if (!runtime.failed && !WriteReadyFile(
                ready_path, resolved_plan, runtime, prepare_duration, &error))
            SetFailure(&runtime, error);
        if (!runtime.failed && !LockHduRead(input_hdu))
            SetFailure(&runtime, "cannot lock input DADA ring for reading");
        else if (!runtime.failed)
            input_locked = true;
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
    runtime.writer.Abort();
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
