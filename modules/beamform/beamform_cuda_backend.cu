#include "beamform_backend.h"

#include <cublas_v2.h>
#include <cuComplex.h>
#include <cuda_runtime_api.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace rdma_dada {
namespace modules {
namespace beamform {
namespace {

static_assert(sizeof(Complex32) == sizeof(cuComplex),
              "Complex32 and cuComplex must have the same size");
static_assert(offsetof(Complex32, real) == offsetof(cuComplex, x),
              "Complex32 real component must match cuComplex");
static_assert(offsetof(Complex32, imag) == offsetof(cuComplex, y),
              "Complex32 imaginary component must match cuComplex");

pipeline::StageStatus CudaError(const char* operation, cudaError_t error) {
    std::ostringstream message;
    message << operation << " failed: " << cudaGetErrorString(error);
    return pipeline::StageStatus::Error(message.str());
}

const char* CublasStatusName(cublasStatus_t status) {
    switch (status) {
        case CUBLAS_STATUS_SUCCESS: return "success";
        case CUBLAS_STATUS_NOT_INITIALIZED: return "not initialized";
        case CUBLAS_STATUS_ALLOC_FAILED: return "allocation failed";
        case CUBLAS_STATUS_INVALID_VALUE: return "invalid value";
        case CUBLAS_STATUS_ARCH_MISMATCH: return "architecture mismatch";
        case CUBLAS_STATUS_MAPPING_ERROR: return "mapping error";
        case CUBLAS_STATUS_EXECUTION_FAILED: return "execution failed";
        case CUBLAS_STATUS_INTERNAL_ERROR: return "internal error";
        case CUBLAS_STATUS_NOT_SUPPORTED: return "not supported";
        default: return "unknown cuBLAS error";
    }
}

pipeline::StageStatus CublasError(const char* operation,
                                  cublasStatus_t status) {
    std::ostringstream message;
    message << operation << " failed: " << CublasStatusName(status)
            << " (" << static_cast<int>(status) << ')';
    return pipeline::StageStatus::Error(message.str());
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t* result) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    *result = left * right;
    return true;
}

bool FitsInt(std::uint64_t value) {
    return value <= static_cast<std::uint64_t>(INT_MAX);
}

bool FitsStride(std::uint64_t value) {
    return value <= static_cast<std::uint64_t>(LLONG_MAX);
}

class CublasBeamformExecutor : public CudaBeamformExecutor {
public:
    CublasBeamformExecutor()
        : configured_(false), cuda_device_(-1), device_weights_(NULL),
          compute_mode_(BeamformComputeMode::kFp32) {
        geometry_.nchan = 0;
        geometry_.npol = 0;
        geometry_.nant = 0;
        geometry_.nbeam = 0;
    }

    ~CublasBeamformExecutor() { Finish(); }

