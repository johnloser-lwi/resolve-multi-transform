// Metal compute kernels: the macOS counterpart of CudaKernel.cu.
//
// CUDA compiles Sampler.h itself as device code; Metal's shading language
// cannot include it (no host headers, address-space-qualified pointers), so
// this file is a line-for-line port of the parts the GPU runs:
//
//   Sampler.h   LoadChannel, StoreChannel, WrapCoord, FetchTexel, BoundCoord,
//               SampleBilinear, SampleNearest, SampleImage, RenderPixel
//   Defocus.h   the box pass, as CudaKernel.cu restructures it for a GPU
//               (segmented column passes plus a transpose)
//
// Change Sampler.h or Defocus.h and this file must change with it. The parity
// test (test_render_parity, which on macOS runs every check against this path)
// is what catches the two drifting apart.
//
// Compiled at runtime from this source, embedded into the plugin at build time
// (MetalKernelsSource.h.in), with fast math OFF: integer depths divide by 255 /
// 65535 and the maximum code must load as exactly 1.0, which a reciprocal
// approximation would not give.
//
// This file must never contain the character sequence )MTXMETAL" -- it closes
// the raw string it is embedded in.
//
// UNTESTED ON HARDWARE: written on Windows ahead of a Mac being available.

#include <metal_stdlib>
using namespace metal;

// --- Shared layout with MetalRender.mm --------------------------------------
// Scalars only, all four bytes, so the layout is identical on both sides
// without relying on either compiler's struct rules. Mat3s are padded to 8.

#define MTX_MAX_SAMPLES 64

struct MTXParams
{
    int srcWidth, srcHeight, srcRowBytes, srcDepth;
    int dstWidth, dstHeight, dstRowBytes, dstDepth;
    int srcOriginX, srcOriginY, filterMode, edgeMode;
    int count, hasGhost, haveSrc, reserved0;
    int fillWidth, fillHeight, fillPad, reserved1;   // defocus: padded buffer
    float ghostOpacity, ghostTint0, ghostTint1, ghostTint2;
    float ghostInv[8];
    float inv[MTX_MAX_SAMPLES * 8];
    float opacity[MTX_MAX_SAMPLES];
};

struct DefocusArgs
{
    int   width, height, segment, reserved0;
    float halfWidth, reserved1, reserved2, reserved3;
};

// --- Sampler.h, ported ------------------------------------------------------

constant int kDepthByte  = 0;
constant int kDepthShort = 1;
constant int kDepthHalf  = 2;

constant int kEdgeBlack  = 0;
constant int kEdgeClamp  = 1;
constant int kEdgeMirror = 2;

constant int kFilterNearest = 0;

inline float mtxLerp(float a, float b, float t) { return a + (b - a) * t; }
inline float mtxClamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

inline int bytesPerChannel(int depth)
{
    return depth == kDepthByte ? 1 : (depth == 3 ? 4 : 2);
}

// Half conversions are the hardware's: half -> float is exact either way, and
// float -> half rounds to nearest even, as Sampler.h's software version does.
inline float loadChannel(device const uchar* p, int depth)
{
    switch (depth)
    {
        case kDepthByte:  return float(*p) / 255.0f;
        case kDepthShort: return float(*(device const ushort*)p) / 65535.0f;
        case kDepthHalf:  return float(*(device const half*)p);
        default:          return *(device const float*)p;
    }
}

inline void storeChannel(device uchar* p, int depth, float v)
{
    switch (depth)
    {
        case kDepthByte:  *p = uchar(mtxClamp01(v) * 255.0f + 0.5f); break;
        case kDepthShort: *(device ushort*)p = ushort(mtxClamp01(v) * 65535.0f + 0.5f); break;
        case kDepthHalf:  *(device half*)p = half(v); break;
        default:          *(device float*)p = v; break;
    }
}

inline void storePixel(device uchar* p, int depth, float4 v)
{
    const int b = bytesPerChannel(depth);
    storeChannel(p,         depth, v.x);
    storeChannel(p + b,     depth, v.y);
    storeChannel(p + 2 * b, depth, v.z);
    storeChannel(p + 3 * b, depth, v.w);
}

inline int wrapCoord(int v, int n, int mode, thread bool& outside)
{
    if (v >= 0 && v < n) return v;

    outside = true;
    if (mode == kEdgeClamp) return v < 0 ? 0 : n - 1;
    if (mode == kEdgeMirror && n > 1)
    {
        const int period = 2 * n - 2;
        int m = v % period;
        if (m < 0) m += period;
        return m < n ? m : period - m;
    }
    return v < 0 ? 0 : n - 1;
}

