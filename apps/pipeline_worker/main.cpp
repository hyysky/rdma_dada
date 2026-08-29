#include "rdma_dada/modules/device_to_host/device_to_host_module.h"
#include "rdma_dada/modules/host_to_device/host_to_device_module.h"
#include "rdma_dada/modules/complex_convert/complex_convert_module.h"
#include "rdma_dada/config/observation_artifacts.h"
#include "rdma_dada/config/resolved_plan_json.h"
#include "rdma_dada/pipeline/ascii_metadata.h"
#include "rdma_dada/pipeline/gpu_block_pipeline.h"
#include "rdma_dada/pipeline/module_chain.h"
#include "rdma_dada/pipeline/worker_config.h"
#include "rdma_dada/pipeline/worker_metrics.h"

#include <dada_client.h>
#include <dada_hdu.h>
#include <ipcbuf.h>
#include <ipcio.h>
#include <multilog.h>

#if defined(RDMA_DADA_HAVE_CUDA)
#include <cuda_runtime_api.h>
#include <dada_cuda.h>
#endif

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void HandleSignal(int) { stop_requested = 1; }

struct WorkerRuntime {
    WorkerRuntime()
        : log(NULL), output_hdu(NULL), output_locked(false), failed(false),
          input_block_capacity(0), converted_block_capacity(0),
          scratch_block_capacity(0),
          output_block_capacity(0), output_block_open(false),
          input_ring_location(rdma_dada::pipeline::MemoryLocation::kHost),
          metrics_transfer_started(false), metrics_finalized(false),
          staged_pipeline_active(false),
          next_input_sequence(0U), input_cuda_registered(false),
          registered_ring_blocks(0U), registered_ring_bytes(0U),
          input_ring_registration_ns(0U)
#if defined(RDMA_DADA_HAVE_CUDA)
          , stream(NULL), device_input(NULL), device_converted(NULL),
          device_scratch(NULL), device_output(NULL), event_start(NULL),
          event_h2d(NULL), event_algorithm(NULL), event_d2h(NULL)
#endif
    {}

    rdma_dada::pipeline::WorkerConfig config;
    rdma_dada::pipeline::WorkerBlockGeometry geometry;
    rdma_dada::ResolvedObservationPlan plan;
    rdma_dada::pipeline::Metadata expected_input_header;
    rdma_dada::pipeline::Metadata expected_output_header;
    rdma_dada::pipeline::ModuleChain chain;
    rdma_dada::modules::complex_convert::ComplexConvertModule conversion;
    rdma_dada::modules::host_to_device::HostToDeviceModule h2d;
    rdma_dada::modules::device_to_host::DeviceToHostModule d2h;
    multilog_t* log;
    dada_hdu_t* output_hdu;
    bool output_locked;
    bool failed;
    std::string error;
    std::uint64_t input_block_capacity;
    std::uint64_t converted_block_capacity;
    std::uint64_t scratch_block_capacity;
    std::uint64_t output_block_capacity;
    bool output_block_open;
    rdma_dada::pipeline::MemoryLocation input_ring_location;
    std::string metrics_path;
    rdma_dada::pipeline::WorkerMetrics metrics;
    rdma_dada::pipeline::GpuBlockPipeline gpu_pipeline;
    std::chrono::steady_clock::time_point metrics_transfer_start;
    bool metrics_transfer_started;
    bool metrics_finalized;
    bool staged_pipeline_active;
    std::uint64_t next_input_sequence;
    bool input_cuda_registered;
    std::uint64_t registered_ring_blocks;
    std::uint64_t registered_ring_bytes;
    std::uint64_t input_ring_registration_ns;
    std::vector<std::uint8_t> host_scratch;
    std::vector<std::uint8_t> host_converted;
#if defined(RDMA_DADA_HAVE_CUDA)
    cudaStream_t stream;
    std::uint8_t* device_input;
    std::uint8_t* device_converted;
    std::uint8_t* device_scratch;
    std::uint8_t* device_output;
    cudaEvent_t event_start;
    cudaEvent_t event_h2d;
    cudaEvent_t event_algorithm;
    cudaEvent_t event_d2h;
#endif
};

typedef std::chrono::steady_clock WorkerClock;

std::uint64_t ElapsedNs(const WorkerClock::time_point& start,
                        const WorkerClock::time_point& finish) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start)
            .count());
}

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

rdma_dada::pipeline::StageStatus AcquireOutputBlock(
    WorkerRuntime* runtime, std::uint64_t,
    std::uint8_t** data, std::uint64_t* capacity) {
    if (!runtime || !data || !capacity) {
        return rdma_dada::pipeline::StageStatus::Error(
            "output block acquisition argument is null");
    }
    if (runtime->output_block_open) {
        return rdma_dada::pipeline::StageStatus::Error(
            "an output DADA block is already open");
    }
    std::uint64_t block_id = 0U;
    char* block = ipcio_open_block_write(
        runtime->output_hdu->data_block, &block_id);
    if (!block) {
        return rdma_dada::pipeline::StageStatus::Error(
            "cannot acquire output DADA data block");
    }
    runtime->output_block_open = true;
    *data = reinterpret_cast<std::uint8_t*>(block);
    *capacity = runtime->output_block_capacity;
    return rdma_dada::pipeline::StageStatus::Ok();
}

