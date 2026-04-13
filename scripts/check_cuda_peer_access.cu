#include <cuda_runtime.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

const char* bool_text(bool value) {
    return value ? "yes" : "no";
}

void print_cuda_error(const std::string& prefix, cudaError_t err) {
    std::cerr << prefix << ": " << cudaGetErrorString(err) << std::endl;
}

} // namespace

int main() {
    int device_count = 0;
    cudaError_t err = cudaGetDeviceCount(&device_count);
    if (err != cudaSuccess) {
        print_cuda_error("cudaGetDeviceCount failed", err);
        return EXIT_FAILURE;
    }

    std::cout << "CUDA device count: " << device_count << std::endl;
    if (device_count <= 0) {
        return EXIT_SUCCESS;
    }

    for (int device = 0; device < device_count; ++device) {
        cudaDeviceProp prop{};
        err = cudaGetDeviceProperties(&prop, device);
        if (err != cudaSuccess) {
            print_cuda_error("cudaGetDeviceProperties failed", err);
            return EXIT_FAILURE;
        }

        std::cout << "Device " << device
                  << ": " << prop.name
                  << " cc " << prop.major << "." << prop.minor
                  << std::endl;
    }

    std::cout << std::endl;
    std::cout << "Peer access matrix:" << std::endl;

    for (int src = 0; src < device_count; ++src) {
        for (int dst = 0; dst < device_count; ++dst) {
            if (src == dst) {
                continue;
            }

            int can_access_peer = 0;
            err = cudaDeviceCanAccessPeer(&can_access_peer, src, dst);
            if (err != cudaSuccess) {
                std::cerr << "  " << src << " -> " << dst << ": "
                          << "cudaDeviceCanAccessPeer failed: "
                          << cudaGetErrorString(err) << std::endl;
                continue;
            }

            std::cout << "  " << src << " -> " << dst
                      << ": can_access_peer=" << bool_text(can_access_peer != 0);

            if (can_access_peer) {
                err = cudaSetDevice(src);
                if (err != cudaSuccess) {
                    std::cout << ", enable_peer_access=failed(set_device: "
                              << cudaGetErrorString(err) << ")";
                } else {
                    const cudaError_t enable_err = cudaDeviceEnablePeerAccess(dst, 0);
                    if (enable_err == cudaSuccess || enable_err == cudaErrorPeerAccessAlreadyEnabled) {
                        std::cout << ", enable_peer_access=yes";
                    } else {
                        std::cout << ", enable_peer_access=failed("
                                  << cudaGetErrorString(enable_err) << ")";
                    }
                }
            }

            std::cout << std::endl;
        }
    }

    return EXIT_SUCCESS;
}
