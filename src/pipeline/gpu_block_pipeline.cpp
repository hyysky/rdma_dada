#include "rdma_dada/pipeline/gpu_block_pipeline.h"

#include "rdma_dada/modules/complex_convert/complex_convert_module.h"
#include "rdma_dada/modules/device_to_host/device_to_host_module.h"
#include "rdma_dada/modules/host_to_device/host_to_device_module.h"
#include "rdma_dada/pipeline/module_chain.h"
#include "rdma_dada/pipeline/ordered_slot_scheduler.h"

#if defined(RDMA_DADA_HAVE_CUDA)
#include <cuda_runtime_api.h>
#endif

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace rdma_dada {
namespace pipeline {
namespace {

typedef std::chrono::steady_clock PipelineClock;

std::uint64_t ElapsedNs(const PipelineClock::time_point& start,
                        const PipelineClock::time_point& finish) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start)
            .count());
}

std::uint64_t MonotonicNs(const PipelineClock::time_point& value) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            value.time_since_epoch()).count());
}

bool CheckedAdd(std::uint64_t left, std::uint64_t right,
                std::uint64_t* result) {
    if (!result || left > std::numeric_limits<std::uint64_t>::max() - right) {
        return false;
    }
    *result = left + right;
    return true;
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t* result) {
    if (!result || (left != 0U &&
                    right > std::numeric_limits<std::uint64_t>::max() / left)) {
        return false;
    }
    *result = left * right;
    return true;
}

#if defined(RDMA_DADA_HAVE_CUDA)
StageStatus CudaStatus(cudaError_t result, const char* operation) {
    if (result == cudaSuccess) return StageStatus::Ok();
    return StageStatus::Error(
        std::string(operation) + ": " + cudaGetErrorString(result));
}
#endif

}  // namespace

class GpuBlockPipeline::Impl {
public:
    struct Slot {
        Slot()
            : pinned_output(NULL), input_bytes(0U), output_bytes(0U),
              active(false),
              submitted_ready(false)
#if defined(RDMA_DADA_HAVE_CUDA)
              , stream(NULL), device_input(NULL), device_converted(NULL),
              device_scratch(NULL), device_output(NULL), event_start(NULL),
              event_h2d(NULL), event_algorithm(NULL), event_d2h(NULL),
              event_completion(NULL)
#endif
        {}

        SlotLease lease;
        std::uint8_t* pinned_output;
        std::uint64_t input_bytes;
        std::uint64_t output_bytes;
        bool active;
        bool submitted_ready;
        PipelineClock::time_point service_start;
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
        cudaEvent_t event_completion;
#endif
    };

    Impl()
        : configured(false), aborted(false), accepting(false), draining(false),
          writer_stop(false), next_publish_sequence(0U),
          input_location(MemoryLocation::kHost)
#if defined(RDMA_DADA_HAVE_CUDA)
          , stream(NULL), device_input(NULL), device_converted(NULL),
          device_scratch(NULL), device_output(NULL), event_start(NULL),
          event_h2d(NULL), event_algorithm(NULL), event_d2h(NULL)
#endif
    {}

    StageStatus Release() {
#if defined(RDMA_DADA_HAVE_CUDA)
        for (std::size_t index = 0; index < slots.size(); ++index) {
            Slot& slot = *slots[index];
            if (slot.stream) cudaStreamSynchronize(slot.stream);
            if (slot.device_input) cudaFree(slot.device_input);
            if (slot.device_converted) cudaFree(slot.device_converted);
            if (slot.device_scratch) cudaFree(slot.device_scratch);
            if (slot.device_output) cudaFree(slot.device_output);
            if (slot.event_start) cudaEventDestroy(slot.event_start);
            if (slot.event_h2d) cudaEventDestroy(slot.event_h2d);
            if (slot.event_algorithm) cudaEventDestroy(slot.event_algorithm);
            if (slot.event_d2h) cudaEventDestroy(slot.event_d2h);
            if (slot.event_completion) cudaEventDestroy(slot.event_completion);
            if (slot.stream) cudaStreamDestroy(slot.stream);
            if (slot.pinned_output) {
                cudaHostUnregister(slot.pinned_output);
                std::free(slot.pinned_output);
            }
        }
#endif
        slots.clear();
        scheduler.reset();
#if defined(RDMA_DADA_HAVE_CUDA)
        if (stream) cudaStreamSynchronize(stream);
        if (device_input) cudaFree(device_input);
        if (device_converted) cudaFree(device_converted);
        if (device_scratch) cudaFree(device_scratch);
        if (device_output) cudaFree(device_output);
        device_input = NULL;
        device_converted = NULL;
        device_scratch = NULL;
        device_output = NULL;
        if (event_start) cudaEventDestroy(event_start);
        if (event_h2d) cudaEventDestroy(event_h2d);
        if (event_algorithm) cudaEventDestroy(event_algorithm);
        if (event_d2h) cudaEventDestroy(event_d2h);
        event_start = NULL;
        event_h2d = NULL;
        event_algorithm = NULL;
        event_d2h = NULL;
        if (stream) cudaStreamDestroy(stream);
        stream = NULL;
#endif
        return StageStatus::Ok();
    }

#if defined(RDMA_DADA_HAVE_CUDA)
    StageStatus AllocateStagedSlots();
    StageStatus EnqueueStaged(Slot* slot, const std::uint8_t* ring_data,
                              std::uint64_t input_bytes);
    void WriterLoop();
#endif

