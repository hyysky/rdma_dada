#include "rdma_dada/modules/beamform/beamform_module.h"

#include "beamform_backend.h"

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace rdma_dada {
namespace modules {
namespace beamform {
namespace {

const std::uint64_t kComplex32Bytes = 2 * sizeof(float);
const std::uint64_t kMaxNpyHeaderBytes = 1024 * 1024;

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t* result) {
    if (!result) return false;
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    *result = left * right;
    return true;
}

bool Product(const std::vector<std::uint64_t>& values,
             std::uint64_t* result) {
    std::uint64_t product = 1;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] == 0 || !CheckedMultiply(product, values[i], &product)) {
            return false;
        }
    }
    *result = product;
    return true;
}

bool FindDictionaryValue(const std::string& header, const std::string& key,
                         std::size_t* value_position) {
    const std::string single_quoted = "'" + key + "'";
    const std::string double_quoted = "\"" + key + "\"";
    std::size_t position = header.find(single_quoted);
    if (position == std::string::npos) position = header.find(double_quoted);
    if (position == std::string::npos) return false;
    position = header.find(':', position + key.size() + 2);
    if (position == std::string::npos) return false;
    ++position;
    while (position < header.size() &&
           (header[position] == ' ' || header[position] == '\t')) {
        ++position;
    }
    if (position == header.size()) return false;
    *value_position = position;
    return true;
}

bool ParseQuotedValue(const std::string& header, const std::string& key,
                      std::string* value) {
    std::size_t position = 0;
    if (!FindDictionaryValue(header, key, &position)) return false;
    const char quote = header[position];
    if (quote != '\'' && quote != '"') return false;
    const std::size_t end = header.find(quote, position + 1);
    if (end == std::string::npos) return false;
    *value = header.substr(position + 1, end - position - 1);
    return true;
}

bool ParseFortranOrder(const std::string& header, bool* fortran_order) {
    std::size_t position = 0;
    if (!FindDictionaryValue(header, "fortran_order", &position)) return false;
    if (header.compare(position, 4, "True") == 0) {
        *fortran_order = true;
        return true;
    }
    if (header.compare(position, 5, "False") == 0) {
        *fortran_order = false;
        return true;
    }
    return false;
}

bool ParseShape(const std::string& header,
                std::vector<std::uint64_t>* shape) {
    std::size_t position = 0;
    if (!FindDictionaryValue(header, "shape", &position) ||
        header[position] != '(') {
        return false;
    }
    const std::size_t end = header.find(')', position + 1);
    if (end == std::string::npos) return false;

    shape->clear();
    ++position;
    while (position < end) {
        while (position < end &&
               (header[position] == ' ' || header[position] == '\t' ||
                header[position] == ',')) {
            ++position;
        }
        if (position == end) break;
        if (header[position] < '0' || header[position] > '9') return false;

        errno = 0;
        char* number_end = NULL;
        const unsigned long long number =
            std::strtoull(header.c_str() + position, &number_end, 10);
        if (errno == ERANGE || number_end == header.c_str() + position ||
            number == 0) {
            return false;
        }
        const std::size_t parsed_end =
            static_cast<std::size_t>(number_end - header.c_str());
        if (parsed_end > end) return false;
        shape->push_back(static_cast<std::uint64_t>(number));
        position = parsed_end;
        while (position < end &&
               (header[position] == ' ' || header[position] == '\t')) {
            ++position;
        }
        if (position < end && header[position] != ',') return false;
    }
    return !shape->empty();
}

struct LoadedWeights {
    std::string input_dtype;
    std::vector<std::uint64_t> shape;
    std::vector<Complex32> values;
};

