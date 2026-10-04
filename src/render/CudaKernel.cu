// CUDA render path.
//
// Note what is NOT here: any transform, easing or sampling maths. AnimEngine.h,
// MotionBlur.h and Sampler.h are compiled by nvcc as device code and produce
// results comparable to the CPU renderer, which is the point of keeping them
// header-only and device-safe. Two hand-written copies of the easing solver
// would drift apart, and the divergence would show up as a subtle timing
// difference that is very hard to see and even harder to attribute.

#include "AnimEngine.h"
#include "Defocus.h"
#include "MotionBlur.h"
#include "Sampler.h"

#include <cuda_runtime.h>

#include <mutex>

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

// --- Blur ("defocus") --------------------------------------------------------
//
// Three kernels around the one shared line routine in Defocus.h: render the
// transform into a padded float buffer, run the box passes over it, store the
// interior at the destination's depth. See Defocus.h for the blur itself and
// why the border exists.

/** One thread per padded pixel: the ordinary per-pixel render, into floats. */
__global__ void DefocusFillKernel(const unsigned char* __restrict__ src,
                                  int srcWidth, int srcHeight, int srcRowBytes, int srcDepth,
                                  float* __restrict__ scratch, int pw, int ph, int pad,
                                  mtx::SampleTransforms st, int filterMode, int edgeMode,
                                  int srcOriginX, int srcOriginY)
{
    const int px = blockIdx.x * blockDim.x + threadIdx.x;
    const int py = blockIdx.y * blockDim.y + threadIdx.y;
    if (px >= pw || py >= ph) return;

    mtx::ImageView srcView;
    srcView.data           = src;
    srcView.width          = srcWidth;
    srcView.height         = srcHeight;
    srcView.rowStrideBytes = srcRowBytes;
    srcView.depth          = srcDepth;
    srcView.originX        = srcOriginX;
    srcView.originY        = srcOriginY;

    mtx::RenderPixel(srcView, st,
                     static_cast<float>(px - pad) + 0.5f,
                     static_cast<float>(py - pad) + 0.5f,
                     static_cast<mtx::FilterMode>(filterMode),
                     static_cast<mtx::EdgeMode>(edgeMode),
                     scratch + (static_cast<size_t>(py) * pw + px) * 4);
}

// Why the GPU does not just run Defocus.h's line routine one thread per line,
// as Text Animator does on its small per-glyph tiles: on a full UHD frame that
// is ~2000 threads walking ~4000 pixels each, with every warp of a row pass
// reading 32 different rows. Measured, it cost ~15 ms a frame -- fifty times
// the transform itself. Two changes, same arithmetic:
//
//  * Segmented: each thread runs the running window over a short stretch of
//    its line, seeding the window by summing it directly. Ten-odd times the
//    threads, a tenth of the walk each.
//  * Columns only: a column pass has neighbouring threads reading neighbouring
//    pixels -- coalesced -- where a row pass scatters. So all three passes of
//    one axis run down columns, the image is transposed, and the other axis's
//    three passes run down columns too.
//
// The box passes are linear and act on independent axes, so rows-then-columns
// and columns-then-rows are the same image; only float rounding differs from
// the CPU's order, and the parity test bounds that.

/** Pixels of a column per thread. Each segment pays one window's worth of
 *  additions to seed its sum, so the segment grows with the window to keep
 *  that seeding to about a tenth of the walk -- at least 256, where small
 *  radii want maximum parallelism. */
static int DefocusSegment(float halfWidth)
{
    const int window = 2 * static_cast<int>(halfWidth) + 1;
    return window * 10 > 256 ? window * 10 : 256;
}

/** One box pass down every column of a w x h RGBA image, one thread per
 *  (column, segment). Same window and edge rule as DefocusBoxLine. */
