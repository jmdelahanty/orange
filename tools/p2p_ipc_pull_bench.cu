// Two-process die-to-die copy through a CUDA IPC import: ownership shapes crossed with the copy API.
//  shape recpull   : owner allocs src on die a; child imports it ON DIE a, then pulls from die b (the external recorder's shape)
//  shape dstpull   : owner allocs src on die a; child imports it on die b and pulls
//  shape srcpush   : owner allocs src on die a; child imports it on die a and pushes to its own buffer on die b
//  shape ownerpush : child owns dst on die b and exports it; owner imports it on die a and pushes from a
//  api plain|peer  : cudaMemcpyAsync(DeviceToDevice) | cudaMemcpyPeerAsync
// Build: nvcc -O2 -arch=sm_86 -o targets/native/p2p_ipc_pull_bench tools/p2p_ipc_pull_bench.cu
// Run:   targets/native/p2p_ipc_pull_bench <pair 0..3> <shape> <api>
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#define CK(x) do{cudaError_t e=(x); if(e!=cudaSuccess){printf("%s: %s\n",#x,cudaGetErrorString(e)); _exit(1);} }while(0)
static float timed_copies(void* dst, int ddev, const void* src, int sdev, size_t bytes, cudaStream_t s, bool peer_api)
{
  cudaEvent_t t0, t1; CK(cudaEventCreate(&t0)); CK(cudaEventCreate(&t1));
  for (int w = 0; w < 3; w++) { if (peer_api) CK(cudaMemcpyPeerAsync(dst, ddev, src, sdev, bytes, s)); else CK(cudaMemcpyAsync(dst, src, bytes, cudaMemcpyDeviceToDevice, s)); }
  CK(cudaStreamSynchronize(s));
  const int n = 20; CK(cudaEventRecord(t0, s));
  for (int i = 0; i < n; i++) { if (peer_api) CK(cudaMemcpyPeerAsync(dst, ddev, src, sdev, bytes, s)); else CK(cudaMemcpyAsync(dst, src, bytes, cudaMemcpyDeviceToDevice, s)); }
  CK(cudaEventRecord(t1, s)); CK(cudaStreamSynchronize(s));
  float ms = 0; CK(cudaEventElapsedTime(&ms, t0, t1)); return ms / n;
}
static void enable_peer(int from, int to) { int can = 0; CK(cudaSetDevice(from)); CK(cudaDeviceCanAccessPeer(&can, from, to)); if (can) cudaDeviceEnablePeerAccess(to, 0); }
int main(int argc, char** argv)
{
  const size_t bytes = 4512ull * 4512 * 3 / 2;
  int pairs[4][2] = {{1, 2}, {3, 4}, {5, 6}, {7, 8}};
  const int p = argc > 1 ? atoi(argv[1]) : 0; const char* shape = argc > 2 ? argv[2] : "recpull"; const bool peer_api = argc > 3 && strcmp(argv[3], "peer") == 0;
  int a = pairs[p][0], b = pairs[p][1];
  int p2c[2], c2p[2]; if (pipe(p2c) || pipe(c2p)) return 1;
  pid_t pid = fork();
  if (pid == 0) {
    cudaIpcMemHandle_t h; float ms = 0;
    if (strcmp(shape, "ownerpush") == 0) {
      CK(cudaSetDevice(b)); void* dst = nullptr; CK(cudaMalloc(&dst, bytes));
      CK(cudaIpcGetMemHandle(&h, dst)); if (write(c2p[1], &h, sizeof(h)) != (ssize_t)sizeof(h)) _exit(2);
      char done; if (read(p2c[0], &done, 1) != 1) _exit(3); cudaFree(dst); _exit(0);
    }
    if (read(p2c[0], &h, sizeof(h)) != (ssize_t)sizeof(h)) _exit(2);
    void *src = nullptr, *dst = nullptr; cudaStream_t s;
    if (strcmp(shape, "recpull") == 0) {         // import on a (as the recorder's intake does), pull from b
      enable_peer(b, a);
      CK(cudaSetDevice(a)); CK(cudaIpcOpenMemHandle(&src, h, cudaIpcMemLazyEnablePeerAccess));
      CK(cudaSetDevice(b)); CK(cudaMalloc(&dst, bytes)); CK(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking));
      ms = timed_copies(dst, b, src, a, bytes, s, peer_api);
    } else if (strcmp(shape, "dstpull") == 0) {  // import on b, pull from b
      CK(cudaSetDevice(b)); CK(cudaIpcOpenMemHandle(&src, h, cudaIpcMemLazyEnablePeerAccess));
      CK(cudaMalloc(&dst, bytes)); CK(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking));
      ms = timed_copies(dst, b, src, a, bytes, s, peer_api);
    } else {                                     // srcpush: import on a, push from a
      CK(cudaSetDevice(b)); CK(cudaMalloc(&dst, bytes)); enable_peer(a, b);
      CK(cudaSetDevice(a)); CK(cudaIpcOpenMemHandle(&src, h, cudaIpcMemLazyEnablePeerAccess));
      CK(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking));
      ms = timed_copies(dst, b, src, a, bytes, s, peer_api);
    }
    printf("pair %d->%d %-9s %-5s %.2f ms/frame %.2f GB/s\n", a, b, shape, peer_api ? "peer" : "plain", ms, bytes / 1e6 / ms); fflush(stdout); _exit(0);
  }
  if (strcmp(shape, "ownerpush") == 0) {
    cudaIpcMemHandle_t h; if (read(c2p[0], &h, sizeof(h)) != (ssize_t)sizeof(h)) return 4;
    enable_peer(a, b); CK(cudaSetDevice(a)); void* src = nullptr; CK(cudaMalloc(&src, bytes)); CK(cudaMemset(src, 0x80, bytes));
    void* dst = nullptr; CK(cudaIpcOpenMemHandle(&dst, h, cudaIpcMemLazyEnablePeerAccess));
    cudaStream_t s; CK(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking));
    float ms = timed_copies(dst, b, src, a, bytes, s, peer_api);
    printf("pair %d->%d %-9s %-5s %.2f ms/frame %.2f GB/s\n", a, b, shape, peer_api ? "peer" : "plain", ms, bytes / 1e6 / ms);
    CK(cudaIpcCloseMemHandle(dst)); char done = 1; if (write(p2c[1], &done, 1) != 1) return 5;
  } else {
    CK(cudaSetDevice(a)); void* src = nullptr; CK(cudaMalloc(&src, bytes)); CK(cudaMemset(src, 0x80, bytes));
    cudaIpcMemHandle_t h; CK(cudaIpcGetMemHandle(&h, src)); if (write(p2c[1], &h, sizeof(h)) != (ssize_t)sizeof(h)) return 3;
  }
  int status = 0; waitpid(pid, &status, 0); return 0;
}