inline float4 fetchTexel(device const uchar* src, constant MTXParams& P, int x, int y)
{
    // Nothing to read from means transparent, whatever the edge mode says --
    // see FetchTexel in Sampler.h for why this guard is load-bearing.
    if (P.haveSrc == 0 || P.srcWidth <= 0 || P.srcHeight <= 0) return float4(0.0f);

    bool outside = false;
    const int cx = wrapCoord(x, P.srcWidth,  P.edgeMode, outside);
    const int cy = wrapCoord(y, P.srcHeight, P.edgeMode, outside);
    if (outside && P.edgeMode == kEdgeBlack) return float4(0.0f);

    const int b = bytesPerChannel(P.srcDepth);
    device const uchar* p = src + ulong(cy) * ulong(P.srcRowBytes) + ulong(cx) * ulong(4 * b);
    return float4(loadChannel(p,         P.srcDepth),
                  loadChannel(p + b,     P.srcDepth),
                  loadChannel(p + 2 * b, P.srcDepth),
                  loadChannel(p + 3 * b, P.srcDepth));
}

inline float boundCoord(float v)
{
    const float kLimit = 16777216.0f;
    if (v >  kLimit) return  kLimit;
    if (v < -kLimit) return -kLimit;
    return v == v ? v : kLimit;   // NaN -> far outside, never into the image
}

inline float4 sampleImage(device const uchar* src, constant MTXParams& P, float sx, float sy)
{
    const float ix = sx - float(P.srcOriginX);
    const float iy = sy - float(P.srcOriginY);

    if (P.filterMode == kFilterNearest)
        return fetchTexel(src, P, int(floor(boundCoord(ix))), int(floor(boundCoord(iy))));

    const float fx = boundCoord(ix - 0.5f);
    const float fy = boundCoord(iy - 0.5f);
    const int x0 = int(floor(fx));
    const int y0 = int(floor(fy));
    const float tx = fx - float(x0);
    const float ty = fy - float(y0);

    const float4 p00 = fetchTexel(src, P, x0,     y0);
    const float4 p10 = fetchTexel(src, P, x0 + 1, y0);
    const float4 p01 = fetchTexel(src, P, x0,     y0 + 1);
    const float4 p11 = fetchTexel(src, P, x0 + 1, y0 + 1);

    float4 out;
    for (int c = 0; c < 4; ++c)
    {
        const float top    = mtxLerp(p00[c], p10[c], tx);
        const float bottom = mtxLerp(p01[c], p11[c], tx);
        out[c] = mtxLerp(top, bottom, ty);
    }
    return out;
}

inline float2 applyMat(constant float* m, float x, float y)
{
    return float2(m[0] * x + m[1] * y + m[2],
                  m[3] * x + m[4] * y + m[5]);
}

inline float4 renderPixel(device const uchar* src, constant MTXParams& P, float dx, float dy)
{
    if (P.hasGhost != 0)
    {
        const float2 g = applyMat(P.ghostInv, dx, dy);
        float4 o = sampleImage(src, P, g.x, g.y);
        const float a = P.ghostOpacity;
        o.x *= P.ghostTint0 * a;
        o.y *= P.ghostTint1 * a;
        o.z *= P.ghostTint2 * a;
        o.w *= a;
        return o;
    }

    if (P.count <= 1)
    {
        const float2 s = applyMat(P.inv, dx, dy);
        float4 o = sampleImage(src, P, s.x, s.y);
        const float op = P.opacity[0];
        o.x *= op; o.y *= op; o.z *= op; o.w *= op;
        return o;
    }

    float4 acc = float4(0.0f);
    for (int k = 0; k < P.count; ++k)
    {
        const float2 s = applyMat(P.inv + k * 8, dx, dy);
        const float4 c = sampleImage(src, P, s.x, s.y);
        const float op = P.opacity[k];
        acc.x += c.x * op; acc.y += c.y * op; acc.z += c.z * op; acc.w += c.w * op;
    }
    const float inv = 1.0f / float(P.count);
    return float4(acc.x * inv, acc.y * inv, acc.z * inv, acc.w * inv);
}

// --- The transform ----------------------------------------------------------

kernel void mtx_transform(device const uchar*  src [[buffer(0)]],
                          device uchar*        dst [[buffer(1)]],
                          constant MTXParams&  P   [[buffer(2)]],
                          uint2 gid [[thread_position_in_grid]])
{
    const int x = int(gid.x), y = int(gid.y);
    if (x >= P.dstWidth || y >= P.dstHeight) return;

    const float4 c = renderPixel(src, P, float(x) + 0.5f, float(y) + 0.5f);
    device uchar* p = dst + ulong(y) * ulong(P.dstRowBytes)
                          + ulong(x) * ulong(4 * bytesPerChannel(P.dstDepth));
    storePixel(p, P.dstDepth, c);
}