    pipeline::StageStatus Configure(
        const BeamformGeometry& geometry,
        const std::vector<Complex32>& weights,
        int cuda_device,
        BeamformComputeMode compute_mode) {
        pipeline::StageStatus finish_status = Finish();
        if (!finish_status.ok()) return finish_status;

        if (cuda_device < 0) {
            return pipeline::StageStatus::Error("CUDA_DEVICE must be non-negative");
        }
        int device_count = 0;
        cudaError_t cuda_status = cudaGetDeviceCount(&device_count);
        if (cuda_status != cudaSuccess) {
            return CudaError("cudaGetDeviceCount", cuda_status);
        }
        if (cuda_device >= device_count) {
            return pipeline::StageStatus::Error(
                "CUDA_DEVICE is not available on this server");
        }
        cuda_status = cudaSetDevice(cuda_device);
        if (cuda_status != cudaSuccess) {
            return CudaError("cudaSetDevice", cuda_status);
        }

        cudaDeviceProp properties;
        cuda_status = cudaGetDeviceProperties(&properties, cuda_device);
        if (cuda_status != cudaSuccess) {
            return CudaError("cudaGetDeviceProperties", cuda_status);
        }
        if (compute_mode == BeamformComputeMode::kTf32 &&
            properties.major < 8) {
            return pipeline::StageStatus::Error(
                "COMPUTE_MODE=TF32 requires compute capability 8.0 or newer");
        }

        std::uint64_t batch_count = 0;
        std::uint64_t weight_count = 0;
        std::uint64_t input_leading_dimension = 0;
        std::uint64_t output_leading_dimension = 0;
        if (!CheckedMultiply(geometry.nchan, geometry.npol, &batch_count) ||
            !CheckedMultiply(batch_count, geometry.nant,
                             &input_leading_dimension) ||
            !CheckedMultiply(batch_count, geometry.nbeam,
                             &output_leading_dimension) ||
            !CheckedMultiply(input_leading_dimension, geometry.nbeam,
                             &weight_count)) {
            return pipeline::StageStatus::Error(
                "CUDA beamform geometry overflows");
        }
        if (weight_count != weights.size()) {
            return pipeline::StageStatus::Error(
                "converted weight count does not match beamform geometry");
        }
        if (!FitsInt(geometry.nant) || !FitsInt(geometry.nbeam) ||
            !FitsInt(batch_count) || !FitsInt(input_leading_dimension) ||
            !FitsInt(output_leading_dimension)) {
            return pipeline::StageStatus::Error(
                "CUDA beamform dimensions exceed the cuBLAS int interface");
        }

        const std::size_t weight_bytes = weights.size() * sizeof(Complex32);
        cuda_status = cudaMalloc(
            reinterpret_cast<void**>(&device_weights_), weight_bytes);
        if (cuda_status != cudaSuccess) {
            return CudaError("cudaMalloc(weights)", cuda_status);
        }
        cuda_status = cudaMemcpy(device_weights_, &weights[0], weight_bytes,
                                 cudaMemcpyHostToDevice);
        if (cuda_status != cudaSuccess) {
            cudaFree(device_weights_);
            device_weights_ = NULL;
            return CudaError("cudaMemcpy(weights)", cuda_status);
        }

        geometry_ = geometry;
        cuda_device_ = cuda_device;
        compute_mode_ = compute_mode;
        configured_ = true;
        return pipeline::StageStatus::Ok();
    }

