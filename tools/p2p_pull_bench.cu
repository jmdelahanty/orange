// Die-to-die copy benchmark for the A16 pairs (diagnostic, 2026-09-19).
// One NV12 frame (4512x4512) per copy; "pull" is issued on the destination
// die (the recorder's early peer staging shape), "push" on the source die.
// Build: nvcc -O2 -arch=sm_86 -o targets/native/p2p_pull_bench tools/p2p_pull_bench.cu
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#define CK(x) do{cudaError_t e=(x); if(e!=cudaSuccess){printf("%s: %s\n",#x,cudaGetErrorString(e)); exit(1);} }while(0)
static void bench(int src, int dst, bool pull, size_t bytes, void* psrc, void* pdst, cudaStream_t s_src, cudaStream_t s_dst)
{
  int dev = pull ? dst : src; cudaStream_t s = pull ? s_dst : s_src;
  CK(cudaSetDevice(dev));
  cudaEvent_t t0, t1; CK(cudaEventCreate(&t0)); CK(cudaEventCreate(&t1));
  for (int w = 0; w < 3; w++) CK(cudaMemcpyPeerAsync(pdst, dst, psrc, src, bytes, s));
  CK(cudaStreamSynchronize(s));
  const int n = 20; CK(cudaEventRecord(t0, s));
  for (int i = 0; i < n; i++) CK(cudaMemcpyPeerAsync(pdst, dst, psrc, src, bytes, s));
  CK(cudaEventRecord(t1, s)); CK(cudaStreamSynchronize(s));
  float ms = 0; CK(cudaEventElapsedTime(&ms, t0, t1));
  printf("pair %d->%d %s: %.2f ms/frame, %.2f GB/s\n", src, dst, pull ? "pull (issued on dst)" : "push (issued on src)", ms / n, bytes * n / 1e6 / ms);
  cudaEventDestroy(t0); cudaEventDestroy(t1);
}
int main(int argc, char** argv)
{
  const size_t bytes = 4512ull * 4512 * 3 / 2;
  const bool pull_only = argc > 1 && argv[1][0] == 'p';
  int pairs[4][2] = {{1, 2}, {3, 4}, {5, 6}, {7, 8}};
  for (int p = 0; p < 4; p++) {
    int a = pairs[p][0], b = pairs[p][1]; void *pa, *pb; cudaStream_t sa, sb; int can = 0;
    CK(cudaSetDevice(a)); CK(cudaMalloc(&pa, bytes)); CK(cudaStreamCreate(&sa));
    CK(cudaDeviceCanAccessPeer(&can, a, b)); if (can) cudaDeviceEnablePeerAccess(b, 0);
    CK(cudaSetDevice(b)); CK(cudaMalloc(&pb, bytes)); CK(cudaStreamCreate(&sb));
    CK(cudaDeviceCanAccessPeer(&can, b, a)); if (can) cudaDeviceEnablePeerAccess(a, 0);
    bench(a, b, true, bytes, pa, pb, sa, sb);
    if (!pull_only) { bench(a, b, false, bytes, pa, pb, sa, sb); bench(b, a, true, bytes, pb, pa, sb, sa); }
    CK(cudaSetDevice(a)); cudaFree(pa); CK(cudaSetDevice(b)); cudaFree(pb);
  }
  return 0;
}
