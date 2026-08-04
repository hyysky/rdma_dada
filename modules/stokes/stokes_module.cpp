#include "rdma_dada/modules/stokes/stokes_module.h"

#include "rdma_dada/pipeline/complex32.h"
#include "stokes_backend.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace rdma_dada {
namespace modules {
namespace stokes {
namespace {

const std::uint64_t kRequiredPolarizations = 2;
const std::uint64_t kProductCount = 4;
const std::uint64_t kComplex32Bytes = sizeof(pipeline::Complex32);
const std::uint64_t kFloat32Bytes = sizeof(float);

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t* result) {
    if (!result) return false;
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    *result = left * right;
    return true;
}

pipeline::StageStatus MissingOrInvalid(const std::string& name) {
    return pipeline::StageStatus::Error("missing or invalid " + name);
}

bool IsHostMemory(pipeline::MemoryLocation location) {
    return location == pipeline::MemoryLocation::kHost ||
           location == pipeline::MemoryLocation::kPinnedHost;
}

bool ContainsNonWhitespace(const std::string& text,
                           std::size_t begin, std::size_t end) {
    for (std::size_t i = begin; i < end; ++i) {
        if (text[i] != ' ' && text[i] != '\t') return true;
    }
    return false;
}

bool HasTwoPolarizationLabels(const std::string& labels) {
    const std::size_t comma = labels.find(',');
    return comma != std::string::npos &&
           labels.find(',', comma + 1) == std::string::npos &&
           ContainsNonWhitespace(labels, 0, comma) &&
           ContainsNonWhitespace(labels, comma + 1, labels.size());
}

bool RangesOverlap(const std::uint8_t* input, std::uint64_t input_bytes,
                   const std::uint8_t* output, std::uint64_t output_bytes) {
    const std::uintptr_t input_begin =
        reinterpret_cast<std::uintptr_t>(input);
    const std::uintptr_t output_begin =
        reinterpret_cast<std::uintptr_t>(output);
    const std::uintptr_t max_address =
        std::numeric_limits<std::uintptr_t>::max();
    if (input_bytes > max_address - input_begin ||
        output_bytes > max_address - output_begin) {
        return true;
    }
    const std::uintptr_t input_end =
        input_begin + static_cast<std::uintptr_t>(input_bytes);
    const std::uintptr_t output_end =
        output_begin + static_cast<std::uintptr_t>(output_bytes);
    return input_begin < output_end && output_begin < input_end;
}

}  // namespace

class StokesModule::Impl {
public:
    Impl()
        : configured(false),
          execution_backend(pipeline::ExecutionBackend::kHost),
          cuda_device(-1), nchan(0), npol(0), nbeam(0),
          input_frame_bytes(0), output_frame_bytes(0) {}

