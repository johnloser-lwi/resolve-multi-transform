// Metal render path: host side. The kernels are in MetalKernels.metal; this
// file compiles them once per device, packs the per-render transforms, and
// encodes the same sequence the CUDA launcher runs:
//
//   no source        clear the destination (blit fill)
//   blur inactive    one transform dispatch
//   blur active      fill a padded float buffer -> 3 column passes ->
//                    transpose -> 3 column passes -> transpose -> store
//
// Compiled as Objective-C++ with ARC (see CMakeLists).
//
// UNTESTED ON HARDWARE: written on Windows ahead of a Mac being available.

#include "render/MetalRender.h"

#include "Defocus.h"
#include "render/MetalKernelsSource.h"   // generated: kMetalKernelSource

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

// --- Shared layout with MetalKernels.metal ----------------------------------
// Must match the shader's MTXParams / DefocusArgs exactly: scalars only, all
// four bytes, Mat3s padded to 8 floats.

constexpr int kMaxSamples = mtx::kMaxBlurSamples;

struct MTXParams
{
    int srcWidth, srcHeight, srcRowBytes, srcDepth;
    int dstWidth, dstHeight, dstRowBytes, dstDepth;
    int srcOriginX, srcOriginY, filterMode, edgeMode;
    int count, hasGhost, haveSrc, reserved0;
    int fillWidth, fillHeight, fillPad, reserved1;
    float ghostOpacity, ghostTint0, ghostTint1, ghostTint2;
    float ghostInv[8];
    float inv[kMaxSamples * 8];
    float opacity[kMaxSamples];
};
static_assert(sizeof(MTXParams) == 4 * (20 + 4 + 8 + kMaxSamples * 8 + kMaxSamples),
              "MTXParams must stay padding-free to match the shader");
// setBytes is limited to 4 KB; this keeps the transforms out of a buffer.
static_assert(sizeof(MTXParams) <= 4096, "MTXParams too large for setBytes");

struct DefocusArgs
{
    int   width, height, segment, reserved0;
    float halfWidth, reserved1, reserved2, reserved3;
};

/// Same rule as CudaKernel.cu's DefocusSegment.
int DefocusSegment(float halfWidth)
{
    const int window = 2 * static_cast<int>(halfWidth) + 1;
    return window * 10 > 256 ? window * 10 : 256;
}

// --- Per-device state: pipelines and a scratch-buffer pool -----------------

struct DeviceState
{
    id<MTLDevice>               device;
    id<MTLComputePipelineState> transform, fill, column, transpose, store;
    std::string                 error;   ///< non-empty if the shader failed to build

    // Scratch for the blur: ~270 MB at UHD, so it is pooled rather than
    // allocated per frame -- the CUDA path measured per-frame allocation as half
    // the blur's cost. Buffers return to the pool when their command buffer
    // completes, so concurrent renders (Fusion runs several at once) never share
    // one. A few idle buffers are kept; beyond that they are freed.
    std::mutex poolLock;
    struct Entry { id<MTLBuffer> buffer; bool inUse; };
    std::vector<Entry> pool;
};

std::mutex                                g_stateLock;
std::vector<std::unique_ptr<DeviceState>> g_states;

id<MTLComputePipelineState> MakePipeline(id<MTLLibrary> lib, NSString* name, DeviceState& s)
{
    id<MTLFunction> fn = [lib newFunctionWithName:name];
    if (!fn) { s.error = std::string("Metal kernel missing: ") + name.UTF8String; return nil; }
    NSError* err = nil;
    id<MTLComputePipelineState> ps = [s.device newComputePipelineStateWithFunction:fn error:&err];
    if (!ps)
        s.error = std::string("Metal pipeline failed: ") + name.UTF8String + ": "
                + (err ? err.localizedDescription.UTF8String : "unknown");
    return ps;
}

