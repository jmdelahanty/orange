// One 20 MB peer copy (copy engine) from gpu a to gpu b every period_us, for seconds s.
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#define CK(x) do{cudaError_t e=(x); if(e!=cudaSuccess){printf("CUDA error %s: %s\n",#x,cudaGetErrorString(e)); exit(1);} }while(0)
int main(int argc,char**argv){ int a=atoi(argv[1]), b=atoi(argv[2]); int period_us=argc>3?atoi(argv[3]):10000; int secs=argc>4?atoi(argv[4]):20; const size_t bytes=20358144;
  void *pa,*pb; CK(cudaSetDevice(a)); CK(cudaMalloc(&pa,bytes)); CK(cudaMemset(pa,7,bytes)); CK(cudaSetDevice(b)); CK(cudaMalloc(&pb,bytes)); CK(cudaDeviceEnablePeerAccess(a,0));
  cudaStream_t s; CK(cudaStreamCreateWithFlags(&s,cudaStreamNonBlocking)); cudaEvent_t e0,e1; CK(cudaEventCreate(&e0)); CK(cudaEventCreate(&e1));
  auto t0=std::chrono::steady_clock::now(); long n=0; double sum=0,mx=0;
  while(std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count()<secs){ auto next=std::chrono::steady_clock::now()+std::chrono::microseconds(period_us);
    CK(cudaEventRecord(e0,s)); CK(cudaMemcpyPeerAsync(pb,b,pa,a,bytes,s)); CK(cudaEventRecord(e1,s)); CK(cudaEventSynchronize(e1)); float ms; CK(cudaEventElapsedTime(&ms,e0,e1)); sum+=ms; if(ms>mx)mx=ms; n++;
    std::this_thread::sleep_until(next); }
  printf("paced copies %d->%d: %ld copies, mean %.3f ms, max %.3f ms\n",a,b,n,sum/n,mx); return 0; }