rdma_dada::pipeline::StageStatus CommitOutputBlock(
    WorkerRuntime* runtime, std::uint64_t, std::uint64_t bytes) {
    if (!runtime || !runtime->output_block_open) {
        return rdma_dada::pipeline::StageStatus::Error(
            "no output DADA block is open for commit");
    }
    if (ipcio_close_block_write(runtime->output_hdu->data_block, bytes) < 0) {
        return rdma_dada::pipeline::StageStatus::Error(
            "cannot commit output DADA data block");
    }
    runtime->output_block_open = false;
    return rdma_dada::pipeline::StageStatus::Ok();
}

rdma_dada::pipeline::StageStatus AbortOutputBlock(
    WorkerRuntime* runtime, std::uint64_t) {
    if (!runtime || !runtime->output_block_open) {
        return rdma_dada::pipeline::StageStatus::Ok();
    }
    if (ipcio_close_block_write(runtime->output_hdu->data_block, 0U) < 0) {
        return rdma_dada::pipeline::StageStatus::Error(
            "cannot abort output DADA data block");
    }
    runtime->output_block_open = false;
    return rdma_dada::pipeline::StageStatus::Ok();
}

bool ValidateInputHeader(const rdma_dada::pipeline::Metadata& header,
                         const WorkerRuntime& runtime,
                         std::string* error) {
    struct TextExpectation { const char* name; const char* value; };
    const TextExpectation text_fields[] = {
        {"DATA_STAGE", "UNPACKED"}, {"ORDER", "ATFP"},
        {"LAYOUT_SCOPE", "BLOCK"}, {"SAMPLE_FORMAT", "CI8"},
        {"SAMPLE_ENCODING", "TWOS_COMPLEMENT"},
        {"COMPONENT_ORDER", "IQ"}, {"ENDIAN", "LITTLE"}
    };
    for (std::size_t index = 0;
         index < sizeof(text_fields) / sizeof(text_fields[0]); ++index) {
        std::string actual;
        if (!header.GetString(text_fields[index].name, &actual) ||
            actual != text_fields[index].value) {
            *error = std::string("input DADA header ") +
                text_fields[index].name + " must be " +
                text_fields[index].value;
            return false;
        }
    }
    std::string memory;
    if (!header.GetString("MEMORY", &memory) ||
        (memory != "HOST" && memory != "PINNED_HOST")) {
        *error = "input DADA header MEMORY must be HOST or PINNED_HOST";
        return false;
    }
    struct UintExpectation { const char* name; std::uint64_t value; };
    const UintExpectation uint_fields[] = {
        {"COMPONENT_NBIT", 8U}, {"SAMPLE_NBIT", 16U},
        {"NCHAN", runtime.plan.source.nchan},
        {"NPOL", runtime.plan.source.npol}, {"NANT", runtime.plan.nant},
        {"BLOCK_NTIME", runtime.plan.samples_per_block},
        {"RESOLUTION", runtime.geometry.input_frame_bytes},
        {"RECORD_BYTES", runtime.plan.compute_block_bytes},
        {"OUTPUT_BLOCK_BYTES", runtime.plan.compute_block_bytes},
        {"BLOCK_BYTES", runtime.plan.compute_block_bytes},
        {"RING_BYTES", runtime.plan.compute_ring_bytes}
    };
    for (std::size_t index = 0;
         index < sizeof(uint_fields) / sizeof(uint_fields[0]); ++index) {
        std::uint64_t actual = 0U;
        if (!header.GetUint64(uint_fields[index].name, &actual) ||
            actual != uint_fields[index].value) {
            std::ostringstream message;
            message << "input DADA header " << uint_fields[index].name
                    << " must be " << uint_fields[index].value;
            *error = message.str();
            return false;
        }
    }
    return true;
}

bool ValidateExactMetadataContract(
    const rdma_dada::pipeline::Metadata& actual,
    const rdma_dada::pipeline::Metadata& expected,
    const char* contract_name,
    std::string* error) {
    const std::map<std::string, std::string>& actual_fields = actual.Fields();
    const std::map<std::string, std::string>& expected_fields =
        expected.Fields();
    if (actual_fields == expected_fields) return true;
    for (std::map<std::string, std::string>::const_iterator field =
             expected_fields.begin(); field != expected_fields.end(); ++field) {
        const std::map<std::string, std::string>::const_iterator found =
            actual_fields.find(field->first);
        if (found == actual_fields.end() || found->second != field->second) {
            std::ostringstream message;
            message << contract_name << " header field " << field->first
                    << " must be " << field->second << "; got ";
            if (found == actual_fields.end()) message << "<missing>";
            else message << found->second;
            *error = message.str();
            return false;
        }
    }
    for (std::map<std::string, std::string>::const_iterator field =
             actual_fields.begin(); field != actual_fields.end(); ++field) {
        if (expected_fields.count(field->first) == 0U) {
            *error = std::string(contract_name) +
                " header has unexpected field " + field->first;
            return false;
        }
    }
    *error = std::string(contract_name) + " header metadata mismatch";
    return false;
}