bool LoadNpyWeights(const std::string& path, double scale,
                    LoadedWeights* result, std::string* error) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) {
        *error = "cannot open WEIGHTS_FILE: " + path;
        return false;
    }

    unsigned char prefix[8] = {};
    input.read(reinterpret_cast<char*>(prefix), sizeof(prefix));
    const unsigned char expected_magic[] = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    for (std::size_t i = 0; i < sizeof(expected_magic); ++i) {
        if (!input || prefix[i] != expected_magic[i]) {
            *error = "WEIGHTS_FILE is not a NumPy NPY file";
            return false;
        }
    }

    const unsigned int major = prefix[6];
    std::size_t length_bytes = 0;
    if (major == 1) {
        length_bytes = 2;
    } else if (major == 2) {
        length_bytes = 4;
    } else {
        *error = "unsupported NPY version; expected version 1 or 2";
        return false;
    }

    unsigned char encoded_length[4] = {};
    input.read(reinterpret_cast<char*>(encoded_length),
               static_cast<std::streamsize>(length_bytes));
    if (!input) {
        *error = "truncated NPY header length";
        return false;
    }
    std::uint64_t header_bytes = 0;
    for (std::size_t i = 0; i < length_bytes; ++i) {
        header_bytes |= static_cast<std::uint64_t>(encoded_length[i]) << (8 * i);
    }
    if (header_bytes == 0 || header_bytes > kMaxNpyHeaderBytes) {
        *error = "invalid or excessive NPY header length";
        return false;
    }

    std::string header(static_cast<std::size_t>(header_bytes), '\0');
    input.read(&header[0], static_cast<std::streamsize>(header.size()));
    if (!input) {
        *error = "truncated NPY dictionary header";
        return false;
    }

    bool fortran_order = false;
    if (!ParseQuotedValue(header, "descr", &result->input_dtype) ||
        !ParseFortranOrder(header, &fortran_order) ||
        !ParseShape(header, &result->shape)) {
        *error = "malformed NPY dictionary header";
        return false;
    }
    if (fortran_order) {
        *error = "WEIGHTS_FILE must be C-contiguous, not Fortran ordered";
        return false;
    }
    if (result->shape.size() != 5 || result->shape[4] != 2) {
        *error = "WEIGHTS_FILE shape must be exactly [F,P,A,B,2]";
        return false;
    }

    std::uint64_t component_count = 0;
    if (!Product(result->shape, &component_count) ||
        component_count / 2 >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
        *error = "WEIGHTS_FILE shape is too large";
        return false;
    }

    std::size_t component_bytes = 0;
    if (result->input_dtype == "|i1") {
        component_bytes = 1;
    } else if (result->input_dtype == "<i2") {
        component_bytes = 2;
    } else {
        *error = "unsupported weight dtype; expected |i1 or <i2";
        return false;
    }

    std::uint64_t payload_bytes = 0;
    if (!CheckedMultiply(component_count, component_bytes, &payload_bytes) ||
        payload_bytes > static_cast<std::uint64_t>(
                            std::numeric_limits<std::size_t>::max()) ||
        payload_bytes > static_cast<std::uint64_t>(
                            std::numeric_limits<std::streamsize>::max())) {
        *error = "WEIGHTS_FILE payload is too large";
        return false;
    }
    std::vector<unsigned char> payload(static_cast<std::size_t>(payload_bytes));
    input.read(reinterpret_cast<char*>(&payload[0]),
               static_cast<std::streamsize>(payload.size()));
    if (!input) {
        *error = "truncated NPY weight payload";
        return false;
    }
    if (input.peek() != std::char_traits<char>::eof()) {
        *error = "NPY weight payload has trailing bytes";
        return false;
    }

    result->values.resize(static_cast<std::size_t>(component_count / 2));
    for (std::uint64_t i = 0; i < component_count / 2; ++i) {
        std::int32_t real = 0;
        std::int32_t imag = 0;
        if (component_bytes == 1) {
            const unsigned char real_bits =
                payload[static_cast<std::size_t>(2 * i)];
            const unsigned char imag_bits =
                payload[static_cast<std::size_t>(2 * i + 1)];
            real = real_bits >= 0x80 ?
                static_cast<std::int32_t>(real_bits) - 0x100 : real_bits;
            imag = imag_bits >= 0x80 ?
                static_cast<std::int32_t>(imag_bits) - 0x100 : imag_bits;
        } else {
            const std::size_t offset = static_cast<std::size_t>(4 * i);
            const std::uint16_t real_bits =
                static_cast<std::uint16_t>(payload[offset]) |
                static_cast<std::uint16_t>(payload[offset + 1]) << 8;
            const std::uint16_t imag_bits =
                static_cast<std::uint16_t>(payload[offset + 2]) |
                static_cast<std::uint16_t>(payload[offset + 3]) << 8;
            real = real_bits >= 0x8000 ?
                static_cast<std::int32_t>(real_bits) - 0x10000 : real_bits;
            imag = imag_bits >= 0x8000 ?
                static_cast<std::int32_t>(imag_bits) - 0x10000 : imag_bits;
        }
        const double scaled_real = static_cast<double>(real) * scale;
        const double scaled_imag = static_cast<double>(imag) * scale;
        if (!std::isfinite(scaled_real) || !std::isfinite(scaled_imag) ||
            std::fabs(scaled_real) > std::numeric_limits<float>::max() ||
            std::fabs(scaled_imag) > std::numeric_limits<float>::max()) {
            *error = "scaled weight cannot be represented as CF32";
            return false;
        }
        result->values[static_cast<std::size_t>(i)].real =
            static_cast<float>(scaled_real);
        result->values[static_cast<std::size_t>(i)].imag =
            static_cast<float>(scaled_imag);
    }
    return true;
}