    bool configured;
    pipeline::ExecutionBackend execution_backend;
    int cuda_device;
    std::uint64_t nchan;
    std::uint64_t npol;
    std::uint64_t nbeam;
    std::uint64_t input_frame_bytes;
    std::uint64_t output_frame_bytes;
    std::unique_ptr<CudaStokesExecutor> cuda_executor;
};

StokesModule::StokesModule() : impl_(new Impl) {}

StokesModule::~StokesModule() {}

const char* StokesModule::Name() const { return "stokes"; }

pipeline::StageStatus StokesModule::ConfigureHeader(
    const pipeline::Metadata& input_header,
    const pipeline::StageParameters& parameters,
    pipeline::Metadata* output_header) {
    impl_->configured = false;
    if (impl_->cuda_executor) {
        const pipeline::StageStatus finish_status =
            impl_->cuda_executor->Finish();
        impl_->cuda_executor.reset();
        if (!finish_status.ok()) return finish_status;
    }
    if (!output_header) {
        return pipeline::StageStatus::Error("null output header");
    }

    std::string data_stage;
    std::string order;
    std::string sample_format;
    std::string polarization_labels;
    if (!input_header.GetString("DATA_STAGE", &data_stage) ||
        data_stage != "BEAMFORMED") {
        return pipeline::StageStatus::Error(
            "stokes input DATA_STAGE must be BEAMFORMED");
    }
    if (!input_header.GetString("ORDER", &order) || order != "TFPB") {
        return pipeline::StageStatus::Error("stokes input ORDER must be TFPB");
    }
    if (!input_header.GetString("SAMPLE_FORMAT", &sample_format) ||
        sample_format != "CF32") {
        return pipeline::StageStatus::Error(
            "stokes input SAMPLE_FORMAT must be CF32");
    }
    if (!input_header.GetString("POL_LABELS", &polarization_labels) ||
        !HasTwoPolarizationLabels(polarization_labels)) {
        return MissingOrInvalid("POL_LABELS");
    }
    if (!input_header.GetUint64("NCHAN", &impl_->nchan) ||
        impl_->nchan == 0) {
        return MissingOrInvalid("NCHAN");
    }
    if (!input_header.GetUint64("NPOL", &impl_->npol) ||
        impl_->npol != kRequiredPolarizations) {
        return pipeline::StageStatus::Error(
            "stokes requires NPOL=2");
    }
    if (!input_header.GetUint64("NBEAM", &impl_->nbeam) ||
        impl_->nbeam == 0) {
        return MissingOrInvalid("NBEAM");
    }

    std::string execution_backend;
    if (!parameters.GetString("EXECUTION_BACKEND", &execution_backend)) {
        return MissingOrInvalid("EXECUTION_BACKEND");
    }
    if (execution_backend == "CPU_REFERENCE") {
        impl_->execution_backend = pipeline::ExecutionBackend::kHost;
        impl_->cuda_device = -1;
    } else if (execution_backend == "CUDA") {
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

    std::uint64_t elements_per_frame = 0;
    if (!CheckedMultiply(impl_->nchan, impl_->npol, &elements_per_frame) ||
        !CheckedMultiply(elements_per_frame, impl_->nbeam,
                         &elements_per_frame) ||
        !CheckedMultiply(elements_per_frame, kComplex32Bytes,
                         &impl_->input_frame_bytes) ||
        !CheckedMultiply(impl_->nchan, impl_->nbeam, &elements_per_frame) ||
        !CheckedMultiply(elements_per_frame, kProductCount,
                         &elements_per_frame) ||
        !CheckedMultiply(elements_per_frame, kFloat32Bytes,
                         &impl_->output_frame_bytes)) {
        return pipeline::StageStatus::Error("stokes frame geometry overflows");
    }

    bool has_byte_rate = false;
    std::uint64_t byte_rate = 0;
    if (input_header.Has("BYTES_PER_SECOND")) {
        if (!input_header.GetUint64("BYTES_PER_SECOND", &byte_rate) ||
            byte_rate == 0) {
            return MissingOrInvalid("BYTES_PER_SECOND");
        }
        has_byte_rate = true;
    }

    std::string input_memory;
    if (!input_header.GetString("MEMORY", &input_memory)) {
        return MissingOrInvalid("MEMORY");
    }
    if (impl_->execution_backend == pipeline::ExecutionBackend::kHost &&
        input_memory != "HOST" && input_memory != "PINNED_HOST") {
        return pipeline::StageStatus::Error(
            "CPU_REFERENCE stokes requires MEMORY=HOST or PINNED_HOST");
    }
    if (impl_->execution_backend == pipeline::ExecutionBackend::kCuda &&
        input_memory != "CUDA_DEVICE") {
        return pipeline::StageStatus::Error(
            "CUDA stokes requires MEMORY=CUDA_DEVICE");
    }

    if (impl_->execution_backend == pipeline::ExecutionBackend::kCuda) {
#if defined(RDMA_DADA_HAVE_CUDA)
        impl_->cuda_executor = CreateCudaStokesExecutor();
        const pipeline::StageStatus cuda_status =
            impl_->cuda_executor->Configure(
                impl_->cuda_device, impl_->nbeam);
        if (!cuda_status.ok()) {
            impl_->cuda_executor.reset();
            return cuda_status;
        }
#else
        return pipeline::StageStatus::Error(
            "EXECUTION_BACKEND=CUDA requires a USE_CUDA=ON build");
#endif
    }

    *output_header = input_header;
    output_header->SetString("DATA_STAGE", "POLARIZATION_PRODUCTS");
    output_header->SetString("ORDER", "TFBS");
    output_header->SetString("SAMPLE_FORMAT", "F32");
    output_header->SetString("PRODUCTS", "AA,BB,AB_REAL,AB_IMAG");
    output_header->SetString("EXECUTION_BACKEND", execution_backend);
    output_header->SetUint64("NPRODUCT", kProductCount);
    output_header->SetUint64("COMPONENT_NBIT", 32);
    output_header->SetUint64("SAMPLE_NBIT", 32);
    output_header->SetUint64("RECORD_BYTES", impl_->output_frame_bytes);
    output_header->SetUint64("RESOLUTION", impl_->output_frame_bytes);
    if (has_byte_rate) {
        output_header->SetUint64("BYTES_PER_SECOND", byte_rate);
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

pipeline::StageStatus StokesModule::ProcessBlock(
    const pipeline::InputBlock& input, pipeline::OutputBlock* output,
    const pipeline::BlockExecutionContext& context) {
    if (!impl_->configured) {
        return pipeline::StageStatus::Error("stokes module is not configured");
    }
    if (!output) return pipeline::StageStatus::Error("null output block");
    if (context.backend != impl_->execution_backend) {
        return pipeline::StageStatus::Error(
            "block execution context does not match configured backend");
    }
    if (input.size == 0 || input.size % impl_->input_frame_bytes != 0) {
        return pipeline::StageStatus::Error(
            "input block does not contain complete TFPB time frames");
    }
    if (!input.data) return pipeline::StageStatus::Error("null input data");

    const std::uint64_t ntime = input.size / impl_->input_frame_bytes;
    std::uint64_t required_output_bytes = 0;
    std::uint64_t product_group_count = 0;
    if (!CheckedMultiply(ntime, impl_->output_frame_bytes,
                         &required_output_bytes) ||
        !CheckedMultiply(ntime, impl_->nchan, &product_group_count) ||
        !CheckedMultiply(product_group_count, impl_->nbeam,
                         &product_group_count)) {
        return pipeline::StageStatus::Error("output block geometry overflows");
    }
    if (!output->data || output->capacity < required_output_bytes) {
        return pipeline::StageStatus::Error("output block is too small");
    }
    if (RangesOverlap(input.data, input.size, output->data,
                      required_output_bytes)) {
        return pipeline::StageStatus::Error(
            "stokes input and output blocks must not overlap");
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
                "CUDA stokes requires CUDA device input and output blocks");
        }
        if (!impl_->cuda_executor) {
            return pipeline::StageStatus::Error(
                "CUDA stokes executor is unavailable");
        }
        const pipeline::StageStatus cuda_status =
            impl_->cuda_executor->Process(
                input, output, product_group_count, context);
        if (!cuda_status.ok()) return cuda_status;
    } else {
        if (context.device_id != -1 || context.native_stream != NULL) {
            return pipeline::StageStatus::Error(
                "host reference backend requires a host execution context");
        }
        if (!IsHostMemory(input.location) || !IsHostMemory(output->location)) {
            return pipeline::StageStatus::Error(
                "host reference backend requires host or pinned-host blocks");
        }

        const pipeline::Complex32* source =
            reinterpret_cast<const pipeline::Complex32*>(input.data);
        float* destination = reinterpret_cast<float*>(output->data);
        for (std::uint64_t group = 0; group < product_group_count; ++group) {
            const std::uint64_t tf = group / impl_->nbeam;
            const std::uint64_t beam = group % impl_->nbeam;
            const std::uint64_t x_index =
                tf * kRequiredPolarizations * impl_->nbeam + beam;
            const std::uint64_t y_index = x_index + impl_->nbeam;
            const pipeline::Complex32 x = source[x_index];
            const pipeline::Complex32 y = source[y_index];
            const std::uint64_t output_index = group * kProductCount;
            destination[output_index] = x.real * x.real + x.imag * x.imag;
            destination[output_index + 1] =
                y.real * y.real + y.imag * y.imag;
            destination[output_index + 2] =
                x.real * y.real + x.imag * y.imag;
            destination[output_index + 3] =
                x.imag * y.real - x.real * y.imag;
        }
    }

    output->size = required_output_bytes;
    output->sequence = input.sequence;
    return pipeline::StageStatus::Ok();
}

pipeline::StageStatus StokesModule::Finish() {
    pipeline::StageStatus status = pipeline::StageStatus::Ok();
    if (impl_->cuda_executor) {
        status = impl_->cuda_executor->Finish();
        impl_->cuda_executor.reset();
    }
    impl_->configured = false;
    return status;
}

}  // namespace stokes
}  // namespace modules
}  // namespace rdma_dada