DeviceState* StateFor(id<MTLDevice> device)
{
    std::lock_guard<std::mutex> hold(g_stateLock);
    for (auto& s : g_states)
        if (s->device == device) return s.get();

    auto s = std::make_unique<DeviceState>();
    s->device = device;

    MTLCompileOptions* opts = [MTLCompileOptions new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // Fast math off: integer depths must load their maximum code as exactly 1.0
    // (a reciprocal approximation does not), and the CPU path is IEEE. The
    // property is deprecated in favour of mathMode, which needs the macOS 15
    // SDK; this spelling builds on older SDKs and still works on newer ones.
    opts.fastMathEnabled = NO;
#pragma clang diagnostic pop

    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:kMetalKernelSource]
                                              options:opts
                                                error:&err];
    if (!lib)
    {
        s->error = std::string("Metal shader failed to compile: ")
                 + (err ? err.localizedDescription.UTF8String : "unknown");
    }
    else
    {
        s->transform = MakePipeline(lib, @"mtx_transform",      *s);
        s->fill      = MakePipeline(lib, @"mtx_defocus_fill",   *s);
        s->column    = MakePipeline(lib, @"mtx_defocus_column", *s);
        s->transpose = MakePipeline(lib, @"mtx_transpose",      *s);
        s->store     = MakePipeline(lib, @"mtx_defocus_store",  *s);
    }

    g_states.push_back(std::move(s));
    return g_states.back().get();
}