pipeline::StageStatus MissingOrInvalid(const std::string& name) {
    return pipeline::StageStatus::Error("missing or invalid " + name);
}

bool IsHostMemory(pipeline::MemoryLocation location) {
    return location == pipeline::MemoryLocation::kHost ||
           location == pipeline::MemoryLocation::kPinnedHost;
}

}  // namespace

class BeamformModule::Impl {
public:
    Impl()
        : configured(false), execution_backend(pipeline::ExecutionBackend::kHost),
          cuda_device(-1), compute_mode(BeamformComputeMode::kFp32),
          nchan(0), npol(0), nant(0), nbeam(0),
          input_frame_bytes(0), output_frame_bytes(0) {}

    bool configured;
    pipeline::ExecutionBackend execution_backend;
    int cuda_device;
    BeamformComputeMode compute_mode;
    std::uint64_t nchan;
    std::uint64_t npol;
    std::uint64_t nant;
    std::uint64_t nbeam;
    std::uint64_t input_frame_bytes;
    std::uint64_t output_frame_bytes;
    std::vector<Complex32> weights;
    std::unique_ptr<CudaBeamformExecutor> cuda_executor;
};

BeamformModule::BeamformModule() : impl_(new Impl) {}

BeamformModule::~BeamformModule() {}

const char* BeamformModule::Name() const { return "beamform"; }