    bool configured;
    bool aborted;
    bool accepting;
    bool draining;
    bool writer_stop;
    std::uint64_t next_publish_sequence;
    std::string first_error;
    WorkerConfig config;
    WorkerBlockGeometry geometry;
    OutputBlockFunctions output;
    MemoryLocation input_location;
    modules::host_to_device::HostToDeviceModule h2d;
    modules::complex_convert::ComplexConvertModule conversion;
    ModuleChain chain;
    modules::device_to_host::DeviceToHostModule d2h;
    WorkerMetrics metrics;
    PipelineClock::time_point transfer_start;
    std::vector<std::unique_ptr<Slot> > slots;
    std::unique_ptr<OrderedSlotScheduler> scheduler;
    std::mutex mutex;
    std::condition_variable condition;
    std::thread writer;
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

#if defined(RDMA_DADA_HAVE_CUDA)
StageStatus GpuBlockPipeline::Impl::AllocateStagedSlots() {
    scheduler.reset(new OrderedSlotScheduler(config.cuda_inflight_blocks));
    for (std::uint32_t index = 0U;
        index < config.cuda_inflight_blocks; ++index) {
        std::unique_ptr<Slot> slot(new Slot());
        void* output_buffer = NULL;
        if (posix_memalign(&output_buffer, 4096U,
                           static_cast<std::size_t>(
                               geometry.output_block_bytes)) != 0) {
            if (output_buffer) std::free(output_buffer);
            return StageStatus::Error(
                "cannot allocate aligned output staging buffer");
        }
        slot->pinned_output = static_cast<std::uint8_t*>(output_buffer);
        std::memset(slot->pinned_output, 0,
                    static_cast<std::size_t>(geometry.output_block_bytes));
        // Transfer ownership before the first CUDA call so Configure() can
        // release a partially initialized slot through the normal cleanup
        // path when any later operation fails.
        slots.push_back(std::move(slot));
        Slot* owned_slot = slots.back().get();
        StageStatus status = CudaStatus(cudaHostRegister(
            owned_slot->pinned_output,
            static_cast<std::size_t>(geometry.output_block_bytes),
            cudaHostRegisterDefault), "cudaHostRegister output staging");
        if (!status.ok()) return status;
        status = CudaStatus(cudaStreamCreateWithFlags(
                                &owned_slot->stream, cudaStreamNonBlocking),
                            "cudaStreamCreateWithFlags slot");
        if (!status.ok()) return status;
        status = CudaStatus(cudaEventCreate(&owned_slot->event_start),
                            "cudaEventCreate slot start");
        if (!status.ok()) return status;
        status = CudaStatus(cudaEventCreate(&owned_slot->event_h2d),
                            "cudaEventCreate slot H2D");
        if (!status.ok()) return status;
        status = CudaStatus(cudaEventCreate(&owned_slot->event_algorithm),
                            "cudaEventCreate slot algorithm");
        if (!status.ok()) return status;
        status = CudaStatus(cudaEventCreate(&owned_slot->event_d2h),
                            "cudaEventCreate slot D2H");
        if (!status.ok()) return status;
        status = CudaStatus(cudaEventCreate(&owned_slot->event_completion),
                            "cudaEventCreate slot completion");
        if (!status.ok()) return status;
        status = CudaStatus(cudaMalloc(
            reinterpret_cast<void**>(&owned_slot->device_input),
            static_cast<std::size_t>(geometry.input_block_bytes)),
            "cudaMalloc slot input");
        if (!status.ok()) return status;
        status = CudaStatus(cudaMalloc(
            reinterpret_cast<void**>(&owned_slot->device_converted),
            static_cast<std::size_t>(geometry.converted_block_bytes)),
            "cudaMalloc slot converted");
        if (!status.ok()) return status;
        if (geometry.scratch_block_bytes != 0U) {
            status = CudaStatus(cudaMalloc(
                reinterpret_cast<void**>(&owned_slot->device_scratch),
                static_cast<std::size_t>(geometry.scratch_block_bytes)),
                "cudaMalloc slot scratch");
            if (!status.ok()) return status;
        }
        status = CudaStatus(cudaMalloc(
            reinterpret_cast<void**>(&owned_slot->device_output),
            static_cast<std::size_t>(geometry.output_block_bytes)),
            "cudaMalloc slot output");
        if (!status.ok()) return status;
    }
    return StageStatus::Ok();
}

StageStatus GpuBlockPipeline::Impl::EnqueueStaged(
    Slot* slot, const std::uint8_t* ring_data, std::uint64_t input_bytes) {
    if (!slot) return StageStatus::Error("staged slot is null");
    slot->service_start = PipelineClock::now();
    slot->input_bytes = input_bytes;

    const std::uint64_t ntime = input_bytes / geometry.input_frame_bytes;
    if (ntime > std::numeric_limits<std::uint64_t>::max() /
                    geometry.converted_frame_bytes) {
        return StageStatus::Error("staged converted byte count overflows");
    }
    const std::uint64_t converted_bytes =
        ntime * geometry.converted_frame_bytes;
    std::uint64_t scratch_bytes = 0U;
    StageStatus status = chain.PlanBlock(
        converted_bytes, &scratch_bytes, &slot->output_bytes);
    if (!status.ok()) return status;

    const BlockExecutionContext context = {
        ExecutionBackend::kCuda, config.cuda_device,
        reinterpret_cast<void*>(slot->stream)
    };
    status = CudaStatus(cudaEventRecord(slot->event_start, slot->stream),
                        "cudaEventRecord staged start");
    OutputBlock device_input = {
        slot->device_input, geometry.input_block_bytes, 0U,
        slot->lease.sequence, MemoryLocation::kCudaDevice
    };
    if (status.ok()) {
        const InputBlock host_input = {
            ring_data, input_bytes, slot->lease.sequence,
            MemoryLocation::kPinnedHost
        };
        status = h2d.ProcessBlock(host_input, &device_input, context);
    }
    if (status.ok()) {
        status = CudaStatus(cudaEventRecord(slot->event_h2d, slot->stream),
                            "cudaEventRecord staged H2D");
    }
    OutputBlock converted = {
        slot->device_converted, geometry.converted_block_bytes, 0U,
        slot->lease.sequence, MemoryLocation::kCudaDevice
    };
    if (status.ok()) {
        const InputBlock integer_input = {
            device_input.data, device_input.size, slot->lease.sequence,
            MemoryLocation::kCudaDevice
        };
        status = conversion.ProcessBlock(integer_input, &converted, context);
    }
    OutputBlock device_result = {
        slot->device_output, geometry.output_block_bytes, 0U,
        slot->lease.sequence, MemoryLocation::kCudaDevice
    };
    if (status.ok()) {
        const InputBlock converted_input = {
            converted.data, converted.size, slot->lease.sequence,
            MemoryLocation::kCudaDevice
        };
        status = chain.ProcessBlock(
            converted_input, &device_result, slot->device_scratch,
            scratch_bytes, context);
    }
    if (status.ok() && device_result.size != slot->output_bytes) {
        status = StageStatus::Error(
            "staged module chain produced unexpected output bytes");
    }
    if (status.ok()) {
        status = CudaStatus(cudaEventRecord(
                                slot->event_algorithm, slot->stream),
                            "cudaEventRecord staged algorithm");
    }
    if (status.ok()) {
        const InputBlock result = {
            device_result.data, device_result.size, slot->lease.sequence,
            MemoryLocation::kCudaDevice
        };
        OutputBlock host_result = {
            slot->pinned_output, geometry.output_block_bytes, 0U,
            slot->lease.sequence, MemoryLocation::kPinnedHost
        };
        status = d2h.ProcessBlock(result, &host_result, context);
        if (status.ok() && host_result.size != slot->output_bytes) {
            status = StageStatus::Error(
                "staged D2H produced unexpected output bytes");
        }
    }
    if (status.ok()) {
        status = CudaStatus(cudaEventRecord(slot->event_d2h, slot->stream),
                            "cudaEventRecord staged D2H");
    }
    if (status.ok()) {
        status = CudaStatus(cudaEventRecord(
                                slot->event_completion, slot->stream),
                            "cudaEventRecord staged completion");
    }
    return status;
}

void GpuBlockPipeline::Impl::WriterLoop() {
    StageStatus device_status = CudaStatus(
        cudaSetDevice(config.cuda_device), "cudaSetDevice staged writer");
    if (!device_status.ok()) {
        std::lock_guard<std::mutex> lock(mutex);
        aborted = true;
        accepting = false;
        first_error = device_status.message();
        condition.notify_all();
        return;
    }
    while (true) {
        Slot* slot = NULL;
        {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [this]() {
                if (writer_stop || aborted) return true;
                for (std::size_t index = 0; index < slots.size(); ++index) {
                    if (slots[index]->active &&
                        slots[index]->submitted_ready &&
                        slots[index]->lease.sequence ==
                            next_publish_sequence) {
                        return true;
                    }
                }
                return false;
            });
            if (writer_stop || aborted) return;
            for (std::size_t index = 0; index < slots.size(); ++index) {
                if (slots[index]->active &&
                    slots[index]->submitted_ready &&
                    slots[index]->lease.sequence == next_publish_sequence) {
                    slot = slots[index].get();
                    break;
                }
            }
        }
        if (!slot) continue;
        bool reordered = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (std::size_t index = 0; index < slots.size(); ++index) {
                const Slot& later = *slots[index];
                if (later.active && later.submitted_ready &&
                    later.lease.sequence > slot->lease.sequence &&
                    cudaEventQuery(later.event_completion) == cudaSuccess) {
                    reordered = true;
                    break;
                }
            }
        }
        const PipelineClock::time_point writer_wait_start =
            PipelineClock::now();
        StageStatus status = CudaStatus(
            cudaEventSynchronize(slot->event_completion),
            "cudaEventSynchronize completion");
        const std::uint64_t writer_wait_ns = ElapsedNs(
            writer_wait_start, PipelineClock::now());
        if (status.ok()) {
            std::lock_guard<std::mutex> lock(mutex);
            status = scheduler->MarkCompleted(slot->lease);
        }
        if (status.ok()) metrics.RecordCompletion(reordered, writer_wait_ns);
        if (!status.ok()) {
            std::lock_guard<std::mutex> lock(mutex);
            aborted = true;
            first_error = status.message();
            condition.notify_all();
            return;
        }