id<MTLBuffer> AcquireScratch(DeviceState& s, NSUInteger bytes)
{
    {
        std::lock_guard<std::mutex> hold(s.poolLock);
        for (auto& e : s.pool)
        {
            // Big enough, but not wastefully so: a UHD buffer is not handed to
            // a 1080p render while the UHD one waits.
            if (!e.inUse && e.buffer.length >= bytes && e.buffer.length <= bytes * 2)
            {
                e.inUse = true;
                return e.buffer;
            }
        }
    }

    id<MTLBuffer> b = [s.device newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
    if (!b) return nil;

    std::lock_guard<std::mutex> hold(s.poolLock);
    s.pool.push_back({ b, true });
    return b;
}

void ReleaseScratch(DeviceState& s, id<MTLBuffer> b)
{
    std::lock_guard<std::mutex> hold(s.poolLock);
    for (auto& e : s.pool)
        if (e.buffer == b) { e.inUse = false; break; }

    // Keep at most four idle buffers: enough for a couple of renders in flight
    // with two buffers each, without holding gigabytes after a 4K blur.
    int idle = 0;
    for (auto it = s.pool.begin(); it != s.pool.end();)
    {
        if (!it->inUse && ++idle > 4) it = s.pool.erase(it);
        else ++it;
    }
}

// --- Encoding ------------------------------------------------------------------

void Dispatch(id<MTLComputeCommandEncoder> enc, id<MTLComputePipelineState> ps,
              int w, int h, int tgx, int tgy)
{
    [enc setComputePipelineState:ps];
    // Threadgroup counts rounded up; every kernel bounds-checks its threads,
    // so this works on GPUs without non-uniform threadgroup support.
    [enc dispatchThreadgroups:MTLSizeMake((w + tgx - 1) / tgx, (h + tgy - 1) / tgy, 1)
        threadsPerThreadgroup:MTLSizeMake(tgx, tgy, 1)];
}

void PackParams(MTXParams& P, const mtx::SampleTransforms& st,
                int srcWidth, int srcHeight, int srcRowBytes, int srcDepth,
                int dstWidth, int dstHeight, int dstRowBytes, int dstDepth,
                int filterMode, int edgeMode, int srcOriginX, int srcOriginY)
{
    std::memset(&P, 0, sizeof(P));
    P.srcWidth = srcWidth;   P.srcHeight = srcHeight;
    P.srcRowBytes = srcRowBytes; P.srcDepth = srcDepth;
    P.dstWidth = dstWidth;   P.dstHeight = dstHeight;
    P.dstRowBytes = dstRowBytes; P.dstDepth = dstDepth;
    P.srcOriginX = srcOriginX; P.srcOriginY = srcOriginY;
    P.filterMode = filterMode; P.edgeMode   = edgeMode;
    P.haveSrc  = 1;
    P.count    = st.count < 1 ? 1 : (st.count > kMaxSamples ? kMaxSamples : st.count);
    P.hasGhost = st.hasGhost ? 1 : 0;

    P.ghostOpacity = st.ghostOpacity;
    P.ghostTint0 = st.ghostTint[0]; P.ghostTint1 = st.ghostTint[1]; P.ghostTint2 = st.ghostTint[2];
    for (int i = 0; i < 6; ++i) P.ghostInv[i] = st.ghostInv.m[i];

    for (int k = 0; k < P.count; ++k)
    {
        for (int i = 0; i < 6; ++i) P.inv[k * 8 + i] = st.inv[k].m[i];
        P.opacity[k] = st.opacity[k];
    }
}

/** Encode and commit one render. Returns the committed command buffer through
 *  @p outCb (nil if nothing was committed), or an error string. */
const char* Encode(id<MTLCommandQueue> queue,
                   id<MTLBuffer> src, int srcWidth, int srcHeight, int srcRowBytes, int srcDepth,
                   id<MTLBuffer> dst, int dstWidth, int dstHeight, int dstRowBytes, int dstDepth,
                   const mtx::SampleTransforms& st, int filterMode, int edgeMode,
                   int srcOriginX, int srcOriginY, float defocusPx,
                   id<MTLCommandBuffer> __strong* outCb)
{
    *outCb = nil;
    if (!queue || !dst || dstWidth <= 0 || dstHeight <= 0) return nullptr;

    DeviceState* state = StateFor(queue.device);
    if (!state->error.empty()) return state->error.c_str();

    id<MTLCommandBuffer> cb = [queue commandBuffer];
    if (!cb) return "Metal: could not create a command buffer";
    cb.label = @"MultiTransform";

    // No source, or an empty one, renders transparent -- and it has to be
    // written, not skipped, or the host's buffer keeps its previous contents
    // (see RunMultiTransformCuda). All-zero bytes are zero in every depth.
    if (!src || srcWidth <= 0 || srcHeight <= 0)
    {
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        const NSUInteger want = static_cast<NSUInteger>(dstRowBytes) * static_cast<NSUInteger>(dstHeight);
        [blit fillBuffer:dst range:NSMakeRange(0, want < dst.length ? want : dst.length) value:0];
        [blit endEncoding];
        [cb commit];
        *outCb = cb;
        return nullptr;
    }

    MTXParams P;
    PackParams(P, st, srcWidth, srcHeight, srcRowBytes, srcDepth,
               dstWidth, dstHeight, dstRowBytes, dstDepth,
               filterMode, edgeMode, srcOriginX, srcOriginY);

    // Default (serial) dispatch type: each dispatch sees the previous one's
    // writes, which the blur's chain of passes relies on.
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];

    bool blurred = false;
    if (mtx::DefocusActive(defocusPx))
    {
        const int pad = mtx::DefocusPad(defocusPx);
        const int pw  = dstWidth + 2 * pad, ph = dstHeight + 2 * pad;
        const NSUInteger bytes = static_cast<NSUInteger>(pw) * static_cast<NSUInteger>(ph) * 16;

        id<MTLBuffer> a = AcquireScratch(*state, bytes);
        id<MTLBuffer> b = a ? AcquireScratch(*state, bytes) : nil;
        if (a && b)
        {
            P.fillWidth = pw; P.fillHeight = ph; P.fillPad = pad;

            [enc setBuffer:src offset:0 atIndex:0];
            [enc setBuffer:a   offset:0 atIndex:1];
            [enc setBytes:&P length:sizeof(P) atIndex:2];
            Dispatch(enc, state->fill, pw, ph, 16, 16);

            // Vertical passes on the frame as rendered, then the horizontal ones
            // as column passes on its transpose, then back -- as the CUDA path.
            const float hw = mtx::DefocusHalfWidth(defocusPx);
            const auto columns = [&](id<MTLBuffer> x, id<MTLBuffer> y, int w, int h) {
                DefocusArgs A = { w, h, DefocusSegment(hw), 0, hw, 0.0f, 0.0f, 0.0f };
                const int segs = (h + A.segment - 1) / A.segment;
                id<MTLBuffer> ping[3][2] = { { x, y }, { y, x }, { x, y } };
                for (auto& io : ping)
                {
                    [enc setBuffer:io[0] offset:0 atIndex:0];
                    [enc setBuffer:io[1] offset:0 atIndex:1];
                    [enc setBytes:&A length:sizeof(A) atIndex:2];
                    Dispatch(enc, state->column, w, segs, 128, 1);
                }
            };
            const auto transpose = [&](id<MTLBuffer> in, id<MTLBuffer> out, int w, int h) {
                DefocusArgs A = { w, h, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f };
                [enc setComputePipelineState:state->transpose];
                [enc setBuffer:in  offset:0 atIndex:0];
                [enc setBuffer:out offset:0 atIndex:1];
                [enc setBytes:&A length:sizeof(A) atIndex:2];
                [enc dispatchThreadgroups:MTLSizeMake((w + 31) / 32, (h + 31) / 32, 1)
                    threadsPerThreadgroup:MTLSizeMake(32, 8, 1)];
            };

            columns(a, b, pw, ph);     // -> b  (pw x ph)
            transpose(b, a, pw, ph);   // -> a  (ph x pw)
            columns(a, b, ph, pw);     // -> b  (ph x pw)
            transpose(b, a, ph, pw);   // -> a  (pw x ph)

            [enc setBuffer:a   offset:0 atIndex:0];
            [enc setBuffer:dst offset:0 atIndex:1];
            [enc setBytes:&P length:sizeof(P) atIndex:2];
            Dispatch(enc, state->store, dstWidth, dstHeight, 16, 16);

            DeviceState* s = state;
            [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
                (void)done;
                ReleaseScratch(*s, a);
                ReleaseScratch(*s, b);
            }];
            blurred = true;
        }
        else
        {
            // No scratch memory: render sharp rather than not at all.
            if (a) ReleaseScratch(*state, a);
        }
    }

    if (!blurred)
    {
        [enc setBuffer:src offset:0 atIndex:0];
        [enc setBuffer:dst offset:0 atIndex:1];
        [enc setBytes:&P length:sizeof(P) atIndex:2];
        Dispatch(enc, state->transform, dstWidth, dstHeight, 16, 16);
    }

    [enc endEncoding];
    [cb commit];
    *outCb = cb;
    return nullptr;
}

} // namespace

