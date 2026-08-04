#include "rdma_dada/modules/host_to_device/host_to_device_module.h"

#include "../transfer/transfer_backend.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace rdma_dada {
namespace modules {
namespace host_to_device {
namespace {

pipeline::StageStatus MissingOrInvalid(const std::string& name) {
    return pipeline::StageStatus::Error("missing or invalid " + name);
}

bool ParseHostMemory(const std::string& value,
                     pipeline::MemoryLocation* location) {
    if (!location) return false;
    if (value == "HOST") {
        *location = pipeline::MemoryLocation::kHost;
        return true;
    }
    if (value == "PINNED_HOST") {
        *location = pipeline::MemoryLocation::kPinnedHost;
        return true;
    }
    return false;
}

}  // namespace

class HostToDeviceModule::Impl {
public:
    Impl()
        : configured(false), cuda_device(-1), resolution(0),
          input_location(pipeline::MemoryLocation::kHost) {}

    bool configured;
    int cuda_device;
    std::uint64_t resolution;
    pipeline::MemoryLocation input_location;
    std::unique_ptr<transfer::CudaTransferExecutor> executor;
};

HostToDeviceModule::HostToDeviceModule() : impl_(new Impl) {}

HostToDeviceModule::~HostToDeviceModule() {}

const char* HostToDeviceModule::Name() const { return "host_to_device"; }

pipeline::StageStatus HostToDeviceModule::ConfigureHeader(
    const pipeline::Metadata& input_header,
    const pipeline::StageParameters& parameters,
    pipeline::Metadata* output_header) {
    pipeline::StageStatus status = Finish();
    if (!status.ok()) return status;
    if (!output_header) {
        return pipeline::StageStatus::Error("null output header");
    }

    std::string input_memory;
    if (!input_header.GetString("MEMORY", &input_memory) ||
        !ParseHostMemory(input_memory, &impl_->input_location)) {
        return pipeline::StageStatus::Error(
            "H2D input MEMORY must be HOST or PINNED_HOST");
    }
    if (!input_header.GetUint64("RESOLUTION", &impl_->resolution) ||
        impl_->resolution == 0) {
        return MissingOrInvalid("RESOLUTION");
    }

    std::string execution_backend;
    if (!parameters.GetString("EXECUTION_BACKEND", &execution_backend) ||
        execution_backend != "CUDA") {
        return pipeline::StageStatus::Error(
            "H2D EXECUTION_BACKEND must be CUDA");
    }
    std::uint64_t cuda_device = 0;
    if (!parameters.GetUint64("CUDA_DEVICE", &cuda_device) ||
        cuda_device > static_cast<std::uint64_t>(
                          std::numeric_limits<int>::max())) {
        return MissingOrInvalid("CUDA_DEVICE");
    }
    impl_->cuda_device = static_cast<int>(cuda_device);

#if defined(RDMA_DADA_HAVE_CUDA)
    impl_->executor = transfer::CreateCudaTransferExecutor();
    status = impl_->executor->Configure(impl_->cuda_device);
    if (!status.ok()) {
        impl_->executor.reset();
        return status;
    }
#else
    return pipeline::StageStatus::Error(
        "H2D requires a USE_CUDA=ON build");
#endif

    *output_header = input_header;
    output_header->SetString("MEMORY", "CUDA_DEVICE");
    output_header->SetUint64(
        "CUDA_DEVICE", static_cast<std::uint64_t>(impl_->cuda_device));
    impl_->configured = true;
    return pipeline::StageStatus::Ok();
}

pipeline::StageStatus HostToDeviceModule::ProcessBlock(
    const pipeline::InputBlock& input, pipeline::OutputBlock* output,
    const pipeline::BlockExecutionContext& context) {
    if (!impl_->configured || !impl_->executor) {
        return pipeline::StageStatus::Error(
            "H2D module is not configured");
    }
    if (!output) return pipeline::StageStatus::Error("null output block");
    if (!input.data) return pipeline::StageStatus::Error("null input data");
    if (input.location != impl_->input_location) {
        return pipeline::StageStatus::Error(
            "H2D input block location does not match input header MEMORY");
    }
    if (output->location != pipeline::MemoryLocation::kCudaDevice) {
        return pipeline::StageStatus::Error(
            "H2D output block must use CUDA device memory");
    }
    if (input.size == 0 || input.size % impl_->resolution != 0) {
        return pipeline::StageStatus::Error(
            "H2D input block is not aligned to RESOLUTION");
    }
    if (!output->data || output->capacity < input.size) {
        return pipeline::StageStatus::Error("H2D output block is too small");
    }

    const pipeline::StageStatus status = impl_->executor->Copy(
        input.data, output->data, input.size,
        transfer::TransferDirection::kHostToDevice, context);
    if (!status.ok()) return status;
    output->size = input.size;
    output->sequence = input.sequence;
    return pipeline::StageStatus::Ok();
}

pipeline::StageStatus HostToDeviceModule::Finish() {
    pipeline::StageStatus status = pipeline::StageStatus::Ok();
    if (impl_->executor) {
        status = impl_->executor->Finish();
        impl_->executor.reset();
    }
    impl_->configured = false;
    impl_->cuda_device = -1;
    impl_->resolution = 0;
    return status;
}

}  // namespace host_to_device
}  // namespace modules
}  // namespace rdma_dada
