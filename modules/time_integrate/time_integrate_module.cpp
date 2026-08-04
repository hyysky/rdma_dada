#include "rdma_dada/modules/time_integrate/time_integrate_module.h"

#include "time_integrate_backend.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace rdma_dada {
namespace modules {
namespace time_integrate {
namespace {

pipeline::StageStatus MissingOrInvalid(const std::string& name) {
    return pipeline::StageStatus::Error("missing or invalid " + name);
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t* result) {
    if (!result) return false;
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    *result = left * right;
    return true;
}

bool IsHostMemory(pipeline::MemoryLocation location) {
    return location == pipeline::MemoryLocation::kHost ||
           location == pipeline::MemoryLocation::kPinnedHost;
}

bool RangesOverlap(const std::uint8_t* input, std::uint64_t input_bytes,
                   const std::uint8_t* output, std::uint64_t output_bytes) {
    const std::uintptr_t input_begin =
        reinterpret_cast<std::uintptr_t>(input);
    const std::uintptr_t output_begin =
        reinterpret_cast<std::uintptr_t>(output);
    const std::uintptr_t maximum =
        std::numeric_limits<std::uintptr_t>::max();
    if (input_bytes > maximum - input_begin ||
        output_bytes > maximum - output_begin) {
        return true;
    }
    const std::uintptr_t input_end =
        input_begin + static_cast<std::uintptr_t>(input_bytes);
    const std::uintptr_t output_end =
        output_begin + static_cast<std::uintptr_t>(output_bytes);
    return input_begin < output_end && output_begin < input_end;
}

pipeline::StageStatus ScaleByteField(
    const pipeline::Metadata& input, const std::string& name,
    std::uint64_t integration_length, bool require_positive,
    pipeline::Metadata* output) {
    if (!input.Has(name)) return pipeline::StageStatus::Ok();
    std::uint64_t value = 0;
    if (!input.GetUint64(name, &value) ||
        (require_positive && value == 0) ||
        value % integration_length != 0) {
        return pipeline::StageStatus::Error(
            name + " must be divisible by INTEGRATION_LENGTH");
    }
    output->SetUint64(name, value / integration_length);
    return pipeline::StageStatus::Ok();
}

}  // namespace

class TimeIntegrateModule::Impl {
public:
    Impl()
        : configured(false), integration_length(0), frame_elements(0),
          frame_bytes(0), calculate_mean(false),
          execution_backend(pipeline::ExecutionBackend::kHost),
          cuda_device(-1) {}