        std::uint8_t* ring_output = NULL;
        std::uint64_t ring_capacity = 0U;
        const PipelineClock::time_point wait_start = PipelineClock::now();
        status = output.acquire(
            slot->lease.sequence, &ring_output, &ring_capacity);
        const bool output_acquired = status.ok();
        const std::uint64_t output_wait_ns = ElapsedNs(
            wait_start, PipelineClock::now());
        if (status.ok() &&
            (!ring_output || ring_capacity < slot->output_bytes)) {
            status = StageStatus::Error("staged output ring block is too small");
        }
        if (status.ok()) {
            const PipelineClock::time_point copy_start = PipelineClock::now();
            std::memcpy(ring_output, slot->pinned_output,
                        static_cast<std::size_t>(slot->output_bytes));
            const std::uint64_t output_copy_ns = ElapsedNs(
                copy_start, PipelineClock::now());
            status = output.commit(slot->lease.sequence, slot->output_bytes);
            if (status.ok()) {
                metrics.RecordPublication(
                    slot->output_bytes, output_copy_ns,
                    MonotonicNs(PipelineClock::now()));
            }
        }
        if (!status.ok() && output_acquired) {
            (void)output.abort(slot->lease.sequence);
        }
        if (!status.ok()) {
            std::lock_guard<std::mutex> lock(mutex);
            (void)scheduler->MarkFailed(slot->lease, status.message());
            aborted = true;
            first_error = status.message();
            condition.notify_all();
            return;
        }