pipeline::StageStatus BeamformModule::ConfigureHeader(
    const pipeline::Metadata& input_header,
    const pipeline::StageParameters& parameters,
    pipeline::Metadata* output_header) {
    impl_->configured = false;
    impl_->weights.clear();
    if (impl_->cuda_executor) {
        const pipeline::StageStatus finish_status =
            impl_->cuda_executor->Finish();
        impl_->cuda_executor.reset();
        if (!finish_status.ok()) return finish_status;
    }
    if (!output_header) {
        return pipeline::StageStatus::Error("null output header");
    }

    std::string order;
    std::string sample_format;
    if (!input_header.GetString("ORDER", &order) || order != "TFPA") {
        return pipeline::StageStatus::Error("beamform input ORDER must be TFPA");
    }
    if (!input_header.GetString("SAMPLE_FORMAT", &sample_format) ||
        sample_format != "CF32") {
        return pipeline::StageStatus::Error(
            "beamform input SAMPLE_FORMAT must be CF32");
    }
    if (!input_header.GetUint64("NCHAN", &impl_->nchan) ||
        impl_->nchan == 0) {
        return MissingOrInvalid("NCHAN");
    }
    if (!input_header.GetUint64("NPOL", &impl_->npol) || impl_->npol == 0) {
        return MissingOrInvalid("NPOL");
    }
    if (!input_header.GetUint64("NANT", &impl_->nant) || impl_->nant == 0) {
        return MissingOrInvalid("NANT");
    }

    std::string weights_file;
    std::string weights_order;
    std::string weights_id;
    std::string compute_mode;
    std::string execution_backend;
    double weights_scale = 0.0;
    if (!parameters.GetString("WEIGHTS_FILE", &weights_file) ||
        weights_file.empty()) {
        return MissingOrInvalid("WEIGHTS_FILE");
    }
    if (!parameters.GetString("WEIGHTS_ORDER", &weights_order) ||
        weights_order != "FPAB2") {
        return pipeline::StageStatus::Error("WEIGHTS_ORDER must be FPAB2");
    }
    if (!parameters.GetString("WEIGHTS_ID", &weights_id) ||
        weights_id.empty()) {
        return MissingOrInvalid("WEIGHTS_ID");
    }
    if (!parameters.GetDouble("WEIGHTS_SCALE", &weights_scale) ||
        weights_scale <= 0.0 || !std::isfinite(weights_scale)) {
        return MissingOrInvalid("WEIGHTS_SCALE");
    }
    if (!parameters.GetUint64("NBEAM", &impl_->nbeam) || impl_->nbeam == 0) {
        return MissingOrInvalid("NBEAM");
    }
    if (!parameters.GetString("COMPUTE_MODE", &compute_mode)) {
        return MissingOrInvalid("COMPUTE_MODE");
    }
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
    if (compute_mode != "FP32" && compute_mode != "TF32") {
        return pipeline::StageStatus::Error(
            "COMPUTE_MODE must be FP32 or TF32");
    }
    if (impl_->execution_backend == pipeline::ExecutionBackend::kHost &&
        compute_mode != "FP32") {
        return pipeline::StageStatus::Error(
            "host reference backend supports only COMPUTE_MODE=FP32; "
            "TF32 requires the CUDA backend");
    }
    impl_->compute_mode = compute_mode == "TF32" ?
        BeamformComputeMode::kTf32 : BeamformComputeMode::kFp32;

    LoadedWeights loaded;
    std::string load_error;
    if (!LoadNpyWeights(weights_file, weights_scale, &loaded, &load_error)) {
        return pipeline::StageStatus::Error(load_error);
    }
    if (loaded.shape[0] != impl_->nchan ||
        loaded.shape[1] != impl_->npol ||
        loaded.shape[2] != impl_->nant ||
        loaded.shape[3] != impl_->nbeam) {
        std::ostringstream message;
        message << "WEIGHTS_FILE shape [" << loaded.shape[0] << ','
                << loaded.shape[1] << ',' << loaded.shape[2] << ','
                << loaded.shape[3] << ",2] does not match [NCHAN,NPOL,NANT,"
                << "NBEAM,2]";
        return pipeline::StageStatus::Error(message.str());
    }

    std::uint64_t input_elements = 0;
    std::uint64_t output_elements = 0;
    if (!CheckedMultiply(impl_->nchan, impl_->npol, &input_elements) ||
        !CheckedMultiply(input_elements, impl_->nant, &input_elements) ||
        !CheckedMultiply(input_elements, kComplex32Bytes,
                         &impl_->input_frame_bytes) ||
        !CheckedMultiply(impl_->nchan, impl_->npol, &output_elements) ||
        !CheckedMultiply(output_elements, impl_->nbeam, &output_elements) ||
        !CheckedMultiply(output_elements, kComplex32Bytes,
                         &impl_->output_frame_bytes)) {
        return pipeline::StageStatus::Error("beamform frame geometry overflows");
    }

    if (impl_->execution_backend == pipeline::ExecutionBackend::kCuda) {
#if defined(RDMA_DADA_HAVE_CUDA)
        const BeamformGeometry geometry = {
            impl_->nchan, impl_->npol, impl_->nant, impl_->nbeam
        };
        impl_->cuda_executor = CreateCudaBeamformExecutor();
        const pipeline::StageStatus cuda_status =
            impl_->cuda_executor->Configure(
                geometry, loaded.values, impl_->cuda_device,
                impl_->compute_mode);
        if (!cuda_status.ok()) {
            impl_->cuda_executor.reset();
            return cuda_status;
        }
#else
        return pipeline::StageStatus::Error(
            "EXECUTION_BACKEND=CUDA requires a USE_CUDA=ON build");
#endif
    }

    impl_->weights.swap(loaded.values);
    *output_header = input_header;
    output_header->SetString("DATA_STAGE", "BEAMFORMED");
    output_header->SetString("ORDER", "TFPB");
    output_header->SetString("SAMPLE_FORMAT", "CF32");
    output_header->SetString("WEIGHT_ORDER", "FPAB");
    output_header->SetString("WEIGHTS_INPUT_ORDER", "FPAB2");
    output_header->SetString("WEIGHTS_INPUT_DTYPE", loaded.input_dtype);
    output_header->SetString("WEIGHTS_ID", weights_id);
    output_header->SetDouble("WEIGHTS_SCALE", weights_scale);
    output_header->SetString("COMPUTE_MODE", compute_mode);
    output_header->SetString("EXECUTION_BACKEND", execution_backend);
    if (impl_->execution_backend == pipeline::ExecutionBackend::kCuda) {
        output_header->SetUint64(
            "CUDA_DEVICE", static_cast<std::uint64_t>(impl_->cuda_device));
    }
    output_header->SetUint64("NBEAM", impl_->nbeam);
    output_header->SetUint64("COMPONENT_NBIT", 32);
    output_header->SetUint64("SAMPLE_NBIT", 64);
    output_header->SetUint64("RECORD_BYTES", impl_->output_frame_bytes);
    output_header->SetUint64("RESOLUTION", impl_->output_frame_bytes);
    impl_->configured = true;
    return pipeline::StageStatus::Ok();
}

