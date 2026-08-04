#include "rdma_dada/modules/device_to_host/device_to_host_module.h"
#include "rdma_dada/modules/host_to_device/host_to_device_module.h"
#include "rdma_dada/pipeline/ascii_metadata.h"
#include "rdma_dada/pipeline/module_chain.h"
#include "rdma_dada/pipeline/worker_config.h"

#include <dada_client.h>
#include <dada_hdu.h>
#include <ipcbuf.h>
#include <ipcio.h>
#include <multilog.h>

#if defined(RDMA_DADA_HAVE_CUDA)
#include <cuda_runtime_api.h>
#endif

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

volatile std::sig_atomic_t stop_requested = 0;

void HandleSignal(int) { stop_requested = 1; }

struct WorkerRuntime {
    WorkerRuntime()
        : log(NULL), output_hdu(NULL), output_locked(false), failed(false),
          input_block_capacity(0), scratch_block_capacity(0),
          output_block_capacity(0), output_block_open(false),
          input_ring_location(rdma_dada::pipeline::MemoryLocation::kHost)
#if defined(RDMA_DADA_HAVE_CUDA)
          , stream(NULL), device_input(NULL), device_scratch(NULL),
          device_output(NULL)
#endif
    {}

    rdma_dada::pipeline::WorkerConfig config;
    rdma_dada::pipeline::WorkerBlockGeometry geometry;
    rdma_dada::pipeline::ModuleChain chain;
    rdma_dada::modules::host_to_device::HostToDeviceModule h2d;
    rdma_dada::modules::device_to_host::DeviceToHostModule d2h;
    multilog_t* log;
    dada_hdu_t* output_hdu;
    bool output_locked;
    bool failed;
    std::string error;
    std::uint64_t input_block_capacity;
    std::uint64_t scratch_block_capacity;
    std::uint64_t output_block_capacity;
    bool output_block_open;
    rdma_dada::pipeline::MemoryLocation input_ring_location;
    std::vector<std::uint8_t> host_scratch;
#if defined(RDMA_DADA_HAVE_CUDA)
    cudaStream_t stream;
    std::uint8_t* device_input;
    std::uint8_t* device_scratch;
    std::uint8_t* device_output;
#endif
};

void SetFailure(WorkerRuntime* runtime, const std::string& message) {
    if (!runtime) return;
    runtime->failed = true;
    runtime->error = message;
    if (runtime->log) {
        multilog(runtime->log, LOG_ERR, "%s\n", message.c_str());
    }
}

bool FitsSizeT(std::uint64_t value) {
    return value <= static_cast<std::uint64_t>(
                        std::numeric_limits<std::size_t>::max());
}

#if defined(RDMA_DADA_HAVE_CUDA)
bool CheckCuda(WorkerRuntime* runtime, cudaError_t result,
               const std::string& operation) {
    if (result == cudaSuccess) return true;
    SetFailure(runtime, operation + ": " + cudaGetErrorString(result));
    return false;
}
#endif

void ReleaseExecutionBuffers(WorkerRuntime* runtime) {
    if (!runtime) return;
#if defined(RDMA_DADA_HAVE_CUDA)
    if (runtime->stream) cudaStreamSynchronize(runtime->stream);
    if (runtime->device_input) cudaFree(runtime->device_input);
    if (runtime->device_scratch) cudaFree(runtime->device_scratch);
    if (runtime->device_output) cudaFree(runtime->device_output);
    runtime->device_input = NULL;
    runtime->device_scratch = NULL;
    runtime->device_output = NULL;
    if (runtime->stream) cudaStreamDestroy(runtime->stream);
    runtime->stream = NULL;
#endif
    runtime->host_scratch.clear();
}

void AbortOpenTransfer(WorkerRuntime* runtime) {
    if (!runtime) return;
    runtime->h2d.Finish();
    runtime->d2h.Finish();
    runtime->chain.Finish();
    ReleaseExecutionBuffers(runtime);
    if (runtime->output_locked) {
        if (dada_hdu_unlock_write(runtime->output_hdu) < 0 &&
            !runtime->failed) {
            SetFailure(runtime, "failed to unlock output ring after open error");
        }
        runtime->output_locked = false;
    }
}