        float h2d_ms = 0.0F;
        float algorithm_ms = 0.0F;
        float d2h_ms = 0.0F;
        (void)cudaEventElapsedTime(
            &h2d_ms, slot->event_start, slot->event_h2d);
        (void)cudaEventElapsedTime(
            &algorithm_ms, slot->event_h2d, slot->event_algorithm);
        (void)cudaEventElapsedTime(
            &d2h_ms, slot->event_algorithm, slot->event_d2h);
        metrics.RecordBlock(
            slot->input_bytes, slot->output_bytes,
            ElapsedNs(slot->service_start, PipelineClock::now()),
            output_wait_ns, h2d_ms, algorithm_ms, d2h_ms);
        {
            std::lock_guard<std::mutex> lock(mutex);
            const StageStatus published = scheduler->MarkPublished(slot->lease);
            if (!published.ok()) {
                aborted = true;
                first_error = published.message();
                condition.notify_all();
                return;
            }
            slot->active = false;
            slot->submitted_ready = false;
            ++next_publish_sequence;
            condition.notify_all();
        }
    }
}
#endif

GpuBlockPipeline::GpuBlockPipeline() : impl_(new Impl()) {}

GpuBlockPipeline::~GpuBlockPipeline() { (void)Finish(); }

StageStatus GpuBlockPipeline::Configure(
    const WorkerConfig& config,
    const WorkerBlockGeometry& geometry,
    const Metadata& input_header,
    const OutputBlockFunctions& output,
    Metadata* output_header) {
    StageStatus prior = Finish();
    if (!prior.ok()) return prior;
    if (!output_header) return StageStatus::Error("GPU output header is null");
    if (config.execution_backend != "CUDA") {
        return StageStatus::Error("GPU block pipeline requires CUDA backend");
    }
    if (config.cuda_pipeline_mode == CudaPipelineMode::kSynchronousDirect &&
        config.cuda_inflight_blocks != 1U) {
        return StageStatus::Error("synchronous direct requires one slot");
    }
    if (config.cuda_pipeline_mode == CudaPipelineMode::kStagedPipeline &&
        (config.cuda_inflight_blocks < 1U ||
         config.cuda_inflight_blocks > 4U)) {
        return StageStatus::Error("staged pipeline requires one to four slots");
    }
    if (!output.acquire || !output.commit || !output.abort) {
        return StageStatus::Error("GPU output block functions are incomplete");
    }
    if (geometry.input_block_bytes == 0U ||
        geometry.converted_block_bytes == 0U ||
        geometry.output_block_bytes == 0U) {
        return StageStatus::Error("GPU block geometry is incomplete");
    }

#if !defined(RDMA_DADA_HAVE_CUDA)
    (void)input_header;
    return StageStatus::Error(
        "GPU block pipeline requires a CUDA-enabled build");
#else
    impl_->config = config;
    impl_->geometry = geometry;
    impl_->output = output;
    impl_->aborted = false;
    impl_->accepting = false;
    impl_->draining = false;
    impl_->writer_stop = false;
    impl_->next_publish_sequence = 0U;
    impl_->first_error.clear();
    impl_->metrics.Reset();
    const std::uint64_t slot_count =
        config.cuda_pipeline_mode == CudaPipelineMode::kStagedPipeline ?
            config.cuda_inflight_blocks : 1U;
    std::uint64_t device_bytes_per_slot = 0U;
    std::uint64_t temporary = 0U;
    std::uint64_t weight_bytes = 0U;
    std::uint64_t slot_device_bytes = 0U;
    std::uint64_t planned_device_bytes = 0U;
    std::uint64_t planned_pinned_host_bytes = 0U;
    if (!CheckedAdd(geometry.input_block_bytes,
                    geometry.converted_block_bytes, &temporary) ||
        !CheckedAdd(temporary, geometry.scratch_block_bytes, &temporary) ||
        !CheckedAdd(temporary, geometry.output_block_bytes,
                    &device_bytes_per_slot) ||
        !CheckedMultiply(config.nchan, config.npol, &weight_bytes) ||
        !CheckedMultiply(weight_bytes, config.nant, &weight_bytes) ||
        !CheckedMultiply(weight_bytes, config.nbeam, &weight_bytes) ||
        !CheckedMultiply(weight_bytes, 8U, &weight_bytes) ||
        !CheckedMultiply(device_bytes_per_slot, slot_count,
                         &slot_device_bytes) ||
        !CheckedAdd(slot_device_bytes, weight_bytes,
                    &planned_device_bytes)) {
        return StageStatus::Error("GPU pipeline memory budget overflows");
    }
    if (config.cuda_pipeline_mode == CudaPipelineMode::kStagedPipeline &&
        !CheckedMultiply(geometry.output_block_bytes, slot_count,
                         &planned_pinned_host_bytes)) {
        return StageStatus::Error("GPU pinned staging budget overflows");
    }
    impl_->metrics.ConfigureExecution(
        CudaPipelineModeName(config.cuda_pipeline_mode),
        config.cuda_inflight_blocks, planned_device_bytes,
        planned_pinned_host_bytes);

    StageParameters transfer_parameters;
    transfer_parameters.SetString("EXECUTION_BACKEND", "CUDA");
    transfer_parameters.SetUint64(
        "CUDA_DEVICE", static_cast<std::uint64_t>(config.cuda_device));
    StageParameters algorithm_parameters;
    algorithm_parameters.SetString("EXECUTION_BACKEND", "CUDA");
    algorithm_parameters.SetUint64(
        "CUDA_DEVICE", static_cast<std::uint64_t>(config.cuda_device));
    algorithm_parameters.SetDouble("CONVERSION_SCALE",
                                   config.conversion_scale);

    Metadata h2d_input_header = input_header;
    if (config.cuda_pipeline_mode == CudaPipelineMode::kStagedPipeline) {
        // The public compute-ring metadata remains HOST. pipeline_worker
        // registers the PSRDADA data blocks for the process lifetime, so the
        // internal H2D boundary is pinned without an intermediate host copy.
        h2d_input_header.SetString("MEMORY", "PINNED_HOST");
    }
    Metadata conversion_input_header;
    StageStatus status = impl_->h2d.ConfigureHeader(
        h2d_input_header, transfer_parameters, &conversion_input_header);
    if (!status.ok()) return status;
    std::string input_memory;
    if (!h2d_input_header.GetString("MEMORY", &input_memory)) {
        return StageStatus::Error("GPU input header MEMORY is missing");
    }
    impl_->input_location = input_memory == "PINNED_HOST" ?
        MemoryLocation::kPinnedHost : MemoryLocation::kHost;

    Metadata converted_header;
    status = impl_->conversion.ConfigureHeader(
        conversion_input_header, algorithm_parameters, &converted_header);
    if (!status.ok()) return status;
    Metadata device_output_header;
    status = impl_->chain.Configure(
        converted_header, config, &device_output_header);
    if (!status.ok()) return status;
    std::string modules;
    if (device_output_header.GetString("PIPELINE_MODULES", &modules)) {
        device_output_header.SetString(
            "PIPELINE_MODULES", "complex_convert," + modules);
    }
    device_output_header.SetString("MEMORY", "CUDA_DEVICE");
    device_output_header.SetUint64(
        "CUDA_DEVICE", static_cast<std::uint64_t>(config.cuda_device));
    transfer_parameters.SetString(
        "OUTPUT_MEMORY",
        config.cuda_pipeline_mode == CudaPipelineMode::kStagedPipeline ?
            "PINNED_HOST" : "HOST");
    status = impl_->d2h.ConfigureHeader(
        device_output_header, transfer_parameters, output_header);
    if (!status.ok()) return status;
    // The published ring remains ordinary host memory. PINNED_HOST describes
    // only the private staged D2H target inside this executor. Keep
    // CUDA_DEVICE as processing provenance, matching the compiled output
    // contract and the synchronous-direct path.
    output_header->SetString("MEMORY", "HOST");
    output_header->SetUint64("INPUT_BLOCK_BYTES", geometry.input_block_bytes);
    output_header->SetUint64(
        "CONVERTED_BLOCK_BYTES", geometry.converted_block_bytes);
    output_header->SetDouble("CONVERSION_SCALE", config.conversion_scale);

    std::uint64_t scratch_bytes = 0U;
    std::uint64_t output_bytes = 0U;
    status = impl_->chain.PlanBlock(
        geometry.converted_block_bytes, &scratch_bytes, &output_bytes);
    if (!status.ok()) return status;
    if (scratch_bytes != geometry.scratch_block_bytes ||
        output_bytes != geometry.output_block_bytes) {
        return StageStatus::Error(
            "GPU module plan conflicts with resolved block geometry");
    }

    status = CudaStatus(cudaSetDevice(config.cuda_device), "cudaSetDevice");
    if (!status.ok()) return status;
    if (config.cuda_pipeline_mode == CudaPipelineMode::kStagedPipeline) {
        status = impl_->AllocateStagedSlots();
        if (!status.ok()) return status;
        impl_->configured = true;
        impl_->accepting = true;
        impl_->transfer_start = PipelineClock::now();
        impl_->writer = std::thread(&Impl::WriterLoop, impl_.get());
        return StageStatus::Ok();
    }

    status = CudaStatus(cudaStreamCreateWithFlags(
                            &impl_->stream, cudaStreamNonBlocking),
                        "cudaStreamCreateWithFlags");
    if (!status.ok()) return status;
    status = CudaStatus(cudaEventCreate(&impl_->event_start),
                        "cudaEventCreate start");
    if (!status.ok()) return status;
    status = CudaStatus(cudaEventCreate(&impl_->event_h2d),
                        "cudaEventCreate H2D");
    if (!status.ok()) return status;
    status = CudaStatus(cudaEventCreate(&impl_->event_algorithm),
                        "cudaEventCreate algorithm");
    if (!status.ok()) return status;
    status = CudaStatus(cudaEventCreate(&impl_->event_d2h),
                        "cudaEventCreate D2H");
    if (!status.ok()) return status;
    status = CudaStatus(cudaMalloc(
                            reinterpret_cast<void**>(&impl_->device_input),
                            static_cast<std::size_t>(geometry.input_block_bytes)),
                        "cudaMalloc input block");
    if (!status.ok()) return status;
    status = CudaStatus(cudaMalloc(
                            reinterpret_cast<void**>(&impl_->device_converted),
                            static_cast<std::size_t>(
                                geometry.converted_block_bytes)),
                        "cudaMalloc converted block");
    if (!status.ok()) return status;
    if (geometry.scratch_block_bytes != 0U) {
        status = CudaStatus(cudaMalloc(
                                reinterpret_cast<void**>(&impl_->device_scratch),
                                static_cast<std::size_t>(
                                    geometry.scratch_block_bytes)),
                            "cudaMalloc scratch block");
        if (!status.ok()) return status;
    }
    status = CudaStatus(cudaMalloc(
                            reinterpret_cast<void**>(&impl_->device_output),
                            static_cast<std::size_t>(geometry.output_block_bytes)),
                        "cudaMalloc output block");
    if (!status.ok()) return status;
    impl_->configured = true;
    impl_->transfer_start = PipelineClock::now();
    return StageStatus::Ok();
#endif
}