__global__ void DefocusColumnKernel(const float4* __restrict__ in, float4* __restrict__ out,
                                    int w, int h, float halfWidth, int segment)
{
    const int x  = blockIdx.x * blockDim.x + threadIdx.x;
    const int y0 = blockIdx.y * segment;
    if (x >= w || y0 >= h) return;
    const int y1 = min(h, y0 + segment);

    const int   r   = static_cast<int>(halfWidth);
    const float f   = halfWidth - static_cast<float>(r);
    const float inv = 1.0f / (2.0f * static_cast<float>(r) + 1.0f + 2.0f * f);

    // The window centred on y0, clipped to the image -- what the line walk
    // would be holding on arriving at y0.
    float4 sum = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int k = max(0, y0 - r); k <= min(h - 1, y0 + r); ++k)
    {
        const float4 v = in[static_cast<size_t>(k) * w + x];
        sum.x += v.x; sum.y += v.y; sum.z += v.z; sum.w += v.w;
    }

    for (int i = y0; i < y1; ++i)
    {
        float4 edge = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
        const int lo = i - r - 1, hi = i + r + 1;
        if (lo >= 0) { const float4 v = in[static_cast<size_t>(lo) * w + x];
                       edge.x += v.x; edge.y += v.y; edge.z += v.z; edge.w += v.w; }
        if (hi < h)  { const float4 v = in[static_cast<size_t>(hi) * w + x];
                       edge.x += v.x; edge.y += v.y; edge.z += v.z; edge.w += v.w; }

        out[static_cast<size_t>(i) * w + x] =
            make_float4((sum.x + f * edge.x) * inv, (sum.y + f * edge.y) * inv,
                        (sum.z + f * edge.z) * inv, (sum.w + f * edge.w) * inv);

        if (hi < h)  { const float4 v = in[static_cast<size_t>(hi) * w + x];
                       sum.x += v.x; sum.y += v.y; sum.z += v.z; sum.w += v.w; }
        const int drop = i - r;
        if (drop >= 0) { const float4 v = in[static_cast<size_t>(drop) * w + x];
                         sum.x -= v.x; sum.y -= v.y; sum.z -= v.z; sum.w -= v.w; }
    }
}

/** Tiled transpose: in is w x h, out is h x w. Through shared memory so both
 *  the read and the write are coalesced. */
__global__ void TransposeKernel(const float4* __restrict__ in, float4* __restrict__ out,
                                int w, int h)
{
    __shared__ float4 tile[32][33];   // +1 column: no bank conflicts on the read-back

    int x = blockIdx.x * 32 + threadIdx.x;
    int y = blockIdx.y * 32 + threadIdx.y;
    for (int j = 0; j < 32; j += 8)
        if (x < w && y + j < h) tile[threadIdx.y + j][threadIdx.x] = in[static_cast<size_t>(y + j) * w + x];

    __syncthreads();

    x = blockIdx.y * 32 + threadIdx.x;   // now indexes out's columns (= in's rows)
    y = blockIdx.x * 32 + threadIdx.y;
    for (int j = 0; j < 32; j += 8)
        if (x < h && y + j < w) out[static_cast<size_t>(y + j) * h + x] = tile[threadIdx.x][threadIdx.y + j];
}

/** Three column passes, ping-ponging; the result lands in @p b. */
static void DefocusColumns(float4* a, float4* b, int w, int h, float halfWidth, cudaStream_t s)
{
    const int  seg = DefocusSegment(halfWidth);
    const dim3 block(128, 1);
    const dim3 grid((w + 127) / 128, (h + seg - 1) / seg);
    DefocusColumnKernel<<<grid, block, 0, s>>>(a, b, w, h, halfWidth, seg);
    DefocusColumnKernel<<<grid, block, 0, s>>>(b, a, w, h, halfWidth, seg);
    DefocusColumnKernel<<<grid, block, 0, s>>>(a, b, w, h, halfWidth, seg);
}

static void Transpose(const float4* in, float4* out, int w, int h, cudaStream_t s)
{
    TransposeKernel<<<dim3((w + 31) / 32, (h + 31) / 32), dim3(32, 8), 0, s>>>(in, out, w, h);
}

/** One thread per destination pixel: the padded buffer's interior, stored. */
__global__ void DefocusStoreKernel(const float* __restrict__ scratch, int pw, int pad,
                                   unsigned char* __restrict__ dst,
                                   int dstWidth, int dstHeight, int dstRowBytes, int dstDepth)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dstWidth || y >= dstHeight) return;

    const float* v = scratch + (static_cast<size_t>(y + pad) * pw + (x + pad)) * 4;
    unsigned char* p = dst + static_cast<size_t>(y) * dstRowBytes
                           + static_cast<size_t>(x) * 4 * mtx::BytesPerChannel(dstDepth);
    mtx::StorePixel(p, dstDepth, v);
}