bool ValidateConnectedRings(WorkerRuntime* runtime, dada_hdu_t* input_hdu) {
    const std::uint64_t input_block_bytes = ipcbuf_get_bufsz(
        reinterpret_cast<ipcbuf_t*>(input_hdu->data_block));
    const std::uint64_t output_block_bytes = ipcbuf_get_bufsz(
        reinterpret_cast<ipcbuf_t*>(runtime->output_hdu->data_block));
    const std::uint64_t input_blocks = ipcbuf_get_nbufs(
        reinterpret_cast<ipcbuf_t*>(input_hdu->data_block));
    const std::uint64_t output_blocks = ipcbuf_get_nbufs(
        reinterpret_cast<ipcbuf_t*>(runtime->output_hdu->data_block));
    const std::uint64_t input_header_bytes =
        ipcbuf_get_bufsz(input_hdu->header_block);
    const std::uint64_t output_header_bytes =
        ipcbuf_get_bufsz(runtime->output_hdu->header_block);
    if (input_block_bytes != runtime->plan.compute_block_bytes ||
        input_blocks != runtime->plan.source.compute_ring_blocks ||
        input_header_bytes != 4096U) {
        SetFailure(runtime,
                   "connected input ring geometry does not match resolved "
                   "compute ring");
        return false;
    }
    if (output_block_bytes != runtime->plan.output_block_bytes ||
        output_blocks != runtime->plan.source.compute_ring_blocks ||
        output_header_bytes != 4096U) {
        SetFailure(runtime,
                   "connected output ring geometry does not match resolved "
                   "output ring");
        return false;
    }
    return true;
}

#if defined(RDMA_DADA_HAVE_CUDA)
bool CheckCuda(WorkerRuntime* runtime, cudaError_t result,
               const std::string& operation) {
    if (result == cudaSuccess) return true;
    SetFailure(runtime, operation + ": " + cudaGetErrorString(result));
    return false;
}

bool RegisterInputRingWithCuda(WorkerRuntime* runtime,
                               dada_hdu_t* input_hdu) {
    if (!runtime || !input_hdu ||
        runtime->config.execution_backend != "CUDA") {
        return true;
    }
    if (!CheckCuda(runtime, cudaSetDevice(runtime->config.cuda_device),
                   "cudaSetDevice before input ring registration")) {
        return false;
    }
    std::uint64_t block_count = 0U;
    std::uint64_t block_bytes = 0U;
    if (!dada_hdu_db_addresses(input_hdu, &block_count, &block_bytes) ||
        block_count == 0U || block_bytes == 0U ||
        block_count > std::numeric_limits<std::uint64_t>::max() /
                          block_bytes) {
        SetFailure(runtime, "cannot inspect input DADA ring for CUDA registration");
        return false;
    }
    const WorkerClock::time_point start = WorkerClock::now();
    if (dada_cuda_dbregister(input_hdu) < 0) {
        SetFailure(runtime, "cannot register input DADA ring with CUDA");
        return false;
    }
    runtime->input_cuda_registered = true;
    runtime->registered_ring_blocks = block_count;
    runtime->registered_ring_bytes = block_count * block_bytes;
    runtime->input_ring_registration_ns =
        ElapsedNs(start, WorkerClock::now());
    multilog(runtime->log, LOG_INFO,
             "registered input DADA ring with CUDA: blocks=%llu bytes=%llu "
             "registration_ns=%llu\n",
             static_cast<unsigned long long>(block_count),
             static_cast<unsigned long long>(runtime->registered_ring_bytes),
             static_cast<unsigned long long>(
                 runtime->input_ring_registration_ns));
    return true;
}

bool UnregisterInputRingFromCuda(WorkerRuntime* runtime,
                                 dada_hdu_t* input_hdu) {
    if (!runtime || !input_hdu || !runtime->input_cuda_registered) return true;
    bool ok = CheckCuda(runtime, cudaSetDevice(runtime->config.cuda_device),
                        "cudaSetDevice before input ring unregistration");
    if (ok) {
        ok = CheckCuda(runtime, cudaDeviceSynchronize(),
                       "cudaDeviceSynchronize before input ring unregistration");
    }
    if (ok && dada_cuda_dbunregister(input_hdu) < 0) {
        SetFailure(runtime, "cannot unregister input DADA ring from CUDA");
        ok = false;
    }
    if (ok) runtime->input_cuda_registered = false;
    return ok;
}
#endif

void ReleaseExecutionBuffers(WorkerRuntime* runtime) {
    if (!runtime) return;
#if defined(RDMA_DADA_HAVE_CUDA)
    if (runtime->stream) cudaStreamSynchronize(runtime->stream);
    if (runtime->device_input) cudaFree(runtime->device_input);
    if (runtime->device_converted) cudaFree(runtime->device_converted);
    if (runtime->device_scratch) cudaFree(runtime->device_scratch);
    if (runtime->device_output) cudaFree(runtime->device_output);
    runtime->device_input = NULL;
    runtime->device_converted = NULL;
    runtime->device_scratch = NULL;
    runtime->device_output = NULL;
    if (runtime->event_start) cudaEventDestroy(runtime->event_start);
    if (runtime->event_h2d) cudaEventDestroy(runtime->event_h2d);
    if (runtime->event_algorithm) cudaEventDestroy(runtime->event_algorithm);
    if (runtime->event_d2h) cudaEventDestroy(runtime->event_d2h);
    runtime->event_start = NULL;
    runtime->event_h2d = NULL;
    runtime->event_algorithm = NULL;
    runtime->event_d2h = NULL;
    if (runtime->stream) cudaStreamDestroy(runtime->stream);
    runtime->stream = NULL;
#endif
    runtime->host_scratch.clear();
    runtime->host_converted.clear();
}

