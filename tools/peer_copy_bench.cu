// 20 MB peer copies between two GPUs: copy engine (cudaMemcpyPeerAsync), and a
// kernel reading the peer mapping directly (what a consumer that "pulls" does).
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#define CK(x) do{cudaError_t e=(x); if(e!=cudaSuccess){printf("CUDA error %s at %d: %s\n",#x,__LINE__,cudaGetErrorString(e)); exit(1);} }while(0)
__global__ void pull_kernel(const uint4* __restrict__ src, uint4* __restrict__ dst, size_t n){
    size_t i=blockIdx.x*(size_t)blockDim.x+threadIdx.x; size_t s=(size_t)gridDim.x*blockDim.x;
    for(; i<n; i+=s) dst[i]=src[i];
}
int main(int argc,char**argv){
    int a=argc>1?atoi(argv[1]):1, b=argc>2?atoi(argv[2]):2; const size_t bytes=20358144; const int iters=40;
    int can=0; CK(cudaDeviceCanAccessPeer(&can,b,a)); printf("gpu %d can access peer %d: %d\n",b,a,can);
    void *pa,*pb; CK(cudaSetDevice(a)); CK(cudaMalloc(&pa,bytes)); CK(cudaMemset(pa,7,bytes));
    CK(cudaSetDevice(b)); CK(cudaMalloc(&pb,bytes)); if(can) CK(cudaDeviceEnablePeerAccess(a,0));
    cudaStream_t sb; CK(cudaStreamCreateWithFlags(&sb,cudaStreamNonBlocking)); cudaEvent_t e0,e1; CK(cudaEventCreate(&e0)); CK(cudaEventCreate(&e1));
    auto report=[&](const char* name, std::vector<float>& t){ std::sort(t.begin(),t.end()); float med=t[t.size()/2]; printf("%-44s median %.3f ms  p95 %.3f ms  -> %.2f GB/s\n",name,med,t[t.size()*95/100],bytes/med/1e6); };
    std::vector<float> t;
    // 1) copy engine, issued from the destination device (pull by CE)
    for(int i=0;i<iters+3;i++){ CK(cudaEventRecord(e0,sb)); CK(cudaMemcpyPeerAsync(pb,b,pa,a,bytes,sb)); CK(cudaEventRecord(e1,sb)); CK(cudaEventSynchronize(e1)); float ms; CK(cudaEventElapsedTime(&ms,e0,e1)); if(i>=3) t.push_back(ms);} report("cudaMemcpyPeerAsync dst-side stream (CE pull)",t); t.clear();
    // 2) copy engine issued from the source device (push)
    { CK(cudaSetDevice(a)); cudaStream_t sa; CK(cudaStreamCreateWithFlags(&sa,cudaStreamNonBlocking)); cudaEvent_t f0,f1; CK(cudaEventCreate(&f0)); CK(cudaEventCreate(&f1)); if(can) cudaDeviceEnablePeerAccess(b,0);
      for(int i=0;i<iters+3;i++){ CK(cudaEventRecord(f0,sa)); CK(cudaMemcpyPeerAsync(pb,b,pa,a,bytes,sa)); CK(cudaEventRecord(f1,sa)); CK(cudaEventSynchronize(f1)); float ms; CK(cudaEventElapsedTime(&ms,f0,f1)); if(i>=3) t.push_back(ms);} report("cudaMemcpyPeerAsync src-side stream (CE push)",t); t.clear(); CK(cudaSetDevice(b)); }
    // 3) kernel on the destination reading the peer mapping (SM pull over PCIe)
    if(can){ for(int i=0;i<iters+3;i++){ CK(cudaEventRecord(e0,sb)); pull_kernel<<<1024,256,0,sb>>>((const uint4*)pa,(uint4*)pb,bytes/16); CK(cudaEventRecord(e1,sb)); CK(cudaEventSynchronize(e1)); float ms; CK(cudaEventElapsedTime(&ms,e0,e1)); if(i>=3) t.push_back(ms);} report("kernel on dst reading peer mapping (SM pull)",t); t.clear(); }
    // 4) local D2D on the source die for reference
    { CK(cudaSetDevice(a)); void* pc; CK(cudaMalloc(&pc,bytes)); cudaStream_t sa; CK(cudaStreamCreateWithFlags(&sa,cudaStreamNonBlocking)); cudaEvent_t f0,f1; CK(cudaEventCreate(&f0)); CK(cudaEventCreate(&f1));
      for(int i=0;i<iters+3;i++){ CK(cudaEventRecord(f0,sa)); CK(cudaMemcpyAsync(pc,pa,bytes,cudaMemcpyDeviceToDevice,sa)); CK(cudaEventRecord(f1,sa)); CK(cudaEventSynchronize(f1)); float ms; CK(cudaEventElapsedTime(&ms,f0,f1)); if(i>=3) t.push_back(ms);} report("local D2D on src die (reference)",t); }
    return 0;
}