namespace {

/** @brief This plugin's own stream-ordered memory pool, one per device.
 *
 *  Not the device's default pool. That one belongs to the host as much as to
 *  us -- Resolve may allocate from it -- and its release threshold defaults to
 *  zero, which returns every unused byte to the OS at each synchronisation.
 *  The host synchronises every frame, so the blur's ~270 MB of UHD scratch was
 *  being unmapped and remapped per frame: measured as roughly half the blur's
 *  cost. A private pool can keep its memory between frames without changing
 *  how the host's own allocations behave.
 *
 *  The threshold bounds what an idle pool holds on to: room for the two
 *  scratch buffers of a UHD blur, with headroom for a render or two in flight
 *  at once. Above it, freed memory goes back to the OS as before. */
cudaMemPool_t PluginPool()
{
    constexpr int kMaxDevices = 16;
    static cudaMemPool_t pools[kMaxDevices] = {};
    static std::mutex    lock;

    int device = 0;
    if (cudaGetDevice(&device) != cudaSuccess || device < 0 || device >= kMaxDevices)
    {
        cudaGetLastError();
        return nullptr;
    }

    std::lock_guard<std::mutex> hold(lock);
    if (pools[device]) return pools[device];

    cudaMemPoolProps props = {};
    props.allocType     = cudaMemAllocationTypePinned;
    props.location.type = cudaMemLocationTypeDevice;
    props.location.id   = device;

    cudaMemPool_t pool = nullptr;
    if (cudaMemPoolCreate(&pool, &props) != cudaSuccess) { cudaGetLastError(); return nullptr; }

    unsigned long long keep = 768ull * 1024ull * 1024ull;   // the attribute is a cuuint64_t
    cudaMemPoolSetAttribute(pool, cudaMemPoolAttrReleaseThreshold, &keep);
    pools[device] = pool;
    return pool;
}

/** @brief Render + blur + store. Returns false only if no scratch memory could
 *  be had at all, in which case the caller renders sharp instead.
 *
 *  Stream-ordered allocation (cudaMallocAsync) first: it is pooled, so a
 *  per-frame allocation costs next to nothing, and it is ordered on the very
 *  stream the kernels run on, so concurrent renders -- Fusion runs one
 *  instance on several threads at once -- each get their own buffers with no
 *  shared state to race on. A plain cudaMalloc is the fallback for a driver
 *  without memory pools; that path synchronises before freeing. */
bool RenderDefocused(cudaStream_t stream,
                     const unsigned char* src, int srcWidth, int srcHeight,
                     int srcRowBytes, int srcDepth,
                     unsigned char* dst, int dstWidth, int dstHeight,
                     int dstRowBytes, int dstDepth,
                     const mtx::SampleTransforms& st, int filterMode, int edgeMode,
                     int srcOriginX, int srcOriginY, float defocusPx)
{
    const int    pad   = mtx::DefocusPad(defocusPx);
    const int    pw    = dstWidth + 2 * pad, ph = dstHeight + 2 * pad;
    const size_t bytes = static_cast<size_t>(pw) * static_cast<size_t>(ph) * 4 * sizeof(float);

    float* a = nullptr;
    float* b = nullptr;
    cudaMemPool_t pool = PluginPool();
    bool pooled = pool &&
        cudaMallocFromPoolAsync(reinterpret_cast<void**>(&a), bytes, pool, stream) == cudaSuccess;
    if (pooled &&
        cudaMallocFromPoolAsync(reinterpret_cast<void**>(&b), bytes, pool, stream) != cudaSuccess)
    {
        cudaFreeAsync(a, stream);
        a = nullptr;
        pooled = false;
    }
    if (!pooled)
    {
        cudaGetLastError();   // clear the failed async allocation's error
        if (cudaMalloc(reinterpret_cast<void**>(&a), bytes) != cudaSuccess) { cudaGetLastError(); return false; }
        if (cudaMalloc(reinterpret_cast<void**>(&b), bytes) != cudaSuccess)
        {
            cudaFree(a);
            cudaGetLastError();
            return false;
        }
    }

    const dim3 block(16, 16);
    DefocusFillKernel<<<dim3((pw + 15) / 16, (ph + 15) / 16), block, 0, stream>>>(
        src, srcWidth, srcHeight, srcRowBytes, srcDepth,
        a, pw, ph, pad, st, filterMode, edgeMode, srcOriginX, srcOriginY);

    // Vertical passes on the frame as rendered, then the horizontal ones as
    // column passes on its transpose, then back. Ends in `a`, pw x ph again.
    const float hw = mtx::DefocusHalfWidth(defocusPx);
    float4* A = reinterpret_cast<float4*>(a);
    float4* B = reinterpret_cast<float4*>(b);
    DefocusColumns(A, B, pw, ph, hw, stream);   // -> B  (pw x ph)
    Transpose(B, A, pw, ph, stream);            // -> A  (ph x pw)
    DefocusColumns(A, B, ph, pw, hw, stream);   // -> B  (ph x pw)
    Transpose(B, A, ph, pw, stream);            // -> A  (pw x ph)

    DefocusStoreKernel<<<dim3((dstWidth + 15) / 16, (dstHeight + 15) / 16), block, 0, stream>>>(
        a, pw, pad, dst, dstWidth, dstHeight, dstRowBytes, dstDepth);

    if (pooled)
    {
        cudaFreeAsync(a, stream);
        cudaFreeAsync(b, stream);
    }
    else
    {
        cudaStreamSynchronize(stream);
        cudaFree(a);
        cudaFree(b);
    }
    return true;
}

} // namespace