StageStatus GpuBlockPipeline::SubmitBlock(std::uint64_t sequence,
                                          const std::uint8_t* ring_data,
                                          std::uint64_t input_bytes) {
    if (!impl_->configured) {
        return StageStatus::Error("GPU block pipeline is not configured");
    }
    if (impl_->aborted) return StageStatus::Error(impl_->first_error);
    if (!ring_data) return StageStatus::Error("GPU input block is null");
    if (input_bytes == 0U ||
        input_bytes % impl_->geometry.input_frame_bytes != 0U) {
        return StageStatus::Error(
            "GPU input block does not contain complete ATFP frames");
    }

#if !defined(RDMA_DADA_HAVE_CUDA)
    (void)sequence;
    return StageStatus::Error("CUDA backend is unavailable");
#else
    if (impl_->config.cuda_pipeline_mode ==
        CudaPipelineMode::kStagedPipeline) {
        const PipelineClock::time_point slot_wait_start = PipelineClock::now();
        SlotLease lease = {};
        Impl::Slot* slot = NULL;
        std::uint64_t current_inflight = 0U;
        {
            std::unique_lock<std::mutex> lock(impl_->mutex);
            impl_->condition.wait(lock, [this]() {
                if (impl_->aborted || !impl_->accepting) return true;
                for (std::size_t index = 0;
                     index < impl_->slots.size(); ++index) {
                    if (!impl_->slots[index]->active) return true;
                }
                return false;
            });
            if (impl_->aborted) {
                return StageStatus::Error(impl_->first_error);
            }
            if (!impl_->accepting) {
                return StageStatus::Error("staged GPU pipeline is draining");
            }
            StageStatus acquired = impl_->scheduler->Acquire(sequence, &lease);
            if (!acquired.ok()) return acquired;
            slot = impl_->slots[lease.slot_index].get();
            slot->lease = lease;
            slot->active = true;
            slot->submitted_ready = false;
            for (std::size_t index = 0;
                 index < impl_->slots.size(); ++index) {
                if (impl_->slots[index]->active) ++current_inflight;
            }
        }
        const std::uint64_t submission_ns =
            MonotonicNs(PipelineClock::now());
        StageStatus enqueued = impl_->EnqueueStaged(
            slot, ring_data, input_bytes);
        if (enqueued.ok()) {
            enqueued = CudaStatus(
                cudaEventSynchronize(slot->event_h2d),
                "cudaEventSynchronize staged H2D before ring release");
        }
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (!enqueued.ok()) {
                (void)impl_->scheduler->MarkFailed(lease,
                                                   enqueued.message());
                impl_->aborted = true;
                impl_->accepting = false;
                impl_->first_error = enqueued.message();
            } else {
                slot->submitted_ready = true;
                impl_->metrics.RecordSubmission(
                    0U, 0U,
                    ElapsedNs(slot_wait_start, PipelineClock::now()),
                    current_inflight, submission_ns);
            }
            impl_->condition.notify_all();
        }
        return enqueued;
    }

    const PipelineClock::time_point service_start = PipelineClock::now();
    const std::uint64_t ntime =
        input_bytes / impl_->geometry.input_frame_bytes;
    if (ntime > std::numeric_limits<std::uint64_t>::max() /
                    impl_->geometry.converted_frame_bytes) {
        return StageStatus::Error("converted block byte count overflows");
    }
    const std::uint64_t converted_bytes =
        ntime * impl_->geometry.converted_frame_bytes;
    std::uint64_t scratch_bytes = 0U;
    std::uint64_t output_bytes = 0U;
    StageStatus status = impl_->chain.PlanBlock(
        converted_bytes, &scratch_bytes, &output_bytes);
    if (!status.ok()) return status;

    std::uint8_t* output_data = NULL;
    std::uint64_t output_capacity = 0U;
    const PipelineClock::time_point output_wait_start = PipelineClock::now();
    status = impl_->output.acquire(
        sequence, &output_data, &output_capacity);
    const std::uint64_t output_wait_ns = ElapsedNs(
        output_wait_start, PipelineClock::now());
    if (!status.ok()) return status;
    if (!output_data || output_capacity < output_bytes) {
        (void)impl_->output.abort(sequence);
        return StageStatus::Error("GPU output ring block is too small");
    }

    bool acquired = true;
    double h2d_ms_recorded = 0.0;
    double algorithm_ms_recorded = 0.0;
    double d2h_ms_recorded = 0.0;
    status = CudaStatus(cudaEventRecord(impl_->event_start, impl_->stream),
                        "cudaEventRecord start");
    const BlockExecutionContext context = {
        ExecutionBackend::kCuda, impl_->config.cuda_device,
        reinterpret_cast<void*>(impl_->stream)
    };
    if (status.ok()) {
        const InputBlock host_input = {
            ring_data, input_bytes, sequence, impl_->input_location
        };
        OutputBlock device_input = {
            impl_->device_input, impl_->geometry.input_block_bytes,
            0U, sequence, MemoryLocation::kCudaDevice
        };
        status = impl_->h2d.ProcessBlock(host_input, &device_input, context);
        if (status.ok()) {
            status = CudaStatus(cudaEventRecord(
                                    impl_->event_h2d, impl_->stream),
                                "cudaEventRecord H2D");
        }
        OutputBlock converted = {
            impl_->device_converted, impl_->geometry.converted_block_bytes,
            0U, sequence, MemoryLocation::kCudaDevice
        };
        if (status.ok()) {
            const InputBlock integer_input = {
                device_input.data, device_input.size, sequence,
                MemoryLocation::kCudaDevice
            };
            status = impl_->conversion.ProcessBlock(
                integer_input, &converted, context);
        }
        OutputBlock result = {
            impl_->device_output, impl_->geometry.output_block_bytes,
            0U, sequence, MemoryLocation::kCudaDevice
        };
        if (status.ok()) {
            const InputBlock converted_input = {
                converted.data, converted.size, sequence,
                MemoryLocation::kCudaDevice
            };
            status = impl_->chain.ProcessBlock(
                converted_input, &result, impl_->device_scratch,
                scratch_bytes, context);
        }
        if (status.ok() && result.size != output_bytes) {
            status = StageStatus::Error(
                "GPU module chain produced unexpected output bytes");
        }
        if (status.ok()) {
            status = CudaStatus(cudaEventRecord(
                                    impl_->event_algorithm, impl_->stream),
                                "cudaEventRecord algorithm");
        }
        if (status.ok()) {
            const InputBlock device_result = {
                result.data, result.size, sequence,
                MemoryLocation::kCudaDevice
            };
            OutputBlock host_result = {
                output_data, output_capacity, 0U, sequence,
                MemoryLocation::kHost
            };
            status = impl_->d2h.ProcessBlock(
                device_result, &host_result, context);
            if (status.ok() && host_result.size != output_bytes) {
                status = StageStatus::Error(
                    "GPU D2H produced unexpected output bytes");
            }
        }
        if (status.ok()) {
            status = CudaStatus(cudaEventRecord(
                                    impl_->event_d2h, impl_->stream),
                                "cudaEventRecord D2H");
        }
    }
    const StageStatus synchronize = CudaStatus(
        cudaStreamSynchronize(impl_->stream), "cudaStreamSynchronize");
    if (status.ok() && !synchronize.ok()) status = synchronize;
    if (status.ok()) {
        float h2d_ms = 0.0F;
        float algorithm_ms = 0.0F;
        float d2h_ms = 0.0F;
        status = CudaStatus(cudaEventElapsedTime(
                                &h2d_ms, impl_->event_start,
                                impl_->event_h2d),
                            "cudaEventElapsedTime H2D");
        if (status.ok()) {
            status = CudaStatus(cudaEventElapsedTime(
                                    &algorithm_ms, impl_->event_h2d,
                                    impl_->event_algorithm),
                                "cudaEventElapsedTime algorithm");
        }
        if (status.ok()) {
            status = CudaStatus(cudaEventElapsedTime(
                                    &d2h_ms, impl_->event_algorithm,
                                    impl_->event_d2h),
                                "cudaEventElapsedTime D2H");
        }
        h2d_ms_recorded = h2d_ms;
        algorithm_ms_recorded = algorithm_ms;
        d2h_ms_recorded = d2h_ms;
    }
    if (!status.ok()) {
        if (acquired) (void)impl_->output.abort(sequence);
        return status;
    }
    status = impl_->output.commit(sequence, output_bytes);
    if (!status.ok()) {
        (void)impl_->output.abort(sequence);
        return status;
    }
    impl_->metrics.RecordBlock(
        input_bytes, output_bytes,
        ElapsedNs(service_start, PipelineClock::now()), output_wait_ns,
        h2d_ms_recorded, algorithm_ms_recorded, d2h_ms_recorded);
    return StageStatus::Ok();