void AbortOpenTransfer(WorkerRuntime* runtime) {
    if (!runtime) return;
    if (runtime->staged_pipeline_active) {
        (void)runtime->gpu_pipeline.Abort(
            runtime->error.empty() ? "transfer open aborted" : runtime->error);
    }
    (void)runtime->gpu_pipeline.Finish();
    runtime->staged_pipeline_active = false;
    runtime->h2d.Finish();
    runtime->d2h.Finish();
    runtime->conversion.Finish();
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
        !FitsSizeT(runtime->converted_block_capacity) ||
        !FitsSizeT(runtime->scratch_block_capacity) ||
        !FitsSizeT(runtime->output_block_capacity)) {
        SetFailure(runtime, "ring block capacity exceeds addressable size_t");
        return false;
    }
    if (runtime->config.execution_backend == "CPU_REFERENCE") {
        runtime->host_converted.resize(
            static_cast<std::size_t>(runtime->converted_block_capacity));
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
    if (!CheckCuda(runtime, cudaEventCreate(&runtime->event_start),
                   "cudaEventCreate start") ||
        !CheckCuda(runtime, cudaEventCreate(&runtime->event_h2d),
                   "cudaEventCreate H2D") ||
        !CheckCuda(runtime, cudaEventCreate(&runtime->event_algorithm),
                   "cudaEventCreate algorithm") ||
        !CheckCuda(runtime, cudaEventCreate(&runtime->event_d2h),
                   "cudaEventCreate D2H")) {
        return false;
    }
    if (!CheckCuda(runtime,
                   cudaMalloc(reinterpret_cast<void**>(&runtime->device_input),
                              static_cast<std::size_t>(
                                  runtime->input_block_capacity)),
                   "cudaMalloc input block")) {
        return false;
    }
    if (!CheckCuda(runtime,
                   cudaMalloc(
                       reinterpret_cast<void**>(&runtime->device_converted),
                       static_cast<std::size_t>(
                           runtime->converted_block_capacity)),
                   "cudaMalloc converted TFPA block")) {
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
    runtime->staged_pipeline_active = false;
    runtime->next_input_sequence = 0U;
    runtime->metrics.Reset();
    // Keep fallback metrics truthful when transfer setup fails before the
    // staged executor owns the authoritative metrics object.  Zero planned
    // byte counts mean setup did not reach the allocation/configuration gate;
    // they must not make the requested execution mode look like direct/1.
    runtime->metrics.ConfigureExecution(
        rdma_dada::CudaPipelineModeName(
            runtime->config.cuda_pipeline_mode),
        runtime->config.cuda_inflight_blocks, 0U, 0U);
    if (runtime->input_cuda_registered) {
        runtime->metrics.RecordInputRingRegistration(
            runtime->registered_ring_blocks,
            runtime->registered_ring_bytes,
            runtime->input_ring_registration_ns);
    }
    runtime->metrics_transfer_start = WorkerClock::now();
    runtime->metrics_transfer_started = true;
    runtime->metrics_finalized = false;

    try {
        rdma_dada::pipeline::Metadata input_header;
        std::string error;
        if (!rdma_dada::pipeline::ParseAsciiMetadata(
                client->header, client->header_size, &input_header, &error)) {
            SetFailure(runtime, "cannot parse input DADA header: " + error);
            return -1;
        }
        std::string input_config_id;
        std::string input_geometry_id;
        if (!input_header.GetString("CONFIG_ID", &input_config_id) ||
            input_config_id != runtime->plan.config_id ||
            !input_header.GetString("GEOMETRY_ID", &input_geometry_id) ||
            input_geometry_id != runtime->plan.geometry_id) {
            SetFailure(runtime,
                       "input DADA header CONFIG_ID/GEOMETRY_ID does not "
                       "match resolved observation plan");
            return -1;
        }
        if (!ValidateInputHeader(input_header, *runtime, &error)) {
            SetFailure(runtime, error);
            return -1;
        }
        if (!ValidateExactMetadataContract(
                input_header, runtime->expected_input_header,
                "input", &error)) {
            SetFailure(runtime, error);
            return -1;
        }

        rdma_dada::pipeline::StageParameters algorithm_parameters;
        algorithm_parameters.SetString(
            "EXECUTION_BACKEND", runtime->config.execution_backend);
        algorithm_parameters.SetDouble(
            "CONVERSION_SCALE", runtime->config.conversion_scale);
        rdma_dada::pipeline::StageParameters transfer_parameters;
        rdma_dada::pipeline::Metadata conversion_input_header = input_header;
        if (runtime->config.execution_backend == "CUDA") {
            transfer_parameters.SetString("EXECUTION_BACKEND", "CUDA");
            transfer_parameters.SetUint64(
                "CUDA_DEVICE",
                static_cast<std::uint64_t>(runtime->config.cuda_device));
            algorithm_parameters.SetUint64(
                "CUDA_DEVICE",
                static_cast<std::uint64_t>(runtime->config.cuda_device));

            rdma_dada::pipeline::StageStatus transfer_status =
                runtime->h2d.ConfigureHeader(
                    input_header, transfer_parameters,
                    &conversion_input_header);
            if (!transfer_status.ok()) {
                SetFailure(runtime,
                           "cannot configure H2D module: " +
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

        rdma_dada::pipeline::Metadata converted_header;
        rdma_dada::pipeline::StageStatus configure_status =
            runtime->conversion.ConfigureHeader(
                conversion_input_header, algorithm_parameters,
                &converted_header);
        if (!configure_status.ok()) {
            SetFailure(runtime,
                       "cannot configure complex conversion: " +
                           configure_status.message());
            AbortOpenTransfer(runtime);
            return -1;
        }
        if (runtime->config.execution_backend == "CPU_REFERENCE") {
            converted_header.SetString("MEMORY", "HOST");
            converted_header.Erase("CUDA_DEVICE");
        }

        rdma_dada::pipeline::Metadata output_header;
        configure_status = runtime->chain.Configure(
            converted_header, runtime->config, &output_header);
        if (!configure_status.ok()) {
            SetFailure(runtime,
                       "cannot configure module chain: " +
                           configure_status.message());
            AbortOpenTransfer(runtime);
            return -1;
        }
        std::string pipeline_modules;
        if (output_header.GetString("PIPELINE_MODULES", &pipeline_modules)) {
            output_header.SetString(
                "PIPELINE_MODULES", "complex_convert," + pipeline_modules);
        }

        if (runtime->config.execution_backend == "CUDA") {
            rdma_dada::pipeline::Metadata device_output_header =
                output_header;
            device_output_header.SetString("MEMORY", "CUDA_DEVICE");
            device_output_header.SetUint64(
                "CUDA_DEVICE",
                static_cast<std::uint64_t>(runtime->config.cuda_device));
            transfer_parameters.SetString("OUTPUT_MEMORY", "HOST");
            const rdma_dada::pipeline::StageStatus transfer_status =
                runtime->d2h.ConfigureHeader(
                    device_output_header, transfer_parameters, &output_header);
            if (!transfer_status.ok()) {
                SetFailure(runtime,
                           "cannot configure D2H module: " +
                               transfer_status.message());
                AbortOpenTransfer(runtime);
                return -1;
            }
        }
        output_header.SetUint64(
            "INPUT_BLOCK_BYTES", runtime->geometry.input_block_bytes);
        output_header.SetUint64(
            "CONVERTED_BLOCK_BYTES",
            runtime->geometry.converted_block_bytes);
        output_header.SetDouble(
            "CONVERSION_SCALE", runtime->config.conversion_scale);

        runtime->input_block_capacity = ipcbuf_get_bufsz(
            reinterpret_cast<ipcbuf_t*>(client->data_block));
        runtime->converted_block_capacity =
            runtime->geometry.converted_block_bytes;
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
                runtime->converted_block_capacity,
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
        if (runtime->output_block_capacity !=
                runtime->plan.output_block_bytes ||
            runtime->plan.output_ring_bytes !=
                runtime->plan.output_block_bytes *
                    runtime->plan.source.compute_ring_blocks) {
            SetFailure(runtime,
                       "output ring capacity does not match resolved plan");
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

        if (runtime->config.execution_backend == "CUDA" &&
            runtime->config.cuda_pipeline_mode ==
                rdma_dada::CudaPipelineMode::kStagedPipeline) {
            rdma_dada::pipeline::OutputBlockFunctions output_functions;
            output_functions.acquire =
                [runtime](std::uint64_t sequence, std::uint8_t** output_data,
                          std::uint64_t* capacity) {
                    return AcquireOutputBlock(
                        runtime, sequence, output_data, capacity);
                };
            output_functions.commit =
                [runtime](std::uint64_t sequence, std::uint64_t bytes) {
                    return CommitOutputBlock(runtime, sequence, bytes);
                };
            output_functions.abort =
                [runtime](std::uint64_t sequence) {
                    return AbortOutputBlock(runtime, sequence);
                };
            rdma_dada::pipeline::Metadata staged_output_header;
            const rdma_dada::pipeline::StageStatus staged_status =
                runtime->gpu_pipeline.Configure(
                    runtime->config, runtime->geometry, input_header,
                    output_functions, &staged_output_header);
            if (!staged_status.ok()) {
                SetFailure(runtime,
                           "cannot configure staged GPU pipeline: " +
                               staged_status.message());
                AbortOpenTransfer(runtime);
                return -1;
            }
            output_header = staged_output_header;
            runtime->staged_pipeline_active = true;
            runtime->gpu_pipeline.RecordInputRingRegistration(
                runtime->registered_ring_blocks,
                runtime->registered_ring_bytes,
                runtime->input_ring_registration_ns);
        }

        std::uint64_t transfer_size = 0;
        if (input_header.Has("TRANSFER_SIZE")) {
            if (!input_header.GetUint64("TRANSFER_SIZE", &transfer_size)) {
                SetFailure(runtime, "input TRANSFER_SIZE is invalid");
                AbortOpenTransfer(runtime);
                return -1;
            }
            if (transfer_size != 0) {
                if (transfer_size % runtime->geometry.input_frame_bytes != 0) {
                    SetFailure(runtime,
                               "input TRANSFER_SIZE is not aligned to an "
                               "ATFP CI8 time frame");
                    AbortOpenTransfer(runtime);
                    return -1;
                }
                std::uint64_t transfer_scratch_bytes = 0;
                std::uint64_t transfer_output_bytes = 0;
                if (runtime->staged_pipeline_active) {
                    plan_status = runtime->gpu_pipeline.PlanBlock(
                        transfer_size, &transfer_scratch_bytes,
                        &transfer_output_bytes);
                } else {
                    const std::uint64_t transfer_ntime =
                        transfer_size / runtime->geometry.input_frame_bytes;
                    if (transfer_ntime >
                        std::numeric_limits<std::uint64_t>::max() /
                            runtime->geometry.converted_frame_bytes) {
                        SetFailure(runtime,
                                   "converted TRANSFER_SIZE overflows uint64");
                        AbortOpenTransfer(runtime);
                        return -1;
                    }
                    const std::uint64_t converted_transfer_size =
                        transfer_ntime *
                            runtime->geometry.converted_frame_bytes;
                    plan_status = runtime->chain.PlanBlock(
                        converted_transfer_size, &transfer_scratch_bytes,
                        &transfer_output_bytes);
                }
                if (!plan_status.ok()) {
                    SetFailure(runtime,
                               "input TRANSFER_SIZE is incompatible with the "
                               "module chain: " + plan_status.message());
                    AbortOpenTransfer(runtime);
                    return -1;
                }
            }
        }

        if (runtime->staged_pipeline_active) {
            // The staged executor owns its own configured module instances.
            // Release the temporary direct-path instances used above for
            // common header/geometry validation so weights and CUDA backend
            // state are not duplicated for the transfer lifetime.
            const rdma_dada::pipeline::StageStatus old_chain =
                runtime->chain.Finish();
            const rdma_dada::pipeline::StageStatus old_conversion =
                runtime->conversion.Finish();
            const rdma_dada::pipeline::StageStatus old_h2d =
                runtime->h2d.Finish();
            const rdma_dada::pipeline::StageStatus old_d2h =
                runtime->d2h.Finish();
            if (!old_chain.ok() || !old_conversion.ok() ||
                !old_h2d.ok() || !old_d2h.ok()) {
                SetFailure(runtime,
                           "cannot release temporary direct GPU modules");
                AbortOpenTransfer(runtime);
                return -1;
            }
        }

        output_header.SetUint64(
            "INPUT_BLOCK_BYTES", runtime->input_block_capacity);
        output_header.SetUint64(
            "OUTPUT_BLOCK_BYTES", runtime->output_block_capacity);
        output_header.SetUint64("BLOCK_BYTES", runtime->output_block_capacity);
        output_header.SetUint64("RING_BYTES", runtime->plan.output_ring_bytes);
        if (!ValidateExactMetadataContract(
                output_header, runtime->expected_output_header,
                "output", &error)) {
            SetFailure(runtime, error);
            AbortOpenTransfer(runtime);
            return -1;
        }
        std::string output_stage;
        std::string output_order;
        std::string output_format;
        if (!output_header.GetString("DATA_STAGE", &output_stage) ||
            output_stage != runtime->plan.output_data_stage ||
            !output_header.GetString("ORDER", &output_order) ||
            output_order != runtime->plan.output_order ||
            !output_header.GetString("SAMPLE_FORMAT", &output_format) ||
            output_format != runtime->plan.output_sample_format) {
            SetFailure(runtime,
                       "computed output DADA header does not match resolved "
                       "output contract");
            AbortOpenTransfer(runtime);
            return -1;
        }
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

        if (!runtime->staged_pipeline_active &&
            !PrepareExecutionBuffers(runtime)) {
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

        // TRANSFER_SIZE is a strict observation contract and is used above to
        // validate/plan the output geometry.  It must not be used as the
        // PSRDADA reader stop limit: when a finite transfer ends on an exact
        // full-block boundary, the consumer can reach that byte count before
        // the producer publishes EOD.  dada_client_read then opens a spurious
        // continuation transfer with an incremented OBS_OFFSET.  Reading to
        // ring EOD keeps the complete finite observation in one transfer and
        // matches the upstream unpack worker's publication contract.
        client->transfer_bytes = 0U;
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
    const WorkerClock::time_point service_start = WorkerClock::now();

    std::uint64_t scratch_bytes = 0;
    std::uint64_t output_bytes = 0;
    double h2d_ms_recorded = 0.0;
    double algorithm_ms_recorded = 0.0;
    double d2h_ms_recorded = 0.0;
    if (data_size == 0 ||
        data_size % runtime->geometry.input_frame_bytes != 0) {
        SetFailure(runtime,
                   "input block does not contain complete ATFP CI8 frames");
        return -1;
    }
    if (runtime->staged_pipeline_active) {
        rdma_dada::pipeline::StageStatus status =
            runtime->gpu_pipeline.SubmitBlock(
            runtime->next_input_sequence,
            static_cast<const std::uint8_t*>(data), data_size);
        if (!status.ok()) {
            SetFailure(runtime,
                       "staged GPU block processing failed: " +
                           status.message());
            (void)runtime->gpu_pipeline.Abort(status.message());
            return -1;
        }
        ++runtime->next_input_sequence;
        return static_cast<int64_t>(data_size);
    }

    const std::uint64_t actual_ntime =
        data_size / runtime->geometry.input_frame_bytes;
    if (actual_ntime >
        std::numeric_limits<std::uint64_t>::max() /
            runtime->geometry.converted_frame_bytes) {
        SetFailure(runtime, "converted block byte count overflows uint64");
        return -1;
    }
    const std::uint64_t converted_bytes =
        actual_ntime * runtime->geometry.converted_frame_bytes;
    rdma_dada::pipeline::StageStatus status = runtime->chain.PlanBlock(
        converted_bytes, &scratch_bytes, &output_bytes);
    if (!status.ok() || output_bytes > runtime->output_block_capacity) {
        SetFailure(runtime,
                   status.ok() ? "planned output exceeds output ring block" :
                                 status.message());
        return -1;
    }

    std::uint64_t output_block_id = 0;
    const WorkerClock::time_point output_wait_start = WorkerClock::now();
    char* output_data = ipcio_open_block_write(
        runtime->output_hdu->data_block, &output_block_id);
    const std::uint64_t output_wait_ns = ElapsedNs(
        output_wait_start, WorkerClock::now());
    if (!output_data) {
        SetFailure(runtime, "cannot acquire output DADA data block");
        return -1;
    }
    runtime->output_block_open = true;

    const rdma_dada::pipeline::BlockExecutionContext host_context = {
        rdma_dada::pipeline::ExecutionBackend::kHost, -1, NULL
    };
    if (runtime->config.execution_backend == "CPU_REFERENCE") {
        const rdma_dada::pipeline::InputBlock integer_input = {
            static_cast<const std::uint8_t*>(data), data_size, block_id,
            rdma_dada::pipeline::MemoryLocation::kHost
        };
        rdma_dada::pipeline::OutputBlock converted_output = {
            &runtime->host_converted[0], runtime->host_converted.size(),
            0, 0, rdma_dada::pipeline::MemoryLocation::kHost
        };
        status = runtime->conversion.ProcessBlock(
            integer_input, &converted_output, host_context);
        const rdma_dada::pipeline::InputBlock input = {
            converted_output.data, converted_output.size,
            converted_output.sequence,
            rdma_dada::pipeline::MemoryLocation::kHost
        };
        rdma_dada::pipeline::OutputBlock output = {
            reinterpret_cast<std::uint8_t*>(output_data),
            runtime->output_block_capacity, 0, block_id,
            rdma_dada::pipeline::MemoryLocation::kHost
        };
        std::uint8_t* scratch = runtime->host_scratch.empty() ?
            NULL : &runtime->host_scratch[0];
        if (status.ok()) {
            status = runtime->chain.ProcessBlock(
                input, &output, scratch, runtime->host_scratch.size(),
                host_context);
        }
        if (status.ok() && output.size != output_bytes) {
            status = rdma_dada::pipeline::StageStatus::Error(
                "module chain produced an unexpected output byte count");
        }
    } else {
#if defined(RDMA_DADA_HAVE_CUDA)
        bool gpu_timing_ready = CheckCuda(
            runtime,
            cudaEventRecord(runtime->event_start, runtime->stream),
            "cudaEventRecord start");
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
        status = gpu_timing_ready ? runtime->h2d.ProcessBlock(
            host_input, &device_input, cuda_context) :
            rdma_dada::pipeline::StageStatus::Error(runtime->error);
        if (status.ok() && gpu_timing_ready) {
            gpu_timing_ready = CheckCuda(
                runtime,
                cudaEventRecord(runtime->event_h2d, runtime->stream),
                "cudaEventRecord H2D");
        }
        if (status.ok()) {
            const rdma_dada::pipeline::InputBlock integer_input = {
                device_input.data, device_input.size, device_input.sequence,
                rdma_dada::pipeline::MemoryLocation::kCudaDevice
            };
            rdma_dada::pipeline::OutputBlock converted_output = {
                runtime->device_converted,
                runtime->converted_block_capacity, 0, 0,
                rdma_dada::pipeline::MemoryLocation::kCudaDevice
            };
            status = runtime->conversion.ProcessBlock(
                integer_input, &converted_output, cuda_context);
            const rdma_dada::pipeline::InputBlock input = {
                converted_output.data, converted_output.size,
                converted_output.sequence,
                rdma_dada::pipeline::MemoryLocation::kCudaDevice
            };
            rdma_dada::pipeline::OutputBlock output = {
                runtime->device_output, runtime->output_block_capacity,
                0, block_id,
                rdma_dada::pipeline::MemoryLocation::kCudaDevice
            };
            if (status.ok()) {
                status = runtime->chain.ProcessBlock(
                    input, &output, runtime->device_scratch,
                    scratch_bytes, cuda_context);
            }
            if (status.ok() && output.size != output_bytes) {
                status = rdma_dada::pipeline::StageStatus::Error(
                    "module chain produced an unexpected output byte count");
            }
            if (status.ok() && gpu_timing_ready) {
                gpu_timing_ready = CheckCuda(
                    runtime,
                    cudaEventRecord(runtime->event_algorithm,
                                    runtime->stream),
                    "cudaEventRecord algorithm");
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
                if (status.ok() && gpu_timing_ready) {
                    gpu_timing_ready = CheckCuda(
                        runtime,
                        cudaEventRecord(runtime->event_d2h,
                                        runtime->stream),
                        "cudaEventRecord D2H");
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
        if (status.ok() && gpu_timing_ready) {
            float h2d_ms = 0.0F;
            float algorithm_ms = 0.0F;
            float d2h_ms = 0.0F;
            if (!CheckCuda(runtime,
                           cudaEventElapsedTime(
                               &h2d_ms, runtime->event_start,
                               runtime->event_h2d),
                           "cudaEventElapsedTime H2D") ||
                !CheckCuda(runtime,
                           cudaEventElapsedTime(
                               &algorithm_ms, runtime->event_h2d,
                               runtime->event_algorithm),
                           "cudaEventElapsedTime algorithm") ||
                !CheckCuda(runtime,
                           cudaEventElapsedTime(
                               &d2h_ms, runtime->event_algorithm,
                               runtime->event_d2h),
                           "cudaEventElapsedTime D2H")) {
                status = rdma_dada::pipeline::StageStatus::Error(
                    runtime->error);
            } else {
                h2d_ms_recorded = h2d_ms;
                algorithm_ms_recorded = algorithm_ms;
                d2h_ms_recorded = d2h_ms;
            }
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
    const std::uint64_t service_ns = ElapsedNs(service_start, WorkerClock::now());
    runtime->metrics.RecordBlock(
        data_size, output_bytes, service_ns, output_wait_ns,
        h2d_ms_recorded, algorithm_ms_recorded, d2h_ms_recorded);
    return static_cast<int64_t>(data_size);
}

int CloseTransfer(dada_client_t* client, std::uint64_t) {
    WorkerRuntime* runtime =
        static_cast<WorkerRuntime*>(client ? client->context : NULL);
    if (!runtime) return -1;
    bool cleanup_failed = false;
    if (runtime->staged_pipeline_active) {
        const rdma_dada::pipeline::StageStatus drain_status =
            runtime->gpu_pipeline.Drain();
        if (!drain_status.ok()) {
            SetFailure(runtime,
                       "staged GPU drain failed: " + drain_status.message());
            cleanup_failed = true;
        }
    }
    if (runtime->metrics_transfer_started) {
        runtime->metrics.SetTransferElapsedNs(ElapsedNs(
            runtime->metrics_transfer_start, WorkerClock::now()));
        runtime->metrics_transfer_started = false;
    }
    if (runtime->output_block_open) {
        if (ipcio_close_block_write(
                runtime->output_hdu->data_block, 0) < 0) {
            cleanup_failed = true;
        }
        runtime->output_block_open = false;
    }
#if defined(RDMA_DADA_HAVE_CUDA)
    if (!runtime->staged_pipeline_active && runtime->stream &&
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
    const rdma_dada::pipeline::StageStatus conversion_finish_status =
        runtime->conversion.Finish();
    if (!conversion_finish_status.ok()) {
        SetFailure(runtime,
                   "complex conversion finish failed: " +
                       conversion_finish_status.message());
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
    if (runtime->staged_pipeline_active) {
        const rdma_dada::pipeline::StageStatus gpu_finish_status =
            runtime->gpu_pipeline.Finish();
        if (!gpu_finish_status.ok() && !runtime->failed) {
            SetFailure(runtime,
                       "staged GPU finish failed: " +
                           gpu_finish_status.message());
            cleanup_failed = true;
        }
    }
    std::string metrics_error;
    const rdma_dada::pipeline::WorkerMetrics& final_metrics =
        runtime->staged_pipeline_active ? runtime->gpu_pipeline.metrics() :
                                          runtime->metrics;
    runtime->metrics_finalized = true;
    if (!final_metrics.WriteJson(runtime->metrics_path, &metrics_error)) {
        SetFailure(runtime, metrics_error);
        cleanup_failed = true;
    }
    runtime->staged_pipeline_active = false;
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
    std::cerr << "Usage: " << program
              << " RESOLVED_OBSERVATION.json [--metrics-json PATH]\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 4) {
        PrintUsage(argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 4 && std::string(argv[2]) != "--metrics-json") {
        PrintUsage(argv[0]);
        return EXIT_FAILURE;
    }

    WorkerRuntime runtime;
    if (argc == 4) runtime.metrics_path = argv[3];
    std::string config_error;
    if (!rdma_dada::LoadResolvedObservationPlan(
            argv[1], &runtime.plan, &config_error) ||
        !rdma_dada::pipeline::BuildWorkerConfigFromResolvedPlan(
            runtime.plan, &runtime.config, &runtime.geometry,
            &config_error)) {
        std::cerr << "pipeline_worker: " << config_error << '\n';
        return EXIT_FAILURE;
    }
    rdma_dada::ObservationArtifacts expected_artifacts;
    if (!rdma_dada::BuildObservationArtifactsFromResolvedPlan(
            runtime.plan, &expected_artifacts, &config_error)) {
        std::cerr << "pipeline_worker: cannot build compiled header contract: "
                  << config_error << '\n';
        return EXIT_FAILURE;
    }
    runtime.expected_input_header = expected_artifacts.unpacked_header;
    runtime.expected_output_header = expected_artifacts.output_header;

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
        if (!runtime.failed) {
            (void)ValidateConnectedRings(&runtime, input_hdu);
        }
        if (!runtime.failed && dada_hdu_lock_read(input_hdu) < 0) {
            SetFailure(&runtime, "cannot lock input DADA ring for reading");
        } else if (!runtime.failed) {
            input_locked = true;
        }
#if defined(RDMA_DADA_HAVE_CUDA)
        if (!runtime.failed &&
            !RegisterInputRingWithCuda(&runtime, input_hdu)) {
            result = EXIT_FAILURE;
        }
#endif
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
    // CloseTransfer writes the authoritative metrics source.  In staged mode
    // that source belongs to GpuBlockPipeline, not runtime.metrics.  Only use
    // the direct metrics object as a fallback when no transfer reached its
    // close callback (for example, an input-open failure).
    if (!runtime.metrics_path.empty() && !runtime.metrics_finalized) {
        if (runtime.metrics_transfer_started) {
            runtime.metrics.SetTransferElapsedNs(ElapsedNs(
                runtime.metrics_transfer_start, WorkerClock::now()));
            runtime.metrics_transfer_started = false;
        }
        std::string metrics_error;
        if (!runtime.metrics.WriteJson(runtime.metrics_path, &metrics_error)) {
            SetFailure(&runtime, metrics_error);
            result = EXIT_FAILURE;
        }
    }
    if (input_locked && dada_hdu_unlock_read(input_hdu) < 0) {
        result = EXIT_FAILURE;
    }
    if (client) dada_client_destroy(client);
#if defined(RDMA_DADA_HAVE_CUDA)
    if (!UnregisterInputRingFromCuda(&runtime, input_hdu)) {
        result = EXIT_FAILURE;
    }
#endif
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
