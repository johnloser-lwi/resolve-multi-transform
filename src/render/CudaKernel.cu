// CUDA render path.
//
// Note what is NOT here: any transform, easing or sampling maths. AnimEngine.h,
// MotionBlur.h and Sampler.h are compiled by nvcc as device code and produce
// results comparable to the CPU renderer, which is the point of keeping them
// header-only and device-safe. Two hand-written copies of the easing solver
// would drift apart, and the divergence would show up as a subtle timing
// difference that is very hard to see and even harder to attribute.

#include "AnimEngine.h"
#include "MotionBlur.h"
#include "Sampler.h"

#include <cuda_runtime.h>

__global__ void MultiTransformKernel(const float* __restrict__ src,
                                     int srcWidth, int srcHeight, int srcRowFloats,
                                     float* __restrict__ dst,
                                     int dstWidth, int dstHeight, int dstRowFloats,
                                     mtx::SampleTransforms st,
                                     int filterMode, int edgeMode)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dstWidth || y >= dstHeight) return;

    mtx::ImageView srcView;
    srcView.data            = src;
    srcView.width           = srcWidth;
    srcView.height          = srcHeight;
    srcView.rowStrideFloats = srcRowFloats;

    float out[4];
    mtx::RenderPixel(srcView, st,
                     static_cast<float>(x) + 0.5f,
                     static_cast<float>(y) + 0.5f,
                     static_cast<mtx::FilterMode>(filterMode),
                     static_cast<mtx::EdgeMode>(edgeMode),
                     out);

    float* p = dst + static_cast<size_t>(y) * dstRowFloats + static_cast<size_t>(x) * 4;
    p[0] = out[0]; p[1] = out[1]; p[2] = out[2]; p[3] = out[3];
}

void RunMultiTransformCuda(void* pStream,
                           const float* src, int srcWidth, int srcHeight, int srcRowFloats,
                           float* dst, int dstWidth, int dstHeight, int dstRowFloats,
                           const mtx::SampleTransforms& st,
                           int filterMode, int edgeMode)
{
    if (dstWidth <= 0 || dstHeight <= 0) return;

    cudaStream_t stream = static_cast<cudaStream_t>(pStream);

    const dim3 block(16, 16);
    const dim3 grid((dstWidth  + block.x - 1) / block.x,
                    (dstHeight + block.y - 1) / block.y);

    MultiTransformKernel<<<grid, block, 0, stream>>>(
        src, srcWidth, srcHeight, srcRowFloats,
        dst, dstWidth, dstHeight, dstRowFloats,
        st, filterMode, edgeMode);
}

/** @brief Synchronous variant for the parity test, which has no host stream and
 *  needs the result back before it can compare anything. */
void RunMultiTransformCudaSync(const float* hostSrc, int srcWidth, int srcHeight,
                               float* hostDst, int dstWidth, int dstHeight,
                               const mtx::SampleTransforms& st,
                               int filterMode, int edgeMode)
{
    const size_t srcBytes = static_cast<size_t>(srcWidth) * srcHeight * 4 * sizeof(float);
    const size_t dstBytes = static_cast<size_t>(dstWidth) * dstHeight * 4 * sizeof(float);

    float* dSrc = nullptr;
    float* dDst = nullptr;
    if (cudaMalloc(&dSrc, srcBytes) != cudaSuccess) return;
    if (cudaMalloc(&dDst, dstBytes) != cudaSuccess) { cudaFree(dSrc); return; }

    cudaMemcpy(dSrc, hostSrc, srcBytes, cudaMemcpyHostToDevice);

    RunMultiTransformCuda(nullptr,
                          dSrc, srcWidth, srcHeight, srcWidth * 4,
                          dDst, dstWidth, dstHeight, dstWidth * 4,
                          st, filterMode, edgeMode);

    cudaDeviceSynchronize();
    cudaMemcpy(hostDst, dDst, dstBytes, cudaMemcpyDeviceToHost);

    cudaFree(dSrc);
    cudaFree(dDst);
}