bool PrepareExecutionBuffers(WorkerRuntime* runtime) {
    if (!FitsSizeT(runtime->input_block_capacity) ||
        !FitsSizeT(runtime->scratch_block_capacity) ||
        !FitsSizeT(runtime->output_block_capacity)) {
        SetFailure(runtime, "ring block capacity exceeds addressable size_t");
        return false;
    }
    if (runtime->config.execution_backend == "CPU_REFERENCE") {
        if (runtime->scratch_block_capacity > 0U) {
            runtime->host_scratch.resize(
                static_cast<std::size_t>(
                    runtime->scratch_block_capacity));
        }
        return true;
    }

#if defined(RDMA_DADA_HAVE_CUDA)
    if (!CheckCuda(runtime, cudaSetDevice(runtime->config.cuda_device),
                   "cudaSetDevice")) {
        return false;
    }
    if (!CheckCuda(runtime,
                   cudaStreamCreateWithFlags(
                       &runtime->stream, cudaStreamNonBlocking),
                   "cudaStreamCreateWithFlags")) {
        return false;
    }
    if (!CheckCuda(runtime,
                   cudaMalloc(reinterpret_cast<void**>(&runtime->device_input),
                              static_cast<std::size_t>(
                                  runtime->input_block_capacity)),
                   "cudaMalloc input block")) {
        return false;
    }
    if (runtime->scratch_block_capacity > 0U &&
        !CheckCuda(runtime,
                   cudaMalloc(
                       reinterpret_cast<void**>(
                           &runtime->device_scratch),
                       static_cast<std::size_t>(
                           runtime->scratch_block_capacity)),
                   "cudaMalloc module-chain scratch block")) {
        return false;
    }
    if (!CheckCuda(runtime,
                   cudaMalloc(reinterpret_cast<void**>(&runtime->device_output),
                              static_cast<std::size_t>(
                                  runtime->output_block_capacity)),
                   "cudaMalloc output block")) {
        return false;
    }
    return true;
#else
    SetFailure(runtime,
               "execution.backend=CUDA requires pipeline_worker built with "
               "USE_CUDA=ON");
    return false;
#endif
}

