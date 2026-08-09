#include "rdma_dada/modules/complex_convert/complex_convert_module.h"

#include "complex_convert_backend.h"
#include "rdma_dada/pipeline/complex32.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace rdma_dada {
namespace modules {
namespace complex_convert {
namespace {

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
    std::uint64_t alignment_bytes, std::uint64_t input_sample_bytes,
    bool required, pipeline::Metadata* output) {
    if (!input.Has(name)) {
        return required ? MissingOrInvalid(name) : pipeline::StageStatus::Ok();
    }
    std::uint64_t input_value = 0;
    if (!input.GetUint64(name, &input_value) ||
        (required && input_value == 0) ||
        input_value % alignment_bytes != 0 ||
        input_value % input_sample_bytes != 0) {
        return pipeline::StageStatus::Error(
            name + " is not aligned to the ATFP block geometry");
    }
    std::uint64_t output_value = 0;
    if (!CheckedMultiply(input_value / input_sample_bytes,
                         sizeof(pipeline::Complex32), &output_value)) {
        return pipeline::StageStatus::Error(
            name + " output scaling overflows");
    }
    output->SetUint64(name, output_value);
    return pipeline::StageStatus::Ok();
}

}  // namespace

class ComplexConvertModule::Impl {
public:
    Impl()
        : configured(false), scale(0.0f), component_bits(0),
          input_sample_bytes(0), nant(0), elements_per_antenna(0),
          input_frame_bytes(0), output_frame_bytes(0),
          input_block_bytes(0), output_block_bytes(0),
          execution_backend(pipeline::ExecutionBackend::kHost),
          cuda_device(-1) {}

