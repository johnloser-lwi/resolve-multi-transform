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

__global__ void MultiTransformKernel(const unsigned char* __restrict__ src,
                                     int srcWidth, int srcHeight, int srcRowBytes, int srcDepth,
                                     unsigned char* __restrict__ dst,
                                     int dstWidth, int dstHeight, int dstRowBytes, int dstDepth,
                                     mtx::SampleTransforms st,
                                     int filterMode, int edgeMode,
                                     int srcOriginX, int srcOriginY)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dstWidth || y >= dstHeight) return;

    mtx::ImageView srcView;
    srcView.data           = src;
    srcView.width          = srcWidth;
    srcView.height         = srcHeight;
    srcView.rowStrideBytes = srcRowBytes;
    srcView.depth          = srcDepth;
    srcView.originX        = srcOriginX;
    srcView.originY        = srcOriginY;

    float out[4];
    mtx::RenderPixel(srcView, st,
                     static_cast<float>(x) + 0.5f,
                     static_cast<float>(y) + 0.5f,
                     static_cast<mtx::FilterMode>(filterMode),
                     static_cast<mtx::EdgeMode>(edgeMode),
                     out);

    // The depth switch is uniform across the launch, so it costs a predicted
    // branch and nothing more.
    unsigned char* p = dst + static_cast<size_t>(y) * dstRowBytes
                           + static_cast<size_t>(x) * 4 * mtx::BytesPerChannel(dstDepth);
    mtx::StorePixel(p, dstDepth, out);
}

const char* RunMultiTransformCuda(void* pStream,
                                  const void* src, int srcWidth, int srcHeight,
                                  int srcRowBytes, int srcDepth,
                                  void* dst, int dstWidth, int dstHeight,
                                  int dstRowBytes, int dstDepth,
                                  const mtx::SampleTransforms& st,
                                  int filterMode, int edgeMode,
                                  int srcOriginX, int srcOriginY)
{
    if (!dst || dstWidth <= 0 || dstHeight <= 0) return nullptr;

    cudaStream_t stream = static_cast<cudaStream_t>(pStream);

    // No source, or an empty one, renders as transparent -- and it has to be
    // *written*, not skipped. Returning early leaves the destination holding
    // whatever the host's buffer held last, which shows up as a stale or torn
    // frame exactly when a node's input vanishes mid-scrub. Cleared by rows,
    // since the pitch need not equal the width. All-zero bytes are zero in
    // every depth, so one memset serves them all.
    if (!src || srcWidth <= 0 || srcHeight <= 0)
    {
        const cudaError_t clr = cudaMemset2DAsync(
            dst, static_cast<size_t>(dstRowBytes), 0,
            static_cast<size_t>(dstWidth) * 4 * mtx::BytesPerChannel(dstDepth),
            static_cast<size_t>(dstHeight), stream);
        if (clr != cudaSuccess) return cudaGetErrorString(clr);
        if (stream == nullptr)
        {
            const cudaError_t syncErr = cudaDeviceSynchronize();
            if (syncErr != cudaSuccess) return cudaGetErrorString(syncErr);
        }
        return nullptr;
    }

    const dim3 block(16, 16);
    const dim3 grid((dstWidth  + block.x - 1) / block.x,
                    (dstHeight + block.y - 1) / block.y);

    MultiTransformKernel<<<grid, block, 0, stream>>>(
        static_cast<const unsigned char*>(src), srcWidth, srcHeight, srcRowBytes, srcDepth,
        static_cast<unsigned char*>(dst),       dstWidth, dstHeight, dstRowBytes, dstDepth,
        st, filterMode, edgeMode, srcOriginX, srcOriginY);

    // Synchronise ourselves when the host did not give us a stream.
    //
    // Declaring setSupportsCudaStream(true) is a promise that we render on the
    // host's stream and that the host will synchronise it. That promise cannot
    // hold if there is no stream: the launch goes to the default stream and
    // returns immediately, so the host is free to read the output buffer while
    // the kernel is still writing it. The result is intermittently torn or
    // stale frames -- fine in a host that always supplies a stream, broken in
    // one that does not. The SDK spells this out: a plugin using the default
    // stream must synchronise before returning.
    if (stream == nullptr)
    {
        const cudaError_t syncErr = cudaDeviceSynchronize();
        if (syncErr != cudaSuccess) return cudaGetErrorString(syncErr);
    }

    const cudaError_t err = cudaGetLastError();
    return (err == cudaSuccess) ? nullptr : cudaGetErrorString(err);
}