    bool configured;
    std::uint64_t integration_length;
    std::uint64_t frame_elements;
    std::uint64_t frame_bytes;
    bool calculate_mean;
    pipeline::ExecutionBackend execution_backend;
    int cuda_device;
    std::unique_ptr<CudaTimeIntegrateExecutor> cuda_executor;
};

TimeIntegrateModule::TimeIntegrateModule() : impl_(new Impl) {}

TimeIntegrateModule::~TimeIntegrateModule() {}

const char* TimeIntegrateModule::Name() const { return "time_integrate"; }

pipeline::StageStatus TimeIntegrateModule::ConfigureHeader(
    const pipeline::Metadata& input_header,
    const pipeline::StageParameters& parameters,
    pipeline::Metadata* output_header) {
    pipeline::StageStatus status = Finish();
    if (!status.ok()) return status;
    if (!output_header) {
        return pipeline::StageStatus::Error("null output header");
    }

    std::string data_stage;
    std::string order;
    std::string text;
    if (!input_header.GetString("DATA_STAGE", &data_stage) ||
        (data_stage != "POWER" && data_stage != "POWER_INTEGRATED" &&
         data_stage != "POLARIZATION_PRODUCTS" &&
         data_stage != "POLARIZATION_PRODUCTS_INTEGRATED")) {
        return pipeline::StageStatus::Error(
            "time integration input DATA_STAGE must be POWER, "
            "POWER_INTEGRATED, POLARIZATION_PRODUCTS, or "
            "POLARIZATION_PRODUCTS_INTEGRATED");
    }
    const bool integrates_power =
        data_stage == "POWER" || data_stage == "POWER_INTEGRATED";
    if (!input_header.GetString("ORDER", &order) ||
        (integrates_power && order != "TFPB") ||
        (!integrates_power && order != "TFBS")) {
        return pipeline::StageStatus::Error(
            "POWER integration requires TFPB and polarization-products "
            "integration requires TFBS");
    }
    if (!input_header.GetString("SAMPLE_FORMAT", &text) || text != "F32") {
        return pipeline::StageStatus::Error(
            "time integration input SAMPLE_FORMAT must be F32");
    }
    std::string backend;
    std::string operation;
    if (!parameters.GetString("EXECUTION_BACKEND", &backend)) {
        return MissingOrInvalid("EXECUTION_BACKEND");
    }
    if (backend == "CPU_REFERENCE") {
        impl_->execution_backend = pipeline::ExecutionBackend::kHost;
        impl_->cuda_device = -1;
    } else if (backend == "CUDA") {
        std::uint64_t cuda_device = 0;
        if (!parameters.GetUint64("CUDA_DEVICE", &cuda_device) ||
            cuda_device > static_cast<std::uint64_t>(
                              std::numeric_limits<int>::max())) {
            return MissingOrInvalid("CUDA_DEVICE");
        }
        impl_->execution_backend = pipeline::ExecutionBackend::kCuda;
        impl_->cuda_device = static_cast<int>(cuda_device);
#if !defined(RDMA_DADA_HAVE_CUDA)
        return pipeline::StageStatus::Error(
            "EXECUTION_BACKEND=CUDA requires a USE_CUDA=ON build");
#endif
    } else {
        return pipeline::StageStatus::Error(
            "EXECUTION_BACKEND must be CPU_REFERENCE or CUDA");
    }
    if (!parameters.GetString("INTEGRATION_OPERATION", &operation) ||
        (operation != "SUM" && operation != "MEAN")) {
        return pipeline::StageStatus::Error(
            "INTEGRATION_OPERATION must be SUM or MEAN");
    }
    impl_->calculate_mean = operation == "MEAN";
    if (!parameters.GetUint64(
            "INTEGRATION_LENGTH", &impl_->integration_length) ||
        impl_->integration_length == 0) {
        return MissingOrInvalid("INTEGRATION_LENGTH");
    }

    std::string input_memory;
    if (!input_header.GetString("MEMORY", &input_memory)) {
        return MissingOrInvalid("MEMORY");
    }
    if (impl_->execution_backend == pipeline::ExecutionBackend::kHost &&
        input_memory != "HOST" && input_memory != "PINNED_HOST") {
        return pipeline::StageStatus::Error(
            "CPU time integration requires MEMORY=HOST or PINNED_HOST");
    }
    if (impl_->execution_backend == pipeline::ExecutionBackend::kCuda &&
        input_memory != "CUDA_DEVICE") {
        return pipeline::StageStatus::Error(
            "CUDA time integration requires MEMORY=CUDA_DEVICE");
    }
    if (impl_->execution_backend == pipeline::ExecutionBackend::kCuda) {
        std::uint64_t header_cuda_device = 0;
        if (!input_header.GetUint64(
                "CUDA_DEVICE", &header_cuda_device) ||
            header_cuda_device != static_cast<std::uint64_t>(
                                      impl_->cuda_device)) {
            return pipeline::StageStatus::Error(
                "input CUDA_DEVICE must match configured CUDA_DEVICE");
        }
    }

    std::uint64_t nchan = 0;
    std::uint64_t nbeam = 0;
    if (!input_header.GetUint64("NCHAN", &nchan) || nchan == 0) {
        return MissingOrInvalid("NCHAN");
    }
    if (!input_header.GetUint64("NBEAM", &nbeam) || nbeam == 0) {
        return MissingOrInvalid("NBEAM");
    }
    std::uint64_t inner_axis = 0;
    if (order == "TFPB") {
        if (!input_header.GetUint64("NPOL", &inner_axis) ||
            inner_axis == 0) {
            return MissingOrInvalid("NPOL");
        }
    } else {
        if (!input_header.GetUint64("NPRODUCT", &inner_axis) ||
            inner_axis != 4) {
            return pipeline::StageStatus::Error(
                "TFBS integration requires NPRODUCT=4");
        }
    }
    if (!CheckedMultiply(nchan, nbeam, &impl_->frame_elements) ||
        !CheckedMultiply(impl_->frame_elements, inner_axis,
                         &impl_->frame_elements) ||
        !CheckedMultiply(impl_->frame_elements, sizeof(float),
                         &impl_->frame_bytes)) {
        return pipeline::StageStatus::Error(
            "time integration frame geometry overflows");
    }
    std::uint64_t record_bytes = 0;
    std::uint64_t resolution = 0;
    if (!input_header.GetUint64("RECORD_BYTES", &record_bytes) ||
        record_bytes != impl_->frame_bytes) {
        return pipeline::StageStatus::Error(
            "RECORD_BYTES must equal one complete input F32 time frame");
    }
    if (!input_header.GetUint64("RESOLUTION", &resolution) ||
        resolution != impl_->frame_bytes) {
        return pipeline::StageStatus::Error(
            "RESOLUTION must equal one complete input F32 time frame");
    }

    double tsamp = 0.0;
    if (!input_header.GetDouble("TSAMP", &tsamp) || tsamp <= 0.0 ||
        !std::isfinite(tsamp)) {
        return MissingOrInvalid("TSAMP");
    }
    const double output_tsamp =
        tsamp * static_cast<double>(impl_->integration_length);
    if (!std::isfinite(output_tsamp)) {
        return pipeline::StageStatus::Error("integrated TSAMP overflows");
    }

    std::uint64_t total_integration_length = impl_->integration_length;
    if (input_header.Has("TOTAL_INTEGRATION_LENGTH")) {
        std::uint64_t upstream_length = 0;
        if (!input_header.GetUint64(
                "TOTAL_INTEGRATION_LENGTH", &upstream_length) ||
            upstream_length == 0 ||
            !CheckedMultiply(upstream_length, impl_->integration_length,
                             &total_integration_length)) {
            return pipeline::StageStatus::Error(
                "TOTAL_INTEGRATION_LENGTH is invalid or overflows");
        }
    }

    if (impl_->execution_backend == pipeline::ExecutionBackend::kCuda) {
#if defined(RDMA_DADA_HAVE_CUDA)
        impl_->cuda_executor = CreateCudaTimeIntegrateExecutor();
        status = impl_->cuda_executor->Configure(
            impl_->cuda_device, impl_->frame_elements,
            impl_->integration_length, impl_->calculate_mean);
        if (!status.ok()) {
            impl_->cuda_executor.reset();
            return status;
        }
#else
        return pipeline::StageStatus::Error(
            "EXECUTION_BACKEND=CUDA requires a USE_CUDA=ON build");
#endif
    }

    *output_header = input_header;
    output_header->SetString(
        "DATA_STAGE",
        integrates_power ?
            "POWER_INTEGRATED" :
            "POLARIZATION_PRODUCTS_INTEGRATED");
    output_header->SetString("INTEGRATION_OPERATION", operation);
    output_header->SetString("EXECUTION_BACKEND", backend);
    output_header->SetUint64(
        "INTEGRATION_LENGTH", impl_->integration_length);
    output_header->SetUint64(
        "TOTAL_INTEGRATION_LENGTH", total_integration_length);
    output_header->SetUint64("RECORD_BYTES", impl_->frame_bytes);
    output_header->SetUint64("RESOLUTION", impl_->frame_bytes);
    output_header->SetDouble("TSAMP", output_tsamp);
    std::uint64_t input_bytes_per_second = 0;
    if (!input_header.GetUint64(
            "BYTES_PER_SECOND", &input_bytes_per_second) ||
        input_bytes_per_second == 0) {
        return MissingOrInvalid("BYTES_PER_SECOND");
    }
    const char* const scaled_fields[] = {
        "BYTES_PER_SECOND", "TRANSFER_SIZE", "FILE_SIZE", "OBS_OFFSET"
    };
    for (std::size_t i = 0;
         i < sizeof(scaled_fields) / sizeof(scaled_fields[0]); ++i) {
        const pipeline::StageStatus status = ScaleByteField(
            input_header, scaled_fields[i], impl_->integration_length,
            i == 0, output_header);
        if (!status.ok()) return status;
    }
    if (impl_->execution_backend == pipeline::ExecutionBackend::kCuda) {
        output_header->SetUint64(
            "CUDA_DEVICE", static_cast<std::uint64_t>(impl_->cuda_device));
    } else {
        output_header->Erase("CUDA_DEVICE");
    }
    impl_->configured = true;
    return pipeline::StageStatus::Ok();
}

pipeline::StageStatus TimeIntegrateModule::ProcessBlock(
    const pipeline::InputBlock& input, pipeline::OutputBlock* output,
    const pipeline::BlockExecutionContext& context) {
    if (!impl_->configured) {
        return pipeline::StageStatus::Error(
            "time integration module is not configured");
    }
    if (!output) return pipeline::StageStatus::Error("null output block");
    if (context.backend != impl_->execution_backend) {
        return pipeline::StageStatus::Error(
            "block execution context does not match configured backend");
    }
    if (!input.data || input.size == 0 ||
        input.size % impl_->frame_bytes != 0) {
        return pipeline::StageStatus::Error(
            "input block does not contain complete F32 time frames");
    }
    const std::uint64_t input_ntime = input.size / impl_->frame_bytes;
    if (input_ntime % impl_->integration_length != 0) {
        return pipeline::StageStatus::Error(
            "input T must be divisible by INTEGRATION_LENGTH");
    }
    const std::uint64_t output_bytes = input.size / impl_->integration_length;
    if (!output->data || output->capacity < output_bytes) {
        return pipeline::StageStatus::Error(
            "time integration output block is too small");
    }
    if (RangesOverlap(input.data, input.size, output->data, output_bytes)) {
        return pipeline::StageStatus::Error(
            "time integration input and output blocks must not overlap");
    }

    const std::uint64_t output_ntime =
        input_ntime / impl_->integration_length;
    std::uint64_t output_element_count = 0;
    if (!CheckedMultiply(output_ntime, impl_->frame_elements,
                         &output_element_count)) {
        return pipeline::StageStatus::Error(
            "time integration output element count overflows");
    }
    if (impl_->execution_backend == pipeline::ExecutionBackend::kCuda) {
        if (context.device_id != impl_->cuda_device ||
            context.native_stream == NULL) {
            return pipeline::StageStatus::Error(
                "CUDA execution requires the configured device and a "
                "worker-owned stream");
        }
        if (input.location != pipeline::MemoryLocation::kCudaDevice ||
            output->location != pipeline::MemoryLocation::kCudaDevice) {
            return pipeline::StageStatus::Error(
                "CUDA time integration requires CUDA device blocks");
        }
        if (!impl_->cuda_executor) {
            return pipeline::StageStatus::Error(
                "CUDA time-integration executor is unavailable");
        }
        const pipeline::StageStatus cuda_status =
            impl_->cuda_executor->Process(
                input, output, output_element_count, context);
        if (!cuda_status.ok()) return cuda_status;
    } else {
        if (context.device_id != -1 || context.native_stream != NULL) {
            return pipeline::StageStatus::Error(
                "CPU time integration requires a host execution context");
        }
        if (!IsHostMemory(input.location) ||
            !IsHostMemory(output->location)) {
            return pipeline::StageStatus::Error(
                "CPU time integration requires host or pinned-host blocks");
        }
        const float* source = reinterpret_cast<const float*>(input.data);
        float* destination = reinterpret_cast<float*>(output->data);
        for (std::uint64_t output_time = 0;
             output_time < output_ntime; ++output_time) {
            for (std::uint64_t element = 0;
                 element < impl_->frame_elements; ++element) {
                float sum = 0.0f;
                for (std::uint64_t integration_index = 0;
                     integration_index < impl_->integration_length;
                     ++integration_index) {
                    const std::uint64_t input_time =
                        output_time * impl_->integration_length +
                        integration_index;
                    sum += source[
                        input_time * impl_->frame_elements + element];
                }
                destination[output_time * impl_->frame_elements + element] =
                    impl_->calculate_mean ?
                        sum / static_cast<float>(impl_->integration_length) :
                        sum;
            }
        }
    }

    output->size = output_bytes;
    output->sequence = input.sequence;
    return pipeline::StageStatus::Ok();
}

pipeline::StageStatus TimeIntegrateModule::Finish() {
    pipeline::StageStatus status = pipeline::StageStatus::Ok();
    if (impl_->cuda_executor) {
        status = impl_->cuda_executor->Finish();
        impl_->cuda_executor.reset();
    }
    impl_->configured = false;
    impl_->integration_length = 0;
    impl_->frame_elements = 0;
    impl_->frame_bytes = 0;
    impl_->calculate_mean = false;
    impl_->execution_backend = pipeline::ExecutionBackend::kHost;
    impl_->cuda_device = -1;
    return status;
}

}  // namespace time_integrate
}  // namespace modules
}  // namespace rdma_dada
