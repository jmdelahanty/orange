#ifndef ORANGE_NPP_UTILS_H
#define ORANGE_NPP_UTILS_H

#include <cuda_runtime.h>
#include <npp.h>

inline void EnsureNppStream(cudaStream_t stream)
{
    static thread_local cudaStream_t last_stream = nullptr;
    if (last_stream != stream) {
        nppSetStream(stream);
        last_stream = stream;
    }
}

#endif
