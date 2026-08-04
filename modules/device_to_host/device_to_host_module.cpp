#include "rdma_dada/modules/device_to_host/device_to_host_module.h"

#include "../transfer/transfer_backend.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace rdma_dada {
namespace modules {
namespace device_to_host {
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

class DeviceToHostModule::Impl {
public:
    Impl()
        : configured(false), cuda_device(-1), resolution(0),
          output_location(pipeline::MemoryLocation::kHost),
          output_memory("HOST") {}

    bool configured;
    int cuda_device;
    std::uint64_t resolution;
    pipeline::MemoryLocation output_location;
    std::string output_memory;
    std::unique_ptr<transfer::CudaTransferExecutor> executor;
};

DeviceToHostModule::DeviceToHostModule() : impl_(new Impl) {}

DeviceToHostModule::~DeviceToHostModule() {}

const char* DeviceToHostModule::Name() const { return "device_to_host"; }

pipeline::StageStatus DeviceToHostModule::ConfigureHeader(
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
        input_memory != "CUDA_DEVICE") {
        return pipeline::StageStatus::Error(
            "D2H input MEMORY must be CUDA_DEVICE");
    }
    if (!input_header.GetUint64("RESOLUTION", &impl_->resolution) ||
        impl_->resolution == 0) {
        return MissingOrInvalid("RESOLUTION");
    }

    std::string execution_backend;
    if (!parameters.GetString("EXECUTION_BACKEND", &execution_backend) ||
        execution_backend != "CUDA") {
        return pipeline::StageStatus::Error(
            "D2H EXECUTION_BACKEND must be CUDA");
    }
    std::uint64_t cuda_device = 0;
    std::uint64_t header_cuda_device = 0;
    if (!parameters.GetUint64("CUDA_DEVICE", &cuda_device) ||
        cuda_device > static_cast<std::uint64_t>(
                          std::numeric_limits<int>::max())) {
        return MissingOrInvalid("CUDA_DEVICE");
    }
    if (!input_header.GetUint64("CUDA_DEVICE", &header_cuda_device) ||
        header_cuda_device != cuda_device) {
        return pipeline::StageStatus::Error(
            "D2H input CUDA_DEVICE must match configured CUDA_DEVICE");
    }
    impl_->cuda_device = static_cast<int>(cuda_device);

    if (!parameters.GetString("OUTPUT_MEMORY", &impl_->output_memory) ||
        !ParseHostMemory(
            impl_->output_memory, &impl_->output_location)) {
        return pipeline::StageStatus::Error(
            "D2H OUTPUT_MEMORY must be HOST or PINNED_HOST");
    }

#if defined(RDMA_DADA_HAVE_CUDA)
    impl_->executor = transfer::CreateCudaTransferExecutor();
    status = impl_->executor->Configure(impl_->cuda_device);
    if (!status.ok()) {
        impl_->executor.reset();
        return status;
    }
#else
    return pipeline::StageStatus::Error(
        "D2H requires a USE_CUDA=ON build");
#endif

    *output_header = input_header;
    output_header->SetString("MEMORY", impl_->output_memory);
    impl_->configured = true;
    return pipeline::StageStatus::Ok();
}

pipeline::StageStatus DeviceToHostModule::ProcessBlock(
    const pipeline::InputBlock& input, pipeline::OutputBlock* output,
    const pipeline::BlockExecutionContext& context) {
    if (!impl_->configured || !impl_->executor) {
        return pipeline::StageStatus::Error(
            "D2H module is not configured");
    }
    if (!output) return pipeline::StageStatus::Error("null output block");
    if (!input.data) return pipeline::StageStatus::Error("null input data");
    if (input.location != pipeline::MemoryLocation::kCudaDevice) {
        return pipeline::StageStatus::Error(
            "D2H input block must use CUDA device memory");
    }
    if (output->location != impl_->output_location) {
        return pipeline::StageStatus::Error(
            "D2H output block location does not match OUTPUT_MEMORY");
    }
    if (input.size == 0 || input.size % impl_->resolution != 0) {
        return pipeline::StageStatus::Error(
            "D2H input block is not aligned to RESOLUTION");
    }
    if (!output->data || output->capacity < input.size) {
        return pipeline::StageStatus::Error("D2H output block is too small");
    }

    const pipeline::StageStatus status = impl_->executor->Copy(
        input.data, output->data, input.size,
        transfer::TransferDirection::kDeviceToHost, context);
    if (!status.ok()) return status;
    output->size = input.size;
    output->sequence = input.sequence;
    return pipeline::StageStatus::Ok();
}

pipeline::StageStatus DeviceToHostModule::Finish() {
    pipeline::StageStatus status = pipeline::StageStatus::Ok();
    if (impl_->executor) {
        status = impl_->executor->Finish();
        impl_->executor.reset();
    }
    impl_->configured = false;
    impl_->cuda_device = -1;
    impl_->resolution = 0;
    impl_->output_location = pipeline::MemoryLocation::kHost;
    impl_->output_memory = "HOST";
    return status;
}

}  // namespace device_to_host
}  // namespace modules
}  // namespace rdma_dada
