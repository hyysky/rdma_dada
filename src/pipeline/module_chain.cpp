#include "rdma_dada/pipeline/module_chain.h"

#include "rdma_dada/modules/beamform/beamform_module.h"
#include "rdma_dada/modules/power/power_module.h"
#include "rdma_dada/modules/stokes/stokes_module.h"
#include "rdma_dada/modules/time_integrate/time_integrate_module.h"
#include "rdma_dada/pipeline/complex32.h"

#include <limits>
#include <memory>
#include <sstream>
#include <string>

namespace rdma_dada {
namespace pipeline {
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

StageStatus MissingOrInvalid(const std::string& name) {
    return StageStatus::Error("missing or invalid " + name);
}

StageStatus RequireString(const Metadata& header, const std::string& name,
                          const std::string& expected) {
    std::string value;
    if (!header.GetString(name, &value)) return MissingOrInvalid(name);
    if (value != expected) {
        return StageStatus::Error(name + " must be " + expected);
    }
    return StageStatus::Ok();
}

StageStatus ScaleByteField(const Metadata& source, const std::string& name,
                           std::uint64_t input_frame_bytes,
                           std::uint64_t output_frame_bytes,
                           bool require_positive, Metadata* destination) {
    if (!source.Has(name)) return StageStatus::Ok();
    std::uint64_t input_value = 0;
    if (!source.GetUint64(name, &input_value) ||
        (require_positive && input_value == 0)) {
        return MissingOrInvalid(name);
    }
    if (input_value % input_frame_bytes != 0) {
        return StageStatus::Error(
            name + " is not aligned to a complete TFPA time frame");
    }
    std::uint64_t output_value = 0;
    if (!CheckedMultiply(input_value / input_frame_bytes,
                         output_frame_bytes, &output_value)) {
        return StageStatus::Error(name + " output scaling overflows");
    }
    destination->SetUint64(name, output_value);
    return StageStatus::Ok();
}

StageStatus ScaleGlobalByteFields(const Metadata& source,
                                  std::uint64_t input_frame_bytes,
                                  std::uint64_t output_frame_bytes,
                                  Metadata* destination) {
    const char* const optional_fields[] = {
        "TRANSFER_SIZE", "FILE_SIZE", "OBS_OFFSET"
    };
    StageStatus status = ScaleByteField(
        source, "BYTES_PER_SECOND", input_frame_bytes, output_frame_bytes,
        true, destination);
    if (!status.ok()) return status;
    for (std::size_t i = 0;
         i < sizeof(optional_fields) / sizeof(optional_fields[0]); ++i) {
        status = ScaleByteField(source, optional_fields[i], input_frame_bytes,
                                output_frame_bytes, false, destination);
        if (!status.ok()) return status;
    }
    return StageStatus::Ok();
}

StageParameters MakeModuleParameters(const WorkerConfig& config) {
    StageParameters parameters;
    parameters.SetString("WEIGHTS_FILE", config.weights_file);
    parameters.SetString("WEIGHTS_ORDER", config.weights_order);
    parameters.SetString("WEIGHTS_ID", config.weights_id);
    parameters.SetDouble("WEIGHTS_SCALE", config.weights_scale);
    parameters.SetUint64("NBEAM", config.nbeam);
    parameters.SetString("COMPUTE_MODE", config.compute_mode);
    parameters.SetString("EXECUTION_BACKEND", config.execution_backend);
    parameters.SetUint64(
        "INTEGRATION_LENGTH", config.integration_length);
    parameters.SetString(
        "INTEGRATION_OPERATION", config.integration_operation);
    if (config.execution_backend == "CUDA") {
        parameters.SetUint64(
            "CUDA_DEVICE", static_cast<std::uint64_t>(config.cuda_device));
    }
    return parameters;
}

}  // namespace

class ModuleChain::Impl {
public:
    Impl() : configured(false), product(WorkerProduct::kBeamformed) {
        plan.input_frame_bytes = 0;
        plan.beamformed_frame_bytes = 0;
        plan.product_frame_bytes = 0;
        plan.output_frame_bytes = 0;
        plan.integration_length = 1;
        plan.integration_enabled = false;
        plan.module_count = 0;
        plan.execution_location = MemoryLocation::kHost;
    }