const char* RunMultiTransformMetal(void* pCmdQueue,
                                   const void* src, int srcWidth, int srcHeight,
                                   int srcRowBytes, int srcDepth,
                                   void* dst, int dstWidth, int dstHeight,
                                   int dstRowBytes, int dstDepth,
                                   const mtx::SampleTransforms& st,
                                   int filterMode, int edgeMode,
                                   int srcOriginX, int srcOriginY,
                                   float defocusPx)
{
    @autoreleasepool {
        // The host owns these objects; bridging without a transfer leaves its
        // ownership untouched.
        id<MTLCommandQueue> queue  = (__bridge id<MTLCommandQueue>)pCmdQueue;
        id<MTLBuffer>       srcBuf = (__bridge id<MTLBuffer>)const_cast<void*>(src);
        id<MTLBuffer>       dstBuf = (__bridge id<MTLBuffer>)dst;

        // Not waited on: the host schedules its own later work on this queue,
        // which Metal orders after ours, just as the CUDA path leaves
        // synchronisation of the host's stream to the host.
        id<MTLCommandBuffer> cb = nil;
        return Encode(queue, srcBuf, srcWidth, srcHeight, srcRowBytes, srcDepth,
                      dstBuf, dstWidth, dstHeight, dstRowBytes, dstDepth,
                      st, filterMode, edgeMode, srcOriginX, srcOriginY, defocusPx, &cb);
    }
}

void RunMultiTransformMetalSync(const void* hostSrc, int srcWidth, int srcHeight,
                                void* hostDst, int dstWidth, int dstHeight,
                                const mtx::SampleTransforms& st,
                                int filterMode, int edgeMode,
                                int srcOriginX, int srcOriginY,
                                int srcDepth, int dstDepth,
                                float defocusPx)
{
    @autoreleasepool {
        static id<MTLDevice>       device = MTLCreateSystemDefaultDevice();
        static id<MTLCommandQueue> queue  = [device newCommandQueue];
        if (!device || !queue) return;

        const int srcRowBytes = srcWidth * 4 * mtx::BytesPerChannel(srcDepth);
        const int dstRowBytes = dstWidth * 4 * mtx::BytesPerChannel(dstDepth);
        const NSUInteger srcBytes = static_cast<NSUInteger>(srcRowBytes) * srcHeight;
        const NSUInteger dstBytes = static_cast<NSUInteger>(dstRowBytes) * dstHeight;

        // An empty source is a legitimate input -- it is how the empty-image
        // path is exercised -- so no buffer is made for it.
        id<MTLBuffer> srcBuf = (srcBytes > 0 && hostSrc)
            ? [device newBufferWithBytes:hostSrc length:srcBytes options:MTLResourceStorageModeShared]
            : nil;
        id<MTLBuffer> dstBuf = [device newBufferWithLength:dstBytes options:MTLResourceStorageModeShared];
        if (!dstBuf) return;

        id<MTLCommandBuffer> cb = nil;
        const char* err = Encode(queue, srcBuf, srcWidth, srcHeight, srcRowBytes, srcDepth,
                                 dstBuf, dstWidth, dstHeight, dstRowBytes, dstDepth,
                                 st, filterMode, edgeMode, srcOriginX, srcOriginY, defocusPx, &cb);
        if (err) { std::fprintf(stderr, "Metal: %s\n", err); return; }
        if (cb) [cb waitUntilCompleted];

        std::memcpy(hostDst, dstBuf.contents, dstBytes);
    }
}