// --- Blur ("defocus") -------------------------------------------------------

/** The transform into the padded float buffer, as DefocusFillKernel. */
kernel void mtx_defocus_fill(device const uchar*  src     [[buffer(0)]],
                             device float4*       scratch [[buffer(1)]],
                             constant MTXParams&  P       [[buffer(2)]],
                             uint2 gid [[thread_position_in_grid]])
{
    const int px = int(gid.x), py = int(gid.y);
    if (px >= P.fillWidth || py >= P.fillHeight) return;

    scratch[ulong(py) * ulong(P.fillWidth) + ulong(px)] =
        renderPixel(src, P, float(px - P.fillPad) + 0.5f, float(py - P.fillPad) + 0.5f);
}

/** One box pass down every column, one thread per (column, segment) -- the
 *  same window, edge rule and running sum as DefocusColumnKernel. */
kernel void mtx_defocus_column(device const float4* in  [[buffer(0)]],
                               device float4*       out [[buffer(1)]],
                               constant DefocusArgs& A  [[buffer(2)]],
                               uint2 gid [[thread_position_in_grid]])
{
    const int w = A.width, h = A.height;
    const int x  = int(gid.x);
    const int y0 = int(gid.y) * A.segment;
    if (x >= w || y0 >= h) return;
    const int y1 = min(h, y0 + A.segment);

    const int   r   = int(A.halfWidth);
    const float f   = A.halfWidth - float(r);
    const float inv = 1.0f / (2.0f * float(r) + 1.0f + 2.0f * f);

    float4 sum = float4(0.0f);
    for (int k = max(0, y0 - r); k <= min(h - 1, y0 + r); ++k)
        sum += in[ulong(k) * ulong(w) + ulong(x)];

    for (int i = y0; i < y1; ++i)
    {
        float4 edge = float4(0.0f);
        const int lo = i - r - 1, hi = i + r + 1;
        if (lo >= 0) edge += in[ulong(lo) * ulong(w) + ulong(x)];
        if (hi < h)  edge += in[ulong(hi) * ulong(w) + ulong(x)];

        out[ulong(i) * ulong(w) + ulong(x)] = (sum + f * edge) * inv;

        if (hi < h) sum += in[ulong(hi) * ulong(w) + ulong(x)];
        const int drop = i - r;
        if (drop >= 0) sum -= in[ulong(drop) * ulong(w) + ulong(x)];
    }
}

/** Tiled transpose through threadgroup memory: in is w x h, out is h x w.
 *  Threadgroup must be 32 x 8, as TransposeKernel's block. */
kernel void mtx_transpose(device const float4* in  [[buffer(0)]],
                          device float4*       out [[buffer(1)]],
                          constant DefocusArgs& A  [[buffer(2)]],
                          uint2 tid [[thread_position_in_threadgroup]],
                          uint2 tg  [[threadgroup_position_in_grid]])
{
    threadgroup float4 tile[32][33];   // +1 column: no bank conflicts on the read-back

    const int w = A.width, h = A.height;
    int x = int(tg.x) * 32 + int(tid.x);
    int y = int(tg.y) * 32 + int(tid.y);
    for (int j = 0; j < 32; j += 8)
        if (x < w && y + j < h) tile[tid.y + j][tid.x] = in[ulong(y + j) * ulong(w) + ulong(x)];

    // Every thread reaches this, in or out of bounds: a barrier some threads
    // skip is undefined.
    threadgroup_barrier(mem_flags::mem_threadgroup);

    x = int(tg.y) * 32 + int(tid.x);
    y = int(tg.x) * 32 + int(tid.y);
    for (int j = 0; j < 32; j += 8)
        if (x < h && y + j < w) out[ulong(y + j) * ulong(h) + ulong(x)] = tile[tid.x][tid.y + j];
}

/** The padded buffer's interior, stored at the destination's depth. */
kernel void mtx_defocus_store(device const float4* scratch [[buffer(0)]],
                              device uchar*        dst     [[buffer(1)]],
                              constant MTXParams&  P       [[buffer(2)]],
                              uint2 gid [[thread_position_in_grid]])
{
    const int x = int(gid.x), y = int(gid.y);
    if (x >= P.dstWidth || y >= P.dstHeight) return;

    const float4 v = scratch[ulong(y + P.fillPad) * ulong(P.fillWidth) + ulong(x + P.fillPad)];
    device uchar* p = dst + ulong(y) * ulong(P.dstRowBytes)
                          + ulong(x) * ulong(4 * bytesPerChannel(P.dstDepth));
    storePixel(p, P.dstDepth, v);
}