const char* RunMultiTransformCuda(void* pStream,
                                  const void* src, int srcWidth, int srcHeight,
                                  int srcRowBytes, int srcDepth,
                                  void* dst, int dstWidth, int dstHeight,
                                  int dstRowBytes, int dstDepth,
                                  const mtx::SampleTransforms& st,
                                  int filterMode, int edgeMode,
                                  int srcOriginX, int srcOriginY,
                                  float defocusPx)
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

    const bool blurred = mtx::DefocusActive(defocusPx) &&
        RenderDefocused(stream,
                        static_cast<const unsigned char*>(src), srcWidth, srcHeight,
                        srcRowBytes, srcDepth,
                        static_cast<unsigned char*>(dst), dstWidth, dstHeight,
                        dstRowBytes, dstDepth,
                        st, filterMode, edgeMode, srcOriginX, srcOriginY, defocusPx);

    if (!blurred)
    {
        const dim3 block(16, 16);
        const dim3 grid((dstWidth  + block.x - 1) / block.x,
                        (dstHeight + block.y - 1) / block.y);

        MultiTransformKernel<<<grid, block, 0, stream>>>(
            static_cast<const unsigned char*>(src), srcWidth, srcHeight, srcRowBytes, srcDepth,
            static_cast<unsigned char*>(dst),       dstWidth, dstHeight, dstRowBytes, dstDepth,
            st, filterMode, edgeMode, srcOriginX, srcOriginY);
    }

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
                              int filterMode, int edgeMode, int iterations,
                              float defocusPx)
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
                          st, filterMode, edgeMode, 0, 0, defocusPx);
    cudaDeviceSynchronize();

    cudaEvent_t start, stop;
    cudaEventCreate(&start);
    cudaEventCreate(&stop);
    cudaEventRecord(start);

    for (int i = 0; i < iterations; ++i)
    {
        RunMultiTransformCuda(nullptr, dSrc, width, height, width * 16, mtx::kDepthFloat,
                              dDst, width, height, width * 16, mtx::kDepthFloat, st, filterMode, edgeMode, 0, 0, defocusPx);
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
                               int srcDepth, int dstDepth,
                               float defocusPx)
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
                          st, filterMode, edgeMode, srcOriginX, srcOriginY, defocusPx);

    cudaDeviceSynchronize();
    cudaMemcpy(hostDst, dDst, dstBytes, cudaMemcpyDeviceToHost);

    cudaFree(dSrc);
    cudaFree(dDst);
}