int OpenTransfer(dada_client_t* client) {
    WorkerRuntime* runtime =
        static_cast<WorkerRuntime*>(client ? client->context : NULL);
    if (!client || !runtime) return -1;
    runtime->failed = false;
    runtime->error.clear();
    runtime->output_block_open = false;

    try {
        rdma_dada::pipeline::Metadata input_header;
        std::string error;
        if (!rdma_dada::pipeline::ParseAsciiMetadata(
                client->header, client->header_size, &input_header, &error)) {
            SetFailure(runtime, "cannot parse input DADA header: " + error);
            return -1;
        }

        rdma_dada::pipeline::Metadata output_header;
        const rdma_dada::pipeline::StageStatus configure_status =
            runtime->chain.Configure(
                input_header, runtime->config, &output_header);
        if (!configure_status.ok()) {
            SetFailure(runtime,
                       "cannot configure module chain: " +
                           configure_status.message());
            AbortOpenTransfer(runtime);
            return -1;
        }

        if (runtime->config.execution_backend == "CUDA") {
            rdma_dada::pipeline::StageParameters transfer_parameters;
            transfer_parameters.SetString("EXECUTION_BACKEND", "CUDA");
            transfer_parameters.SetUint64(
                "CUDA_DEVICE",
                static_cast<std::uint64_t>(runtime->config.cuda_device));

            rdma_dada::pipeline::Metadata device_input_header;
            rdma_dada::pipeline::StageStatus transfer_status =
                runtime->h2d.ConfigureHeader(
                    input_header, transfer_parameters,
                    &device_input_header);
            if (!transfer_status.ok()) {
                SetFailure(runtime,
                           "cannot configure H2D module: " +
                               transfer_status.message());
                AbortOpenTransfer(runtime);
                return -1;
            }
            if (device_input_header.Fields() !=
                runtime->chain.plan().input_header.Fields()) {
                SetFailure(runtime,
                           "H2D output header does not match module-chain "
                           "input header");
                AbortOpenTransfer(runtime);
                return -1;
            }

            rdma_dada::pipeline::Metadata device_output_header =
                output_header;
            device_output_header.SetString("MEMORY", "CUDA_DEVICE");
            device_output_header.SetUint64(
                "CUDA_DEVICE",
                static_cast<std::uint64_t>(runtime->config.cuda_device));
            transfer_parameters.SetString("OUTPUT_MEMORY", "HOST");
            transfer_status = runtime->d2h.ConfigureHeader(
                device_output_header, transfer_parameters, &output_header);
            if (!transfer_status.ok()) {
                SetFailure(runtime,
                           "cannot configure D2H module: " +
                               transfer_status.message());
                AbortOpenTransfer(runtime);
                return -1;
            }

            std::string input_memory;
            input_header.GetString("MEMORY", &input_memory);
            runtime->input_ring_location =
                input_memory == "PINNED_HOST" ?
                    rdma_dada::pipeline::MemoryLocation::kPinnedHost :
                    rdma_dada::pipeline::MemoryLocation::kHost;
        } else {
            runtime->input_ring_location =
                rdma_dada::pipeline::MemoryLocation::kHost;
        }

        runtime->input_block_capacity = ipcbuf_get_bufsz(
            reinterpret_cast<ipcbuf_t*>(client->data_block));
        runtime->output_block_capacity = ipcbuf_get_bufsz(
            reinterpret_cast<ipcbuf_t*>(runtime->output_hdu->data_block));
        if (runtime->input_block_capacity !=
            runtime->geometry.input_block_bytes) {
            std::ostringstream message;
            message << "input ring block capacity must be "
                    << runtime->geometry.input_block_bytes
                    << " bytes from configured F*A*P*T geometry; got "
                    << runtime->input_block_capacity;
            SetFailure(runtime, message.str());
            AbortOpenTransfer(runtime);
            return -1;
        }
        std::uint64_t expected_output_capacity = 0;
        rdma_dada::pipeline::StageStatus plan_status =
            runtime->chain.PlanBlock(
                runtime->input_block_capacity,
                &runtime->scratch_block_capacity,
                &expected_output_capacity);
        if (!plan_status.ok()) {
            SetFailure(runtime,
                       "invalid input ring block geometry: " +
                           plan_status.message());
            AbortOpenTransfer(runtime);
            return -1;
        }
        if (expected_output_capacity != runtime->output_block_capacity) {
            std::ostringstream message;
            message << "output ring block capacity must be "
                    << expected_output_capacity << " bytes for input block "
                    << runtime->input_block_capacity << " bytes; got "
                    << runtime->output_block_capacity;
            SetFailure(runtime, message.str());
            AbortOpenTransfer(runtime);
            return -1;
        }
        if (runtime->scratch_block_capacity !=
                runtime->geometry.scratch_block_bytes ||
            expected_output_capacity != runtime->geometry.output_block_bytes) {
            SetFailure(runtime,
                       "module chain block plan does not match configured "
                       "F/A/P/T geometry");
            AbortOpenTransfer(runtime);
            return -1;
        }

        std::uint64_t transfer_size = 0;
        if (input_header.Has("TRANSFER_SIZE")) {
            if (!input_header.GetUint64("TRANSFER_SIZE", &transfer_size)) {
                SetFailure(runtime, "input TRANSFER_SIZE is invalid");
                AbortOpenTransfer(runtime);
                return -1;
            }
            if (transfer_size != 0) {
                std::uint64_t transfer_scratch_bytes = 0;
                std::uint64_t transfer_output_bytes = 0;
                plan_status = runtime->chain.PlanBlock(
                    transfer_size, &transfer_scratch_bytes,
                    &transfer_output_bytes);
                if (!plan_status.ok()) {
                    SetFailure(runtime,
                               "input TRANSFER_SIZE is incompatible with the "
                               "module chain: " + plan_status.message());
                    AbortOpenTransfer(runtime);
                    return -1;
                }
            }
        }

        output_header.SetUint64(
            "INPUT_BLOCK_BYTES", runtime->input_block_capacity);
        output_header.SetUint64(
            "OUTPUT_BLOCK_BYTES", runtime->output_block_capacity);
        const std::uint64_t output_header_capacity = ipcbuf_get_bufsz(
            runtime->output_hdu->header_block);
        if (!FitsSizeT(output_header_capacity)) {
            SetFailure(runtime,
                       "output header block capacity exceeds size_t");
            AbortOpenTransfer(runtime);
            return -1;
        }
        std::vector<char> serialized_header(
            static_cast<std::size_t>(output_header_capacity));
        if (!rdma_dada::pipeline::SerializeAsciiMetadata(
                output_header, &serialized_header[0], output_header_capacity,
                &error)) {
            SetFailure(runtime, "cannot serialize output DADA header: " + error);
            AbortOpenTransfer(runtime);
            return -1;
        }

        if (!PrepareExecutionBuffers(runtime)) {
            AbortOpenTransfer(runtime);
            return -1;
        }
        if (dada_hdu_lock_write(runtime->output_hdu) < 0) {
            SetFailure(runtime, "cannot lock output DADA ring for writing");
            AbortOpenTransfer(runtime);
            return -1;
        }
        runtime->output_locked = true;

        char* output_header_block =
            ipcbuf_get_next_write(runtime->output_hdu->header_block);
        if (!output_header_block) {
            SetFailure(runtime, "cannot acquire output DADA header block");
            AbortOpenTransfer(runtime);
            return -1;
        }
        std::copy(serialized_header.begin(), serialized_header.end(),
                  output_header_block);
        if (ipcbuf_enable_eod(runtime->output_hdu->header_block) < 0 ||
            ipcbuf_mark_filled(runtime->output_hdu->header_block,
                               output_header_capacity) < 0) {
            SetFailure(runtime, "cannot publish output DADA header block");
            AbortOpenTransfer(runtime);
            return -1;
        }

        client->transfer_bytes = transfer_size;
        client->optimal_bytes = runtime->input_block_capacity;
        client->header_transfer = 0;
        multilog(runtime->log, LOG_INFO,
                 "pipeline transfer opened: input=%s output=%s product=%s "
                 "T=%llu input_block=%llu output_block=%llu\n",
                 runtime->config.input_key_text.c_str(),
                 runtime->config.output_key_text.c_str(),
                 rdma_dada::pipeline::WorkerProductName(
                     runtime->config.product),
                 static_cast<unsigned long long>(runtime->geometry.ntime),
                 static_cast<unsigned long long>(
                     runtime->input_block_capacity),
                 static_cast<unsigned long long>(
                     runtime->output_block_capacity));
        return 0;
    } catch (const std::exception& exception) {
        SetFailure(runtime,
                   std::string("exception while opening transfer: ") +
                       exception.what());
        AbortOpenTransfer(runtime);
        return -1;
    }
}