#endif
}

StageStatus GpuBlockPipeline::PlanBlock(
    std::uint64_t input_bytes, std::uint64_t* scratch_bytes,
    std::uint64_t* output_bytes) const {
    if (!impl_->configured) {
        return StageStatus::Error("GPU block pipeline is not configured");
    }
    if (!scratch_bytes || !output_bytes) {
        return StageStatus::Error("GPU block plan output is null");
    }
    if (input_bytes == 0U ||
        input_bytes % impl_->geometry.input_frame_bytes != 0U) {
        return StageStatus::Error(
            "GPU input block does not contain complete ATFP frames");
    }
    const std::uint64_t ntime =
        input_bytes / impl_->geometry.input_frame_bytes;
    if (ntime > std::numeric_limits<std::uint64_t>::max() /
                    impl_->geometry.converted_frame_bytes) {
        return StageStatus::Error("converted block byte count overflows");
    }
    return impl_->chain.PlanBlock(
        ntime * impl_->geometry.converted_frame_bytes,
        scratch_bytes, output_bytes);
}

void GpuBlockPipeline::RecordInputRingRegistration(
    std::uint64_t registered_ring_blocks,
    std::uint64_t registered_ring_bytes,
    std::uint64_t registration_ns) {
    impl_->metrics.RecordInputRingRegistration(
        registered_ring_blocks, registered_ring_bytes, registration_ns);
}