    bool configured;
    WorkerProduct product;
    ModuleChainPlan plan;
    modules::beamform::BeamformModule beamform;
    std::unique_ptr<AlgorithmModule> post_beamform;
    std::unique_ptr<AlgorithmModule> integration;
};

ModuleChain::ModuleChain() : impl_(new Impl) {}

ModuleChain::~ModuleChain() { Finish(); }

StageStatus ModuleChain::Configure(const Metadata& ring_input_header,
                                   const WorkerConfig& config,
                                   Metadata* ring_output_header) {
    StageStatus status = Finish();
    if (!status.ok()) return status;
    if (!ring_output_header) {
        return StageStatus::Error("null ring output header");
    }

    WorkerBlockGeometry configured_geometry;
    std::string geometry_error;
    if (!ComputeWorkerBlockGeometry(
            config, &configured_geometry, &geometry_error)) {
        return StageStatus::Error(
            "invalid worker block geometry: " + geometry_error);
    }

    status = RequireString(ring_input_header, "DATA_STAGE", "CONVERTED");
    if (!status.ok()) return status;
    status = RequireString(ring_input_header, "ORDER", "TFPA");
    if (!status.ok()) return status;
    status = RequireString(ring_input_header, "SAMPLE_FORMAT", "CF32");
    if (!status.ok()) return status;

    std::string input_memory;
    if (!ring_input_header.GetString("MEMORY", &input_memory) ||
        (input_memory != "HOST" && input_memory != "PINNED_HOST")) {
        return StageStatus::Error(
            "pipeline_worker input ring requires MEMORY=HOST or PINNED_HOST");
    }

    std::uint64_t nchan = 0;
    std::uint64_t npol = 0;
    std::uint64_t nant = 0;
    std::uint64_t input_frame_bytes = 0;
    if (!ring_input_header.GetUint64("NCHAN", &nchan) || nchan == 0) {
        return MissingOrInvalid("NCHAN");
    }
    if (!ring_input_header.GetUint64("NPOL", &npol) || npol == 0) {
        return MissingOrInvalid("NPOL");
    }
    if (!ring_input_header.GetUint64("NANT", &nant) || nant == 0) {
        return MissingOrInvalid("NANT");
    }
    if (nchan != config.nchan || npol != config.npol || nant != config.nant) {
        std::ostringstream message;
        message << "input header F/P/A geometry [" << nchan << ',' << npol
                << ',' << nant << "] does not match worker config ["
                << config.nchan << ',' << config.npol << ',' << config.nant
                << ']';
        return StageStatus::Error(message.str());
    }
    if (!CheckedMultiply(nchan, npol, &input_frame_bytes) ||
        !CheckedMultiply(input_frame_bytes, nant, &input_frame_bytes) ||
        !CheckedMultiply(input_frame_bytes, sizeof(Complex32),
                         &input_frame_bytes)) {
        return StageStatus::Error("input TFPA frame geometry overflows");
    }
    if (input_frame_bytes != configured_geometry.input_frame_bytes) {
        return StageStatus::Error(
            "configured input frame bytes do not match TFPA header geometry");
    }
    std::uint64_t resolution = 0;
    if (!ring_input_header.GetUint64("RESOLUTION", &resolution) ||
        resolution != input_frame_bytes) {
        std::ostringstream message;
        message << "RESOLUTION must equal one TFPA CF32 frame ("
                << input_frame_bytes << " bytes)";
        return StageStatus::Error(message.str());
    }
    std::uint64_t input_byte_rate = 0;
    if (!ring_input_header.GetUint64("BYTES_PER_SECOND", &input_byte_rate) ||
        input_byte_rate == 0) {
        return MissingOrInvalid("BYTES_PER_SECOND");
    }
    if (input_byte_rate % input_frame_bytes != 0) {
        return StageStatus::Error(
            "BYTES_PER_SECOND is not aligned to a complete TFPA frame");
    }

    if (config.execution_backend != "CPU_REFERENCE" &&
        config.execution_backend != "CUDA") {
        return StageStatus::Error(
            "worker EXECUTION_BACKEND must be CPU_REFERENCE or CUDA");
    }
    if (config.execution_backend == "CUDA" && config.cuda_device < 0) {
        return StageStatus::Error("CUDA_DEVICE must be non-negative");
    }

    Metadata module_input_header = ring_input_header;
    if (config.execution_backend == "CUDA") {
        module_input_header.SetString("MEMORY", "CUDA_DEVICE");
        module_input_header.SetUint64(
            "CUDA_DEVICE", static_cast<std::uint64_t>(config.cuda_device));
        impl_->plan.execution_location = MemoryLocation::kCudaDevice;
    } else {
        impl_->plan.execution_location = MemoryLocation::kHost;
        module_input_header.SetString("MEMORY", "HOST");
        module_input_header.Erase("CUDA_DEVICE");
    }

    const StageParameters parameters = MakeModuleParameters(config);
    Metadata beamformed_header;
    status = impl_->beamform.ConfigureHeader(
        module_input_header, parameters, &beamformed_header);
    if (!status.ok()) return status;

    std::uint64_t beamformed_frame_bytes = 0;
    if (!beamformed_header.GetUint64(
            "RESOLUTION", &beamformed_frame_bytes) ||
        beamformed_frame_bytes == 0) {
        impl_->beamform.Finish();
        return MissingOrInvalid("beamform output RESOLUTION");
    }
    if (beamformed_frame_bytes !=
        configured_geometry.beamformed_frame_bytes) {
        impl_->beamform.Finish();
        return StageStatus::Error(
            "beamform output frame does not match configured block geometry");
    }
    status = ScaleGlobalByteFields(
        ring_input_header, input_frame_bytes, beamformed_frame_bytes,
        &beamformed_header);
    if (!status.ok()) {
        impl_->beamform.Finish();
        return status;
    }

    Metadata product_header = beamformed_header;
    impl_->product = config.product;
    switch (config.product) {
        case WorkerProduct::kBeamformed:
            break;
        case WorkerProduct::kPower:
            impl_->post_beamform.reset(new modules::power::PowerModule);
            break;
        case WorkerProduct::kStokes:
            impl_->post_beamform.reset(new modules::stokes::StokesModule);
            break;
        default:
            impl_->beamform.Finish();
            return StageStatus::Error("unsupported worker output product");
    }
    if (impl_->post_beamform) {
        status = impl_->post_beamform->ConfigureHeader(
            beamformed_header, parameters, &product_header);
        if (!status.ok()) {
            impl_->post_beamform->Finish();
            impl_->post_beamform.reset();
            impl_->beamform.Finish();
            return status;
        }
    }

    std::uint64_t product_frame_bytes = 0;
    if (!product_header.GetUint64("RESOLUTION", &product_frame_bytes) ||
        product_frame_bytes == 0) {
        Finish();
        return MissingOrInvalid("module chain output RESOLUTION");
    }
    if (product_frame_bytes != configured_geometry.output_frame_bytes) {
        Finish();
        return StageStatus::Error(
            "module output frame does not match configured block geometry");
    }
    status = ScaleGlobalByteFields(
        ring_input_header, input_frame_bytes, product_frame_bytes,
        &product_header);
    if (!status.ok()) {
        Finish();
        return status;
    }

    Metadata module_output_header = product_header;
    if (config.integration_enabled) {
        impl_->integration.reset(
            new modules::time_integrate::TimeIntegrateModule);
        status = impl_->integration->ConfigureHeader(
            product_header, parameters, &module_output_header);
        if (!status.ok()) {
            Finish();
            return status;
        }
    }

    std::uint64_t output_frame_bytes = 0;
    if (!module_output_header.GetUint64(
            "RESOLUTION", &output_frame_bytes) ||
        output_frame_bytes != product_frame_bytes) {
        Finish();
        return StageStatus::Error(
            "integration must preserve the product frame resolution");
    }

    Metadata published_header = module_output_header;
    published_header.SetString("MEMORY", "HOST");
    std::string pipeline_modules =
        config.product == WorkerProduct::kBeamformed ?
            "beamform" :
            (config.product == WorkerProduct::kPower ?
                 "beamform,power" : "beamform,stokes");
    if (config.integration_enabled) {
        pipeline_modules += ",time_integrate";
    }
    published_header.SetString("PIPELINE_MODULES", pipeline_modules);
    published_header.SetUint64(
        "INPUT_BLOCK_NTIME", configured_geometry.ntime);
    published_header.SetUint64(
        "BLOCK_NTIME", configured_geometry.output_ntime);
    published_header.SetUint64(
        "UDP_PAYLOAD_BYTES", config.udp_payload_bytes);
    published_header.SetUint64("UDP_NSAMP", config.samples_per_udp);
    published_header.SetUint64(
        "UDP_PACKETS_PER_ANTENNA_PER_BLOCK",
        config.udp_packets_per_antenna_per_block);
    published_header.SetUint64(
        "UDP_ANTENNA_GROUP_BYTES",
        configured_geometry.udp_antenna_group_bytes);
    published_header.SetUint64(
        "UDP_GROUP_MULTIPLE", configured_geometry.udp_group_multiple);
    published_header.SetUint64(
        "INPUT_BLOCK_BYTES", configured_geometry.input_block_bytes);
    published_header.SetUint64(
        "OUTPUT_BLOCK_BYTES", configured_geometry.output_block_bytes);

    impl_->plan.input_header = module_input_header;
    impl_->plan.output_header = published_header;
    impl_->plan.input_frame_bytes = input_frame_bytes;
    impl_->plan.beamformed_frame_bytes = beamformed_frame_bytes;
    impl_->plan.product_frame_bytes = product_frame_bytes;
    impl_->plan.output_frame_bytes = output_frame_bytes;
    impl_->plan.integration_length = config.integration_length;
    impl_->plan.integration_enabled = config.integration_enabled;
    impl_->plan.module_count = 1U + (impl_->post_beamform ? 1U : 0U) +
                               (impl_->integration ? 1U : 0U);
    impl_->configured = true;
    *ring_output_header = published_header;
    return StageStatus::Ok();
}

StageStatus ModuleChain::PlanBlock(std::uint64_t input_bytes,
                                   std::uint64_t* scratch_bytes,
                                   std::uint64_t* output_bytes) const {
    if (!impl_->configured) {
        return StageStatus::Error("module chain is not configured");
    }
    if (!scratch_bytes || !output_bytes) {
        return StageStatus::Error("null block plan output");
    }
    if (input_bytes == 0 ||
        input_bytes % impl_->plan.input_frame_bytes != 0) {
        return StageStatus::Error(
            "input block does not contain complete TFPA time frames");
    }
    const std::uint64_t ntime = input_bytes / impl_->plan.input_frame_bytes;
    if (impl_->plan.integration_enabled &&
        ntime % impl_->plan.integration_length != 0) {
        return StageStatus::Error(
            "input T must be divisible by integration length");
    }
    std::uint64_t beamformed_bytes = 0;
    std::uint64_t product_bytes = 0;
    if (!CheckedMultiply(ntime, impl_->plan.beamformed_frame_bytes,
                         &beamformed_bytes) ||
        !CheckedMultiply(ntime, impl_->plan.product_frame_bytes,
                         &product_bytes)) {
        return StageStatus::Error("planned block geometry overflows");
    }
    if (!impl_->post_beamform) {
        *scratch_bytes = 0;
    } else if (!impl_->plan.integration_enabled) {
        *scratch_bytes = beamformed_bytes;
    } else {
        if (product_bytes > std::numeric_limits<std::uint64_t>::max() -
                                beamformed_bytes) {
            return StageStatus::Error("planned scratch geometry overflows");
        }
        *scratch_bytes = beamformed_bytes + product_bytes;
    }
    const std::uint64_t output_ntime = impl_->plan.integration_enabled ?
        ntime / impl_->plan.integration_length : ntime;
    if (!CheckedMultiply(output_ntime, impl_->plan.output_frame_bytes,
                         output_bytes)) {
        return StageStatus::Error("planned output geometry overflows");
    }
    return StageStatus::Ok();
}

StageStatus ModuleChain::ProcessBlock(
    const InputBlock& input, OutputBlock* output, std::uint8_t* scratch,
    std::uint64_t scratch_capacity, const BlockExecutionContext& context) {
    if (!impl_->configured) {
        return StageStatus::Error("module chain is not configured");
    }
    if (!output) return StageStatus::Error("null module chain output block");
    if (input.location != impl_->plan.execution_location ||
        output->location != impl_->plan.execution_location) {
        return StageStatus::Error(
            "module chain block memory does not match configured backend");
    }

    std::uint64_t scratch_bytes = 0;
    std::uint64_t output_bytes = 0;
    StageStatus status = PlanBlock(
        input.size, &scratch_bytes, &output_bytes);
    if (!status.ok()) return status;
    if (output->capacity < output_bytes || !output->data) {
        return StageStatus::Error("module chain output block is too small");
    }

    if (!impl_->post_beamform) {
        return impl_->beamform.ProcessBlock(input, output, context);
    }
    if (!scratch || scratch_capacity < scratch_bytes) {
        return StageStatus::Error(
            "module chain scratch block is too small");
    }
    const std::uint64_t ntime =
        input.size / impl_->plan.input_frame_bytes;
    std::uint64_t beamformed_bytes = 0;
    std::uint64_t product_bytes = 0;
    if (!CheckedMultiply(ntime, impl_->plan.beamformed_frame_bytes,
                         &beamformed_bytes) ||
        !CheckedMultiply(ntime, impl_->plan.product_frame_bytes,
                         &product_bytes)) {
        return StageStatus::Error("module chain scratch geometry overflows");
    }
    OutputBlock beamformed = {
        scratch, scratch_capacity, 0, input.sequence,
        impl_->plan.execution_location
    };
    status = impl_->beamform.ProcessBlock(input, &beamformed, context);
    if (!status.ok()) return status;
    const InputBlock post_input = {
        beamformed.data, beamformed.size, beamformed.sequence,
        beamformed.location
    };
    if (!impl_->integration) {
        return impl_->post_beamform->ProcessBlock(
            post_input, output, context);
    }
    std::uint8_t* product_data =
        scratch + static_cast<std::size_t>(beamformed_bytes);
    OutputBlock product_output = {
        product_data, product_bytes, 0, input.sequence,
        impl_->plan.execution_location
    };
    status = impl_->post_beamform->ProcessBlock(
        post_input, &product_output, context);
    if (!status.ok()) return status;
    const InputBlock integration_input = {
        product_output.data, product_output.size, product_output.sequence,
        product_output.location
    };
    return impl_->integration->ProcessBlock(
        integration_input, output, context);
}

StageStatus ModuleChain::Finish() {
    StageStatus integration_status = StageStatus::Ok();
    if (impl_->integration) {
        integration_status = impl_->integration->Finish();
        impl_->integration.reset();
    }
    StageStatus post_status = StageStatus::Ok();
    if (impl_->post_beamform) {
        post_status = impl_->post_beamform->Finish();
        impl_->post_beamform.reset();
    }
    const StageStatus beam_status = impl_->beamform.Finish();
    impl_->configured = false;
    impl_->plan.input_frame_bytes = 0;
    impl_->plan.beamformed_frame_bytes = 0;
    impl_->plan.product_frame_bytes = 0;
    impl_->plan.output_frame_bytes = 0;
    impl_->plan.integration_length = 1;
    impl_->plan.integration_enabled = false;
    impl_->plan.module_count = 0;
    impl_->plan.execution_location = MemoryLocation::kHost;
    impl_->plan.input_header = Metadata();
    impl_->plan.output_header = Metadata();
    if (!integration_status.ok()) return integration_status;
    if (!post_status.ok()) return post_status;
    return beam_status;
}

const ModuleChainPlan& ModuleChain::plan() const { return impl_->plan; }

}  // namespace pipeline
}  // namespace rdma_dada