int64_t ProcessBlock(dada_client_t* client, void* data,
                     std::uint64_t data_size, std::uint64_t block_id) {
    WorkerRuntime* runtime =
        static_cast<WorkerRuntime*>(client ? client->context : NULL);
    if (!client || !runtime || runtime->failed || !data) return -1;

    std::uint64_t scratch_bytes = 0;
    std::uint64_t output_bytes = 0;
    rdma_dada::pipeline::StageStatus status = runtime->chain.PlanBlock(
        data_size, &scratch_bytes, &output_bytes);
    if (!status.ok() || output_bytes > runtime->output_block_capacity) {
        SetFailure(runtime,
                   status.ok() ? "planned output exceeds output ring block" :
                                 status.message());
        return -1;
    }

    std::uint64_t output_block_id = 0;
    char* output_data = ipcio_open_block_write(
        runtime->output_hdu->data_block, &output_block_id);
    if (!output_data) {
        SetFailure(runtime, "cannot acquire output DADA data block");
        return -1;
    }
    runtime->output_block_open = true;

    const rdma_dada::pipeline::BlockExecutionContext host_context = {
        rdma_dada::pipeline::ExecutionBackend::kHost, -1, NULL
    };
    if (runtime->config.execution_backend == "CPU_REFERENCE") {
        const rdma_dada::pipeline::InputBlock input = {
            static_cast<const std::uint8_t*>(data), data_size, block_id,
            rdma_dada::pipeline::MemoryLocation::kHost
        };
        rdma_dada::pipeline::OutputBlock output = {
            reinterpret_cast<std::uint8_t*>(output_data),
            runtime->output_block_capacity, 0, block_id,
            rdma_dada::pipeline::MemoryLocation::kHost
        };
        std::uint8_t* scratch = runtime->host_scratch.empty() ?
            NULL : &runtime->host_scratch[0];
        status = runtime->chain.ProcessBlock(
            input, &output, scratch, runtime->host_scratch.size(),
            host_context);
        if (status.ok() && output.size != output_bytes) {
            status = rdma_dada::pipeline::StageStatus::Error(
                "module chain produced an unexpected output byte count");
        }
    } else {
#if defined(RDMA_DADA_HAVE_CUDA)
        const rdma_dada::pipeline::BlockExecutionContext cuda_context = {
            rdma_dada::pipeline::ExecutionBackend::kCuda,
            runtime->config.cuda_device,
            reinterpret_cast<void*>(runtime->stream)
        };
        const rdma_dada::pipeline::InputBlock host_input = {
            static_cast<const std::uint8_t*>(data), data_size, block_id,
            runtime->input_ring_location
        };
        rdma_dada::pipeline::OutputBlock device_input = {
            runtime->device_input, runtime->input_block_capacity,
            0, 0, rdma_dada::pipeline::MemoryLocation::kCudaDevice
        };
        status = runtime->h2d.ProcessBlock(
            host_input, &device_input, cuda_context);
        if (status.ok()) {
            const rdma_dada::pipeline::InputBlock input = {
                device_input.data, device_input.size, device_input.sequence,
                rdma_dada::pipeline::MemoryLocation::kCudaDevice
            };
            rdma_dada::pipeline::OutputBlock output = {
                runtime->device_output, runtime->output_block_capacity,
                0, block_id,
                rdma_dada::pipeline::MemoryLocation::kCudaDevice
            };
            status = runtime->chain.ProcessBlock(
                input, &output, runtime->device_scratch,
                scratch_bytes, cuda_context);
            if (status.ok() && output.size != output_bytes) {
                status = rdma_dada::pipeline::StageStatus::Error(
                    "module chain produced an unexpected output byte count");
            }
            if (status.ok()) {
                const rdma_dada::pipeline::InputBlock device_result = {
                    output.data, output.size, output.sequence,
                    rdma_dada::pipeline::MemoryLocation::kCudaDevice
                };
                rdma_dada::pipeline::OutputBlock host_result = {
                    reinterpret_cast<std::uint8_t*>(output_data),
                    runtime->output_block_capacity, 0, 0,
                    rdma_dada::pipeline::MemoryLocation::kHost
                };
                status = runtime->d2h.ProcessBlock(
                    device_result, &host_result, cuda_context);
                if (status.ok() && host_result.size != output_bytes) {
                    status = rdma_dada::pipeline::StageStatus::Error(
                        "D2H produced an unexpected output byte count");
                }
            }
        }
        if (!CheckCuda(runtime, cudaStreamSynchronize(runtime->stream),
                       "cudaStreamSynchronize")) {
            status = status.ok() ?
                rdma_dada::pipeline::StageStatus::Error(runtime->error) :
                rdma_dada::pipeline::StageStatus::Error(
                    status.message() + "; " + runtime->error);
        }
#else
        status = rdma_dada::pipeline::StageStatus::Error(
            "CUDA backend is unavailable in this build");
#endif
    }

    if (!status.ok()) {
        SetFailure(runtime, "block processing failed: " + status.message());
        if (ipcio_close_block_write(
                runtime->output_hdu->data_block, 0) >= 0) {
            runtime->output_block_open = false;
        }
        return -1;
    }
    if (ipcio_close_block_write(
            runtime->output_hdu->data_block, output_bytes) < 0) {
        SetFailure(runtime, "cannot commit output DADA data block");
        return -1;
    }
    runtime->output_block_open = false;
    return static_cast<int64_t>(data_size);
}