    bool configured;
    float scale;
    std::uint64_t component_bits;
    std::uint64_t input_sample_bytes;
    std::uint64_t nant;
    std::uint64_t elements_per_antenna;
    std::uint64_t input_frame_bytes;
    std::uint64_t output_frame_bytes;
    std::uint64_t input_block_bytes;
    std::uint64_t output_block_bytes;
    pipeline::ExecutionBackend execution_backend;
    int cuda_device;
    std::unique_ptr<CudaComplexConvertExecutor> cuda_executor;
};

ComplexConvertModule::ComplexConvertModule() : impl_(new Impl) {}

ComplexConvertModule::~ComplexConvertModule() {}

const char* ComplexConvertModule::Name() const { return "complex_convert"; }

pipeline::StageStatus ComplexConvertModule::ConfigureHeader(
    const pipeline::Metadata& input_header,
    const pipeline::StageParameters& parameters,
    pipeline::Metadata* output_header) {
    pipeline::StageStatus status = Finish();
    if (!status.ok()) return status;
    if (!output_header) {
        return pipeline::StageStatus::Error("null output header");
    }

    std::string text;
    if (!input_header.GetString("DATA_STAGE", &text) || text != "UNPACKED") {
        return pipeline::StageStatus::Error(
            "complex conversion input DATA_STAGE must be UNPACKED");
    }
    if (!input_header.GetString("ORDER", &text) || text != "ATFP") {
        return pipeline::StageStatus::Error(
            "complex conversion input ORDER must be ATFP");
    }
    if (!input_header.GetString("LAYOUT_SCOPE", &text) || text != "BLOCK") {
        return pipeline::StageStatus::Error(
            "complex conversion input LAYOUT_SCOPE must be BLOCK");
    }
    std::string source_sample_format;
    if (!input_header.GetString("SAMPLE_FORMAT", &source_sample_format) ||
        (source_sample_format != "CI8" && source_sample_format != "CI16")) {
        return pipeline::StageStatus::Error(
            "complex conversion SAMPLE_FORMAT must be CI8 or CI16");
    }
    impl_->component_bits = source_sample_format == "CI8" ? 8 : 16;
    impl_->input_sample_bytes = impl_->component_bits / 4;
    if (!input_header.GetString("SAMPLE_ENCODING", &text) ||
        text != "TWOS_COMPLEMENT") {
        return pipeline::StageStatus::Error(
            "complex conversion input SAMPLE_ENCODING must be "
            "TWOS_COMPLEMENT");
    }
    if (!input_header.GetString("COMPONENT_ORDER", &text) || text != "IQ") {
        return pipeline::StageStatus::Error(
            "complex conversion input COMPONENT_ORDER must be IQ");
    }
    if (!input_header.GetString("ENDIAN", &text) || text != "LITTLE") {
        return pipeline::StageStatus::Error(
            "complex conversion input ENDIAN must be LITTLE");
    }
    std::uint64_t number = 0;
    if (!input_header.GetUint64("COMPONENT_NBIT", &number) ||
        number != impl_->component_bits) {
        return pipeline::StageStatus::Error(
            "COMPONENT_NBIT does not match SAMPLE_FORMAT");
    }
    if (!input_header.GetUint64("SAMPLE_NBIT", &number) ||
        number != 2 * impl_->component_bits) {
        return pipeline::StageStatus::Error(
            "SAMPLE_NBIT does not match SAMPLE_FORMAT");
    }
    std::uint64_t nchan = 0;
    std::uint64_t npol = 0;
    if (!input_header.GetUint64("NCHAN", &nchan) || nchan == 0) {
        return MissingOrInvalid("NCHAN");
    }
    if (!input_header.GetUint64("NPOL", &npol) || npol == 0) {
        return MissingOrInvalid("NPOL");
    }
    if (!input_header.GetUint64("NANT", &impl_->nant) || impl_->nant == 0) {
        return MissingOrInvalid("NANT");
    }
    if (!CheckedMultiply(nchan, npol, &impl_->elements_per_antenna)) {
        return pipeline::StageStatus::Error(
            "complex conversion per-antenna geometry overflows");
    }
    std::uint64_t element_count = 0;
    if (!CheckedMultiply(impl_->nant, impl_->elements_per_antenna,
                         &element_count)) {
        return pipeline::StageStatus::Error(
            "complex conversion frame geometry overflows");
    }
    if (!CheckedMultiply(element_count, impl_->input_sample_bytes,
                         &impl_->input_frame_bytes) ||
        !CheckedMultiply(element_count, sizeof(pipeline::Complex32),
                         &impl_->output_frame_bytes)) {
        return pipeline::StageStatus::Error(
            "complex conversion frame geometry overflows");
    }
    if (!input_header.GetUint64("RESOLUTION", &number) ||
        number != impl_->input_frame_bytes) {
        return pipeline::StageStatus::Error(
            "RESOLUTION must equal one complete ATFP time frame");
    }
    std::uint64_t block_ntime = 0;
    if (!input_header.GetUint64("BLOCK_NTIME", &block_ntime) ||
        block_ntime == 0 ||
        !CheckedMultiply(block_ntime, impl_->input_frame_bytes,
                         &impl_->input_block_bytes) ||
        !CheckedMultiply(block_ntime, impl_->output_frame_bytes,
                         &impl_->output_block_bytes)) {
        return MissingOrInvalid("BLOCK_NTIME");
    }
    if (!input_header.GetUint64("OUTPUT_BLOCK_BYTES", &number) ||
        number != impl_->input_block_bytes) {
        return pipeline::StageStatus::Error(
            "OUTPUT_BLOCK_BYTES must match nominal ATFP block geometry");
    }
    if (!input_header.GetUint64("RECORD_BYTES", &number) ||
        number != impl_->input_block_bytes) {
        return pipeline::StageStatus::Error(
            "RECORD_BYTES must equal OUTPUT_BLOCK_BYTES");
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

    std::string input_memory;
    if (!input_header.GetString("MEMORY", &input_memory)) {
        return MissingOrInvalid("MEMORY");
    }
    if (impl_->execution_backend == pipeline::ExecutionBackend::kHost &&
        input_memory != "HOST" && input_memory != "PINNED_HOST") {
        return pipeline::StageStatus::Error(
            "CPU_REFERENCE complex conversion requires MEMORY=HOST or "
            "PINNED_HOST");
    }
    if (impl_->execution_backend == pipeline::ExecutionBackend::kCuda &&
        input_memory != "CUDA_DEVICE") {
        return pipeline::StageStatus::Error(
            "CUDA complex conversion requires MEMORY=CUDA_DEVICE");
    }
    if (impl_->execution_backend == pipeline::ExecutionBackend::kCuda) {
        std::uint64_t header_cuda_device = 0;
        if (!input_header.GetUint64("CUDA_DEVICE", &header_cuda_device) ||
            header_cuda_device !=
                static_cast<std::uint64_t>(impl_->cuda_device)) {
            return pipeline::StageStatus::Error(
                "input CUDA_DEVICE must match configured CUDA_DEVICE");
        }
    }
    double scale = 0.0;
    if (!parameters.GetDouble("CONVERSION_SCALE", &scale) || scale <= 0.0 ||
        !std::isfinite(scale) ||
        scale > static_cast<double>(std::numeric_limits<float>::max())) {
        return MissingOrInvalid("CONVERSION_SCALE");
    }
    const float float_scale = static_cast<float>(scale);
    if (!(float_scale > 0.0f) || !std::isfinite(float_scale)) {
        return pipeline::StageStatus::Error(
            "CONVERSION_SCALE is not representable as positive FP32");
    }
    impl_->scale = float_scale;

    if (impl_->execution_backend == pipeline::ExecutionBackend::kCuda) {
#if defined(RDMA_DADA_HAVE_CUDA)
        impl_->cuda_executor = CreateCudaComplexConvertExecutor();
        status = impl_->cuda_executor->Configure(
            impl_->cuda_device, impl_->component_bits, impl_->scale);
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
    output_header->SetString("DATA_STAGE", "CONVERTED");
    output_header->SetString("ORDER", "TFPA");
    output_header->SetString("SOURCE_ORDER", "ATFP");
    output_header->SetString("SAMPLE_FORMAT", "CF32");
    output_header->SetString("COMPONENT_ORDER", "RI");
    output_header->SetString("EXECUTION_BACKEND", execution_backend);
    output_header->SetString("SOURCE_SAMPLE_FORMAT", source_sample_format);
    output_header->SetString("SOURCE_SAMPLE_ENCODING", "TWOS_COMPLEMENT");
    output_header->SetString("SOURCE_COMPONENT_ORDER", "IQ");
    output_header->SetUint64("SOURCE_COMPONENT_NBIT", impl_->component_bits);
    output_header->Erase("COMPONENT_SIGNED");
    output_header->Erase("SOURCE_COMPONENT_SIGNED");
    output_header->Erase("SAMPLE_ENCODING");
    output_header->SetUint64("COMPONENT_NBIT", 32);
    output_header->SetUint64("SAMPLE_NBIT", 64);
    output_header->SetUint64("RECORD_BYTES", impl_->output_block_bytes);
    output_header->SetUint64("OUTPUT_BLOCK_BYTES", impl_->output_block_bytes);
    output_header->SetUint64("RESOLUTION", impl_->output_frame_bytes);
    output_header->SetDouble("CONVERSION_SCALE", scale);
    if (impl_->execution_backend == pipeline::ExecutionBackend::kCuda) {
        output_header->SetUint64(
            "CUDA_DEVICE", static_cast<std::uint64_t>(impl_->cuda_device));
    } else {
        output_header->Erase("CUDA_DEVICE");
    }
    const char* const frame_aligned_fields[] = {
        "BYTES_PER_SECOND", "TRANSFER_SIZE", "FILE_SIZE", "OBS_OFFSET"
    };
    for (std::size_t i = 0;
         i < sizeof(frame_aligned_fields) / sizeof(frame_aligned_fields[0]);
         ++i) {
        const pipeline::StageStatus status = ScaleByteField(
            input_header, frame_aligned_fields[i], impl_->input_frame_bytes,
            impl_->input_sample_bytes, i == 0, output_header);
        if (!status.ok()) return status;
    }
    impl_->configured = true;
    return pipeline::StageStatus::Ok();
}

pipeline::StageStatus ComplexConvertModule::ProcessBlock(
    const pipeline::InputBlock& input, pipeline::OutputBlock* output,
    const pipeline::BlockExecutionContext& context) {
    if (!impl_->configured) {
        return pipeline::StageStatus::Error(
            "complex conversion module is not configured");
    }
    if (!output) return pipeline::StageStatus::Error("null output block");
    if (context.backend != impl_->execution_backend) {
        return pipeline::StageStatus::Error(
            "block execution context does not match configured backend");
    }
    if (!input.data || input.size == 0 ||
        input.size % impl_->input_frame_bytes != 0) {
        return pipeline::StageStatus::Error(
            "input block does not contain complete ATFP time frames");
    }
    const std::uint64_t frame_count = input.size / impl_->input_frame_bytes;
    std::uint64_t output_bytes = 0;
    if (!CheckedMultiply(frame_count, impl_->output_frame_bytes,
                         &output_bytes)) {
        return pipeline::StageStatus::Error(
            "complex conversion output geometry overflows");
    }
    if (!output->data || output->capacity < output_bytes) {
        return pipeline::StageStatus::Error(
            "complex conversion output block is too small");
    }
    if (RangesOverlap(input.data, input.size, output->data, output_bytes)) {
        return pipeline::StageStatus::Error(
            "complex conversion input and output blocks must not overlap");
    }

    const std::uint64_t sample_count = input.size / impl_->input_sample_bytes;
    const std::uint64_t q = sample_count / impl_->nant;
    if (impl_->execution_backend == pipeline::ExecutionBackend::kCuda) {
        if (context.device_id != impl_->cuda_device ||
            context.native_stream == NULL) {
            return pipeline::StageStatus::Error(
                "CUDA execution requires the configured device and a "
                "worker-owned non-default stream");
        }
        if (input.location != pipeline::MemoryLocation::kCudaDevice ||
            output->location != pipeline::MemoryLocation::kCudaDevice) {
            return pipeline::StageStatus::Error(
                "CUDA complex conversion requires CUDA device blocks");
        }
        if (!impl_->cuda_executor) {
            return pipeline::StageStatus::Error(
                "CUDA complex conversion executor is unavailable");
        }
        const pipeline::StageStatus cuda_status =
            impl_->cuda_executor->Process(
                input, output, impl_->nant, q, context);
        if (!cuda_status.ok()) return cuda_status;
    } else {
        if (context.device_id != -1 || context.native_stream != NULL) {
            return pipeline::StageStatus::Error(
                "CPU_REFERENCE complex conversion requires a host context");
        }
        const bool input_is_host =
            input.location == pipeline::MemoryLocation::kHost ||
            input.location == pipeline::MemoryLocation::kPinnedHost;
        const bool output_is_host =
            output->location == pipeline::MemoryLocation::kHost ||
            output->location == pipeline::MemoryLocation::kPinnedHost;
        if (!input_is_host || !output_is_host) {
            return pipeline::StageStatus::Error(
                "CPU_REFERENCE complex conversion requires host blocks");
        }

        pipeline::Complex32* destination =
            reinterpret_cast<pipeline::Complex32*>(output->data);
        if (impl_->component_bits == 8) {
            const std::int8_t* source =
                reinterpret_cast<const std::int8_t*>(input.data);
            for (std::uint64_t a = 0; a < impl_->nant; ++a) {
                for (std::uint64_t q_index = 0; q_index < q; ++q_index) {
                    const std::uint64_t source_index = a * q + q_index;
                    const std::uint64_t destination_index =
                        q_index * impl_->nant + a;
                    destination[destination_index].real =
                        static_cast<float>(source[2 * source_index]) *
                        impl_->scale;
                    destination[destination_index].imag =
                        static_cast<float>(source[2 * source_index + 1]) *
                        impl_->scale;
                }
            }
        } else {
            for (std::uint64_t a = 0; a < impl_->nant; ++a) {
                for (std::uint64_t q_index = 0; q_index < q; ++q_index) {
                    const std::uint64_t source_index = a * q + q_index;
                    const std::uint64_t destination_index =
                        q_index * impl_->nant + a;
                    std::int16_t real = 0;
                    std::int16_t imag = 0;
                    std::memcpy(&real, input.data + 4 * source_index,
                                sizeof(real));
                    std::memcpy(&imag,
                                input.data + 4 * source_index + sizeof(real),
                                sizeof(imag));
                    destination[destination_index].real =
                        static_cast<float>(real) * impl_->scale;
                    destination[destination_index].imag =
                        static_cast<float>(imag) * impl_->scale;
                }
            }
        }
    }
    output->size = output_bytes;
    output->sequence = input.sequence;
    return pipeline::StageStatus::Ok();
}

pipeline::StageStatus ComplexConvertModule::Finish() {
    pipeline::StageStatus status = pipeline::StageStatus::Ok();
    if (impl_->cuda_executor) {
        status = impl_->cuda_executor->Finish();
        impl_->cuda_executor.reset();
    }
    impl_->configured = false;
    impl_->cuda_device = -1;
    return status;
}

}  // namespace complex_convert
}  // namespace modules
}  // namespace rdma_dada