    pipeline::StageStatus Process(
        const pipeline::InputBlock& input,
        pipeline::OutputBlock* output,
        std::uint64_t ntime,
        const pipeline::BlockExecutionContext& context) {
        if (!configured_) {
            return pipeline::StageStatus::Error(
                "CUDA beamform executor is not configured");
        }
        if (!context.native_stream) {
            return pipeline::StageStatus::Error(
                "CUDA execution requires a worker-owned non-default stream");
        }
        if (context.device_id != cuda_device_) {
            return pipeline::StageStatus::Error(
                "CUDA context device does not match configured CUDA_DEVICE");
        }
        if (!FitsInt(ntime)) {
            return pipeline::StageStatus::Error(
                "block time dimension exceeds the cuBLAS int interface");
        }

        cudaError_t cuda_status = cudaSetDevice(cuda_device_);
        if (cuda_status != cudaSuccess) {
            return CudaError("cudaSetDevice", cuda_status);
        }
        cudaStream_t stream =
            reinterpret_cast<cudaStream_t>(context.native_stream);
        cublasHandle_t handle = NULL;
        std::map<cudaStream_t, cublasHandle_t>::iterator handle_it =
            handles_.find(stream);
        if (handle_it == handles_.end()) {
            cublasStatus_t cublas_status = cublasCreate(&handle);
            if (cublas_status != CUBLAS_STATUS_SUCCESS) {
                return CublasError("cublasCreate", cublas_status);
            }
            cublas_status = cublasSetStream(handle, stream);
            if (cublas_status != CUBLAS_STATUS_SUCCESS) {
                cublasDestroy(handle);
                return CublasError("cublasSetStream", cublas_status);
            }
            cublas_status =
                cublasSetPointerMode(handle, CUBLAS_POINTER_MODE_HOST);
            if (cublas_status != CUBLAS_STATUS_SUCCESS) {
                cublasDestroy(handle);
                return CublasError("cublasSetPointerMode", cublas_status);
            }
            handles_[stream] = handle;
        } else {
            handle = handle_it->second;
        }

        std::uint64_t batch_count = geometry_.nchan * geometry_.npol;
        std::uint64_t input_ld = batch_count * geometry_.nant;
        std::uint64_t output_ld = batch_count * geometry_.nbeam;
        std::uint64_t weight_stride = geometry_.nant * geometry_.nbeam;
        if (!FitsStride(weight_stride) || !FitsStride(geometry_.nant) ||
            !FitsStride(geometry_.nbeam)) {
            return pipeline::StageStatus::Error(
                "CUDA beamform batch stride exceeds the cuBLAS interface");
        }

        const cuComplex alpha = make_cuComplex(1.0f, 0.0f);
        const cuComplex beta = make_cuComplex(0.0f, 0.0f);
        const cublasComputeType_t compute_type =
            compute_mode_ == BeamformComputeMode::kTf32 ?
                CUBLAS_COMPUTE_32F_FAST_TF32 : CUBLAS_COMPUTE_32F;
        const cublasGemmAlgo_t algorithm =
            compute_mode_ == BeamformComputeMode::kTf32 ?
                CUBLAS_GEMM_DEFAULT_TENSOR_OP : CUBLAS_GEMM_DEFAULT;

        // CUBLAS is column-major. FPAB row-major weights are W^T[B,A],
        // TFPA input is X^T[A,T] with a leading dimension spanning all F/P
        // batches, and TFPB output is Y^T[B,T] with the analogous stride.
        const cublasStatus_t cublas_status = cublasGemmStridedBatchedEx(
            handle,
            CUBLAS_OP_N,
            CUBLAS_OP_N,
            static_cast<int>(geometry_.nbeam),
            static_cast<int>(ntime),
            static_cast<int>(geometry_.nant),
            &alpha,
            device_weights_,
            CUDA_C_32F,
            static_cast<int>(geometry_.nbeam),
            static_cast<long long int>(weight_stride),
            input.data,
            CUDA_C_32F,
            static_cast<int>(input_ld),
            static_cast<long long int>(geometry_.nant),
            &beta,
            output->data,
            CUDA_C_32F,
            static_cast<int>(output_ld),
            static_cast<long long int>(geometry_.nbeam),
            static_cast<int>(batch_count),
            compute_type,
            algorithm);
        if (cublas_status != CUBLAS_STATUS_SUCCESS) {
            return CublasError("cublasGemmStridedBatchedEx", cublas_status);
        }
        return pipeline::StageStatus::Ok();
    }

    pipeline::StageStatus Finish() {
        pipeline::StageStatus result = pipeline::StageStatus::Ok();
        if (cuda_device_ >= 0) {
            const cudaError_t set_status = cudaSetDevice(cuda_device_);
            if (set_status != cudaSuccess) {
                result = CudaError("cudaSetDevice", set_status);
            }
        }
        for (std::map<cudaStream_t, cublasHandle_t>::iterator it =
                 handles_.begin();
             it != handles_.end(); ++it) {
            const cudaError_t sync_status = cudaStreamSynchronize(it->first);
            if (result.ok() && sync_status != cudaSuccess) {
                result = CudaError("cudaStreamSynchronize", sync_status);
            }
            const cublasStatus_t destroy_status = cublasDestroy(it->second);
            if (result.ok() && destroy_status != CUBLAS_STATUS_SUCCESS) {
                result = CublasError("cublasDestroy", destroy_status);
            }
        }
        handles_.clear();
        if (device_weights_) {
            const cudaError_t free_status = cudaFree(device_weights_);
            if (result.ok() && free_status != cudaSuccess) {
                result = CudaError("cudaFree(weights)", free_status);
            }
            device_weights_ = NULL;
        }
        configured_ = false;
        cuda_device_ = -1;
        return result;
    }

private:
    bool configured_;
    int cuda_device_;
    BeamformGeometry geometry_;
    cuComplex* device_weights_;
    BeamformComputeMode compute_mode_;
    std::map<cudaStream_t, cublasHandle_t> handles_;
};

}  // namespace

std::unique_ptr<CudaBeamformExecutor> CreateCudaBeamformExecutor() {
    return std::unique_ptr<CudaBeamformExecutor>(new CublasBeamformExecutor);
}

}  // namespace beamform
}  // namespace modules
}  // namespace rdma_dada