pipeline::StageStatus BeamformModule::ProcessBlock(
    const pipeline::InputBlock& input, pipeline::OutputBlock* output,
    const pipeline::BlockExecutionContext& context) {
    if (!impl_->configured) {
        return pipeline::StageStatus::Error("beamform module is not configured");
    }
    if (!output) return pipeline::StageStatus::Error("null output block");
    if (context.backend != impl_->execution_backend) {
        return pipeline::StageStatus::Error(
            "block execution context does not match configured backend");
    }
    if (input.size == 0 || input.size % impl_->input_frame_bytes != 0) {
        return pipeline::StageStatus::Error(
            "input block does not contain complete TFPA time frames");
    }
    if (!input.data) return pipeline::StageStatus::Error("null input data");

    const std::uint64_t ntime = input.size / impl_->input_frame_bytes;
    std::uint64_t required_output_bytes = 0;
    if (!CheckedMultiply(ntime, impl_->output_frame_bytes,
                         &required_output_bytes)) {
        return pipeline::StageStatus::Error("output block geometry overflows");
    }
    if (output->capacity < required_output_bytes || !output->data) {
        return pipeline::StageStatus::Error("output block is too small");
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
                "CUDA beamform requires CUDA device input and output blocks");
        }
        if (!impl_->cuda_executor) {
            return pipeline::StageStatus::Error(
                "CUDA beamform executor is unavailable");
        }
        const pipeline::StageStatus cuda_status =
            impl_->cuda_executor->Process(
                input, output, ntime, context);
        if (!cuda_status.ok()) return cuda_status;
        output->size = required_output_bytes;
        output->sequence = input.sequence;
        return pipeline::StageStatus::Ok();
    }

    if (context.device_id != -1 || context.native_stream != NULL) {
        return pipeline::StageStatus::Error(
            "host reference backend requires a host execution context");
    }
    if (!IsHostMemory(input.location) || !IsHostMemory(output->location)) {
        return pipeline::StageStatus::Error(
            "host reference backend requires host or pinned-host blocks");
    }

    const Complex32* source = reinterpret_cast<const Complex32*>(input.data);
    Complex32* destination = reinterpret_cast<Complex32*>(output->data);
    for (std::uint64_t t = 0; t < ntime; ++t) {
        for (std::uint64_t f = 0; f < impl_->nchan; ++f) {
            for (std::uint64_t p = 0; p < impl_->npol; ++p) {
                for (std::uint64_t b = 0; b < impl_->nbeam; ++b) {
                    float sum_real = 0.0f;
                    float sum_imag = 0.0f;
                    for (std::uint64_t a = 0; a < impl_->nant; ++a) {
                        const std::uint64_t input_index =
                            (((t * impl_->nchan + f) * impl_->npol + p) *
                             impl_->nant) + a;
                        const std::uint64_t weight_index =
                            (((f * impl_->npol + p) * impl_->nant + a) *
                             impl_->nbeam) + b;
                        const Complex32& x = source[input_index];
                        const Complex32& w = impl_->weights[weight_index];
                        sum_real += x.real * w.real - x.imag * w.imag;
                        sum_imag += x.real * w.imag + x.imag * w.real;
                    }
                    const std::uint64_t output_index =
                        (((t * impl_->nchan + f) * impl_->npol + p) *
                         impl_->nbeam) + b;
                    destination[output_index].real = sum_real;
                    destination[output_index].imag = sum_imag;
                }
            }
        }
    }

    output->size = required_output_bytes;
    output->sequence = input.sequence;
    return pipeline::StageStatus::Ok();
}

pipeline::StageStatus BeamformModule::Finish() {
    pipeline::StageStatus status = pipeline::StageStatus::Ok();
    if (impl_->cuda_executor) {
        status = impl_->cuda_executor->Finish();
        impl_->cuda_executor.reset();
    }
    impl_->configured = false;
    impl_->weights.clear();
    return status;
}

}  // namespace beamform
}  // namespace modules
}  // namespace rdma_dada