StageStatus GpuBlockPipeline::Drain() {
    if (!impl_->configured) return StageStatus::Ok();
#if defined(RDMA_DADA_HAVE_CUDA)
    if (impl_->config.cuda_pipeline_mode ==
        CudaPipelineMode::kStagedPipeline) {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->accepting = false;
        impl_->draining = true;
        impl_->condition.notify_all();
        impl_->condition.wait(lock, [this]() {
            return impl_->aborted || impl_->scheduler->empty();
        });
        impl_->writer_stop = true;
        impl_->condition.notify_all();
        const bool failed = impl_->aborted;
        const std::string message = impl_->first_error;
        lock.unlock();
        if (impl_->writer.joinable()) impl_->writer.join();
        impl_->metrics.SetTransferElapsedNs(ElapsedNs(
            impl_->transfer_start, PipelineClock::now()));
        return failed ? StageStatus::Error(message) : StageStatus::Ok();
    }
    const StageStatus status = CudaStatus(
        cudaStreamSynchronize(impl_->stream), "cudaStreamSynchronize drain");
    impl_->metrics.SetTransferElapsedNs(ElapsedNs(
        impl_->transfer_start, PipelineClock::now()));
    return status;
#else
    return StageStatus::Ok();
#endif
}

StageStatus GpuBlockPipeline::Abort(const std::string& reason) {
    if (reason.empty()) return StageStatus::Error("GPU abort reason is empty");
    if (!impl_->aborted) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->aborted = true;
        impl_->accepting = false;
        impl_->writer_stop = true;
        impl_->first_error = reason;
        impl_->condition.notify_all();
    }
#if defined(RDMA_DADA_HAVE_CUDA)
    if (impl_->writer.joinable()) impl_->writer.join();
#endif
    return StageStatus::Ok();
}

StageStatus GpuBlockPipeline::Finish() {
    StageStatus first = Drain();
    const StageStatus chain = impl_->chain.Finish();
    const StageStatus conversion = impl_->conversion.Finish();
    const StageStatus h2d = impl_->h2d.Finish();
    const StageStatus d2h = impl_->d2h.Finish();
    (void)impl_->Release();
    impl_->configured = false;
    impl_->aborted = false;
    impl_->accepting = false;
    impl_->draining = false;
    impl_->writer_stop = false;
    impl_->first_error.clear();
    if (!first.ok()) return first;
    if (!chain.ok()) return chain;
    if (!conversion.ok()) return conversion;
    if (!h2d.ok()) return h2d;
    return d2h;
}

const WorkerMetrics& GpuBlockPipeline::metrics() const {
    return impl_->metrics;
}

}  // namespace pipeline
}  // namespace rdma_dada
