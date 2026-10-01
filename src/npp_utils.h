#ifndef ORANGE_NPP_UTILS_H
#define ORANGE_NPP_UTILS_H

// Per-call NPP stream contexts (the _Ctx API, CUDA >= 10.1; the only NPP API
// left in CUDA 13). Replaces the process-global nppSetStream() current stream:
// that global was shared by every worker thread, nppSetStream() waited for
// outstanding NPP work before switching, and calls that skipped it ran on
// whatever stream another thread had set last or on the legacy default stream.
// A context is plain data (stream + device attributes), so each worker passes
// its own and no cross-thread state exists.
#include <cuda_runtime.h>
#include <npp.h>

#include <stdexcept>
#include <string>

// Build a context for `stream` on the current CUDA device (the caller has
// already called cudaSetDevice for its worker, as every NPP user in Orange
// does). Uses cudaDeviceGetAttribute, not cudaGetDeviceProperties, which holds
// the driver lock for milliseconds.
inline NppStreamContext MakeNppStreamContext(cudaStream_t stream)
{
    NppStreamContext ctx{};
    int device = 0;
    auto check = [](cudaError_t status, const char* what) {
        if (status != cudaSuccess) {
            throw std::runtime_error(std::string("NPP stream context: ") + what + ": " +
                                     cudaGetErrorString(status));
        }
    };
    check(cudaGetDevice(&device), "cudaGetDevice");
    ctx.hStream = stream;
    ctx.nCudaDeviceId = device;
    check(cudaDeviceGetAttribute(&ctx.nMultiProcessorCount, cudaDevAttrMultiProcessorCount, device),
          "multiprocessor count");
    check(cudaDeviceGetAttribute(&ctx.nMaxThreadsPerMultiProcessor, cudaDevAttrMaxThreadsPerMultiProcessor, device),
          "max threads per multiprocessor");
    check(cudaDeviceGetAttribute(&ctx.nMaxThreadsPerBlock, cudaDevAttrMaxThreadsPerBlock, device),
          "max threads per block");
    int shared_mem_per_block = 0;
    check(cudaDeviceGetAttribute(&shared_mem_per_block, cudaDevAttrMaxSharedMemoryPerBlock, device),
          "shared memory per block");
    ctx.nSharedMemPerBlock = static_cast<size_t>(shared_mem_per_block);
    check(cudaDeviceGetAttribute(&ctx.nCudaDevAttrComputeCapabilityMajor, cudaDevAttrComputeCapabilityMajor, device),
          "compute capability major");
    check(cudaDeviceGetAttribute(&ctx.nCudaDevAttrComputeCapabilityMinor, cudaDevAttrComputeCapabilityMinor, device),
          "compute capability minor");
    unsigned int flags = 0;
    if (stream != nullptr) {
        check(cudaStreamGetFlags(stream, &flags), "stream flags");
    }
    ctx.nStreamFlags = flags;
    return ctx;
}

// Cached per thread for the last (device, stream) pair, so hot paths pay the
// attribute queries once per worker, not per frame. The cache is thread-local
// data only; nothing global is set.
inline const NppStreamContext& NppStreamContextFor(cudaStream_t stream)
{
    static thread_local NppStreamContext cached{};
    static thread_local cudaStream_t cached_stream = nullptr;
    static thread_local int cached_device = -1;
    static thread_local bool valid = false;
    int device = 0;
    if (cudaGetDevice(&device) != cudaSuccess) {
        throw std::runtime_error("NPP stream context: cudaGetDevice failed");
    }
    if (!valid || cached_stream != stream || cached_device != device) {
        cached = MakeNppStreamContext(stream);
        cached_stream = stream;
        cached_device = device;
        valid = true;
    }
    return cached;
}

#endif