int CloseTransfer(dada_client_t* client, std::uint64_t) {
    WorkerRuntime* runtime =
        static_cast<WorkerRuntime*>(client ? client->context : NULL);
    if (!runtime) return -1;
    bool cleanup_failed = false;
    if (runtime->output_block_open) {
        if (ipcio_close_block_write(
                runtime->output_hdu->data_block, 0) < 0) {
            cleanup_failed = true;
        }
        runtime->output_block_open = false;
    }
#if defined(RDMA_DADA_HAVE_CUDA)
    if (runtime->stream &&
        cudaStreamSynchronize(runtime->stream) != cudaSuccess) {
        cleanup_failed = true;
    }
#endif
    const rdma_dada::pipeline::StageStatus finish_status =
        runtime->chain.Finish();
    if (!finish_status.ok()) {
        SetFailure(runtime,
                   "module chain finish failed: " + finish_status.message());
        cleanup_failed = true;
    }
    const rdma_dada::pipeline::StageStatus h2d_finish_status =
        runtime->h2d.Finish();
    if (!h2d_finish_status.ok()) {
        SetFailure(runtime,
                   "H2D finish failed: " + h2d_finish_status.message());
        cleanup_failed = true;
    }
    const rdma_dada::pipeline::StageStatus d2h_finish_status =
        runtime->d2h.Finish();
    if (!d2h_finish_status.ok()) {
        SetFailure(runtime,
                   "D2H finish failed: " + d2h_finish_status.message());
        cleanup_failed = true;
    }
    ReleaseExecutionBuffers(runtime);
    if (runtime->output_locked) {
        if (dada_hdu_unlock_write(runtime->output_hdu) < 0) {
            SetFailure(runtime, "cannot unlock output DADA ring");
            cleanup_failed = true;
        }
        runtime->output_locked = false;
    }
    if (!runtime->failed && !cleanup_failed) {
        multilog(runtime->log, LOG_INFO, "pipeline transfer completed\n");
    }
    return runtime->failed || cleanup_failed ? -1 : 0;
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
    std::string config_error;
    if (!rdma_dada::pipeline::LoadWorkerConfig(
            argv[1], &runtime.config, &config_error) ||
        !rdma_dada::pipeline::ComputeWorkerBlockGeometry(
            runtime.config, &runtime.geometry, &config_error)) {
        std::cerr << "pipeline_worker: " << config_error << '\n';
        return EXIT_FAILURE;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    runtime.log = multilog_open("pipeline_worker", 0);
    if (!runtime.log || multilog_add(runtime.log, stderr) < 0) {
        std::cerr << "pipeline_worker: cannot initialize logging\n";
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
        dada_hdu_set_key(
            input_hdu, static_cast<key_t>(runtime.config.input_key));
        dada_hdu_set_key(
            runtime.output_hdu,
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
        if (!runtime.failed && dada_hdu_lock_read(input_hdu) < 0) {
            SetFailure(&runtime, "cannot lock input DADA ring for reading");
        } else if (!runtime.failed) {
            input_locked = true;
        }
    }

    if (!runtime.failed) {
        client = dada_client_create();
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
                    SetFailure(&runtime, "PSRDADA input transfer failed");
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
            if (dada_hdu_lock_read(input_hdu) < 0) {
                SetFailure(&runtime, "cannot relock input DADA ring");
                break;
            }
            input_locked = true;
        }
        if (!runtime.failed) result = EXIT_SUCCESS;
    }

    if (runtime.output_locked) AbortOpenTransfer(&runtime);
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