/** @brief Time the kernel alone: buffers are allocated and uploaded once, so the
 *  result excludes host/device transfer and reflects what Resolve actually pays
 *  per frame (it hands us pointers to memory already on the device). */
float BenchMultiTransformCuda(const float* hostSrc, int width, int height,
                              const mtx::SampleTransforms& st,
                              int filterMode, int edgeMode, int iterations)
{
    const size_t bytes = static_cast<size_t>(width) * height * 4 * sizeof(float);

    float* dSrc = nullptr;
    float* dDst = nullptr;
    if (cudaMalloc(&dSrc, bytes) != cudaSuccess) return -1.0f;
    if (cudaMalloc(&dDst, bytes) != cudaSuccess) { cudaFree(dSrc); return -1.0f; }
    cudaMemcpy(dSrc, hostSrc, bytes, cudaMemcpyHostToDevice);

    // Warm up: the first launch pays JIT/context costs that would otherwise be
    // charged to the measurement.
    RunMultiTransformCuda(nullptr, dSrc, width, height, width * 16, mtx::kDepthFloat,
                          dDst, width, height, width * 16, mtx::kDepthFloat,
                          st, filterMode, edgeMode, 0, 0);
    cudaDeviceSynchronize();

    cudaEvent_t start, stop;
    cudaEventCreate(&start);
    cudaEventCreate(&stop);
    cudaEventRecord(start);

    for (int i = 0; i < iterations; ++i)
    {
        RunMultiTransformCuda(nullptr, dSrc, width, height, width * 16, mtx::kDepthFloat,
                              dDst, width, height, width * 16, mtx::kDepthFloat, st, filterMode, edgeMode, 0, 0);
    }

    cudaEventRecord(stop);
    cudaEventSynchronize(stop);

    float ms = 0.0f;
    cudaEventElapsedTime(&ms, start, stop);

    cudaEventDestroy(start);
    cudaEventDestroy(stop);
    cudaFree(dSrc);
    cudaFree(dDst);

    return ms / static_cast<float>(iterations);
}

/** @brief Synchronous variant for the parity test, which has no host stream and
 *  needs the result back before it can compare anything. */
void RunMultiTransformCudaSync(const void* hostSrc, int srcWidth, int srcHeight,
                               void* hostDst, int dstWidth, int dstHeight,
                               const mtx::SampleTransforms& st,
                               int filterMode, int edgeMode,
                               int srcOriginX, int srcOriginY,
                               int srcDepth, int dstDepth)
{
    const int srcRowBytes = srcWidth * 4 * mtx::BytesPerChannel(srcDepth);
    const int dstRowBytes = dstWidth * 4 * mtx::BytesPerChannel(dstDepth);
    const size_t srcBytes = static_cast<size_t>(srcRowBytes) * srcHeight;
    const size_t dstBytes = static_cast<size_t>(dstRowBytes) * dstHeight;

    unsigned char* dSrc = nullptr;
    unsigned char* dDst = nullptr;

    // An empty source is a legitimate input now -- it is how the empty-image
    // path is exercised -- and cudaMalloc of zero bytes is not something to
    // lean on, so the source buffer is simply not made in that case.
    if (srcBytes > 0)
    {
        if (cudaMalloc(&dSrc, srcBytes) != cudaSuccess) return;
        cudaMemcpy(dSrc, hostSrc, srcBytes, cudaMemcpyHostToDevice);
    }
    if (cudaMalloc(&dDst, dstBytes) != cudaSuccess) { cudaFree(dSrc); return; }

    RunMultiTransformCuda(nullptr,
                          dSrc, srcWidth, srcHeight, srcRowBytes, srcDepth,
                          dDst, dstWidth, dstHeight, dstRowBytes, dstDepth,
                          st, filterMode, edgeMode, srcOriginX, srcOriginY);

    cudaDeviceSynchronize();
    cudaMemcpy(hostDst, dDst, dstBytes, cudaMemcpyDeviceToHost);

    cudaFree(dSrc);
    cudaFree(dDst);
}
