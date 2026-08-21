#pragma once

// Image sampling, shared between the CPU reference renderer and the CUDA kernel.
// Device-safe for the same reason as TransformMath.h: one implementation.

#include "TransformMath.h"

#include <cstring>

namespace mtx {

enum EdgeMode
{
    kEdgeBlack  = 0,   ///< outside the source is transparent black
    kEdgeClamp  = 1,   ///< outside repeats the border pixel
    kEdgeMirror = 2    ///< outside mirrors back into the image
};

// --- Pixel depths ------------------------------------------------------------
//
// The maths is float throughout; depth only exists at the edges, where a texel
// is loaded and where a result is stored. Supporting more than float matters
// for one reason: the host converts to whatever the plugin declares, and a
// plugin that declares float alone turns an 8-bit Fusion comp into 32-bit from
// that node onward -- four times the memory traffic for a transform that gains
// nothing from it. Declaring the depths the host actually uses keeps the comp
// at its own depth.
//
// Plain ints rather than an enum class, because the values travel into the
// CUDA kernel as arguments and the switch has to be identical on both sides.

enum PixelDepth
{
    kDepthByte  = 0,   ///< 8-bit unsigned, 0..255 == 0..1
    kDepthShort = 1,   ///< 16-bit unsigned, 0..65535 == 0..1
    kDepthHalf  = 2,   ///< IEEE 754 binary16
    kDepthFloat = 3    ///< IEEE 754 binary32
};

MTX_HD inline int BytesPerChannel(int depth)
{
    return depth == kDepthByte ? 1 : (depth == kDepthFloat ? 4 : 2);
}

// Bit-level half <-> float, written once and compiled for both targets.
//
// Deliberately not cuda_fp16's intrinsics: those exist only on the device, and
// the CPU reference path has to produce the *same bits* or the parity test
// could never tell a rounding difference from a bug. Round-to-nearest-even on
// the way down, as every hardware implementation does.

MTX_HD inline float BitsToFloat(unsigned int u)
{
#if defined(__CUDA_ARCH__)
    return __uint_as_float(u);
#else
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
#endif
}

MTX_HD inline unsigned int FloatToBits(float f)
{
#if defined(__CUDA_ARCH__)
    return __float_as_uint(f);
#else
    unsigned int u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
#endif
}

MTX_HD inline float HalfToFloat(unsigned short h)
{
    const unsigned int sign = (static_cast<unsigned int>(h) & 0x8000u) << 16;
    unsigned int       exp  = (h >> 10) & 0x1Fu;
    unsigned int       mant =  h        & 0x3FFu;

    if (exp == 0)
    {
        if (mant == 0) return BitsToFloat(sign);          // signed zero

        // Subnormal: renormalise so the leading bit is implicit again.
        exp = 1;
        while ((mant & 0x400u) == 0) { mant <<= 1; --exp; }
        mant &= 0x3FFu;
        return BitsToFloat(sign | ((exp + 112u) << 23) | (mant << 13));
    }
    if (exp == 31) return BitsToFloat(sign | 0x7F800000u | (mant << 13));   // inf / nan

    return BitsToFloat(sign | ((exp + 112u) << 23) | (mant << 13));
}

MTX_HD inline unsigned short FloatToHalf(float f)
{
    const unsigned int u    = FloatToBits(f);
    const unsigned int sign = (u >> 16) & 0x8000u;
    const unsigned int absu = u & 0x7FFFFFFFu;

    if (absu > 0x7F800000u) return static_cast<unsigned short>(sign | 0x7E00u);   // nan
    if (absu >= 0x7F800000u) return static_cast<unsigned short>(sign | 0x7C00u);  // inf

    const int          exp  = static_cast<int>((absu >> 23) & 0xFFu) - 127 + 15;
    unsigned int       mant = absu & 0x7FFFFFu;

    if (exp >= 31) return static_cast<unsigned short>(sign | 0x7C00u);   // overflow -> inf

    if (exp <= 0)
    {
        // Too small even for a subnormal half.
        if (exp < -10) return static_cast<unsigned short>(sign);

        // Subnormal half: shift the (now explicit) leading bit into place and
        // round to nearest even on what falls off.
        mant |= 0x800000u;
        const unsigned int shift = static_cast<unsigned int>(14 - exp);
        unsigned int       hm    = mant >> shift;
        const unsigned int rem   = mant & ((1u << shift) - 1u);
        const unsigned int half  = 1u << (shift - 1);
        if (rem > half || (rem == half && (hm & 1u))) ++hm;
        return static_cast<unsigned short>(sign | hm);
    }

    unsigned int       hv  = sign | (static_cast<unsigned int>(exp) << 10) | (mant >> 13);
    const unsigned int rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (hv & 1u))) ++hv;   // a carry here rolls into inf, correctly
    return static_cast<unsigned short>(hv);
}

/** @brief One channel at @p p, as float in the depth's natural 0..1 scale.
 *
 *  Integer depths divide rather than multiply by a reciprocal. The difference
 *  is at the ends: 65535 * float(1/65535) is not exactly 1.0, and the maximum
 *  code has to load as exactly 1.0 -- it is what "fully opaque" means. A
 *  correctly rounded division gives that on the CPU and on CUDA alike (nvcc's
 *  default float division is IEEE), so the two paths also stay bit-identical. */
MTX_HD inline float LoadChannel(const unsigned char* p, int depth)
{
    switch (depth)
    {
        case kDepthByte:  return static_cast<float>(*p) / 255.0f;
        case kDepthShort: return static_cast<float>(*reinterpret_cast<const unsigned short*>(p))
                               / 65535.0f;
        case kDepthHalf:  return HalfToFloat(*reinterpret_cast<const unsigned short*>(p));
        default:          return *reinterpret_cast<const float*>(p);
    }
}

/** @brief Store @p v into one channel at @p p. Integer depths clamp to 0..1 and
 *  round to nearest; float depths keep whatever they are given, overshoot and
 *  all, exactly as the float-only path always did. */
MTX_HD inline void StoreChannel(unsigned char* p, int depth, float v)
{
    switch (depth)
    {
        case kDepthByte:
            *p = static_cast<unsigned char>(Clamp01(v) * 255.0f + 0.5f);
            break;
        case kDepthShort:
            *reinterpret_cast<unsigned short*>(p) =
                static_cast<unsigned short>(Clamp01(v) * 65535.0f + 0.5f);
            break;
        case kDepthHalf:
            *reinterpret_cast<unsigned short*>(p) = FloatToHalf(v);
            break;
        default:
            *reinterpret_cast<float*>(p) = v;
            break;
    }
}

/** @brief Store four channels starting at @p p. */
MTX_HD inline void StorePixel(unsigned char* p, int depth, const float* v)
{
    const int b = BytesPerChannel(depth);
    StoreChannel(p,         depth, v[0]);
    StoreChannel(p + b,     depth, v[1]);
    StoreChannel(p + 2 * b, depth, v[2]);
    StoreChannel(p + 3 * b, depth, v[3]);
}

/** @brief A non-owning view of an RGBA image at any supported depth. */
struct ImageView
{
    const unsigned char* data;
    int   width;
    int   height;
    int   rowStrideBytes;    ///< bytes per row: the host's pitch, not width * pixel size
    int   depth = kDepthFloat;   ///< a PixelDepth

    /// Where this image's pixel (0,0) sits in the *frame* the transform is
    /// evaluated in, i.e. the source bounds' origin minus the destination's.
    /// Zero whenever the two images share bounds, which is always the case on
    /// Resolve's Edit page. Fusion crops a node's input to its domain of
    /// definition, so there a source can be smaller than the frame and offset
    /// within it -- and sampling it at frame coordinates without this shift
    /// would put the element in the wrong place. Last in the struct so that the
    /// older four-field aggregate initialisers still compile with zeros here.
    int   originX = 0;
    int   originY = 0;

    /// An image with no pixels to read. Fusion produces these: a node whose
    /// element has no domain of definition at the current frame -- outside its
    /// range while scrubbing, or simply empty -- arrives as zero-sized, and can
    /// arrive with a null data pointer as well.
    MTX_HD bool Empty() const { return data == nullptr || width <= 0 || height <= 0; }

    MTX_HD const unsigned char* Pixel(int x, int y) const
    {
        return data + static_cast<size_t>(y) * rowStrideBytes
                    + static_cast<size_t>(x) * 4 * BytesPerChannel(depth);
    }

    /// Four channels at (x,y), converted to float. The one place depth is read.
    MTX_HD void Load(int x, int y, float* out) const
    {
        const unsigned char* p = Pixel(x, y);
        const int b = BytesPerChannel(depth);
        out[0] = LoadChannel(p,         depth);
        out[1] = LoadChannel(p + b,     depth);
        out[2] = LoadChannel(p + 2 * b, depth);
        out[3] = LoadChannel(p + 3 * b, depth);
    }
};

MTX_HD inline int WrapCoord(int v, int n, EdgeMode mode, bool& outside)
{
    if (v >= 0 && v < n) return v;

    outside = true;
    if (mode == kEdgeClamp)
    {
        return v < 0 ? 0 : n - 1;
    }
    if (mode == kEdgeMirror && n > 1)
    {
        const int period = 2 * n - 2;
        int m = v % period;
        if (m < 0) m += period;
        return m < n ? m : period - m;
    }
    return v < 0 ? 0 : n - 1;   // kEdgeBlack: caller discards via `outside`
}

/** @brief Fetch one texel, honouring the edge mode.
 *  Writes 4 channels into @p out. */
MTX_HD inline void FetchTexel(const ImageView& img, int x, int y, EdgeMode mode, float* out)
{
    // Nothing to read from means transparent, whatever the edge mode says.
    //
    // This guard is load-bearing, not defensive decoration. With a zero-sized
    // image WrapCoord has no valid texel to clamp or mirror to: Clamp returns
    // n - 1 = -1, Mirror falls through to the same, and either then reads
    // before the buffer -- or through a null pointer, which is what an empty
    // Fusion image can carry. Only Black happened to bail out first. That was
    // an invalid read on the CPU path and a device fault on CUDA, and Fusion
    // hands out exactly such images while scrubbing a composition whose nodes
    // have no domain of definition at the current frame.
    if (img.Empty())
    {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }

    bool outside = false;
    const int cx = WrapCoord(x, img.width,  mode, outside);
    const int cy = WrapCoord(y, img.height, mode, outside);

    if (outside && mode == kEdgeBlack)
    {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }

    img.Load(cx, cy, out);
}

/** @brief Bilinear sample at continuous coordinates.
 *
 * @param sx,sy pixel-centre coordinates: the centre of texel (0,0) is (0.5,0.5),
 *              matching how the transform maps destination pixel centres.
 */
/** @brief Bound a sampling coordinate before it is turned into an integer.
 *
 * Converting a float outside int range to int is undefined behaviour on the
 * CPU (x86 yields 0x80000000) and saturates on CUDA, and either way x0 + 1 can
 * then overflow. A coordinate this far out can only come from a near-singular
 * transform, and every edge mode treats anything beyond a few image widths
 * identically, so clamping to +-2^24 -- the last float with integer spacing --
 * changes no pixel that could be seen while removing the UB entirely. NaN
 * compares false with everything and so falls through both tests; it is sent
 * to the far side, where every edge mode returns a defined value.
 */
MTX_HD inline float BoundCoord(float v)
{
    const float kLimit = 16777216.0f;
    if (v >  kLimit) return  kLimit;
    if (v < -kLimit) return -kLimit;
    return v == v ? v : kLimit;   // NaN -> far outside, never into the image
}

MTX_HD inline void SampleBilinear(const ImageView& img, float sx, float sy,
                                  EdgeMode mode, float* out)
{
    const float fx = BoundCoord(sx - 0.5f);
    const float fy = BoundCoord(sy - 0.5f);

    const int x0 = static_cast<int>(floorf(fx));
    const int y0 = static_cast<int>(floorf(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);

    float p00[4], p10[4], p01[4], p11[4];
    FetchTexel(img, x0,     y0,     mode, p00);
    FetchTexel(img, x0 + 1, y0,     mode, p10);
    FetchTexel(img, x0,     y0 + 1, mode, p01);
    FetchTexel(img, x0 + 1, y0 + 1, mode, p11);

    for (int c = 0; c < 4; ++c)
    {
        const float top    = Lerp(p00[c], p10[c], tx);
        const float bottom = Lerp(p01[c], p11[c], tx);
        out[c] = Lerp(top, bottom, ty);
    }
}

/** @brief Nearest-neighbour sample, for when crisp edges matter more than smoothness. */
MTX_HD inline void SampleNearest(const ImageView& img, float sx, float sy,
                                 EdgeMode mode, float* out)
{
    FetchTexel(img, static_cast<int>(floorf(BoundCoord(sx))),
                    static_cast<int>(floorf(BoundCoord(sy))), mode, out);
}

enum FilterMode
{
    kFilterNearest  = 0,
    kFilterBilinear = 1
};

MTX_HD inline void SampleImage(const ImageView& img, float sx, float sy,
                               FilterMode filter, EdgeMode edge, float* out)
{
    // Frame coordinates in, image coordinates out: a source that sits offset
    // inside the frame is sampled where it actually is. See ImageView::originX.
    const float ix = sx - static_cast<float>(img.originX);
    const float iy = sy - static_cast<float>(img.originY);

    if (filter == kFilterNearest) SampleNearest(img, ix, iy, edge, out);
    else                          SampleBilinear(img, ix, iy, edge, out);
}

} // namespace mtx

#include "MotionBlur.h"

namespace mtx {

/** @brief Render one destination pixel: sample the source once per shutter
 *  sample and average.
 *
 * Shared by the CPU and CUDA paths so the accumulation, the rounding and the
 * edge handling are identical rather than merely similar.
 *
 * @param dx,dy destination pixel *centre* in destination-image space. Passing
 *              the corner instead causes a half-pixel drift, invisible at 1x
 *              but obvious once the image is scaled up.
 */
MTX_HD inline void RenderPixel(const ImageView& src, const SampleTransforms& st,
                               float dx, float dy,
                               FilterMode filter, EdgeMode edge, float* out)
{
    // Opacity scales all four channels, not just alpha: Resolve hands plugins
    // premultiplied RGBA, where fading means scaling the premultiplied colour
    // as well as the alpha. Touching alpha alone would leave the colour too
    // bright and the fade would look wrong over anything but black.
    if (st.hasGhost)
    {
        // The drag preview *replaces* the frame rather than sitting over it.
        //
        // Compositing it over the real render meant the picture underneath was
        // whatever frame the playhead happened to be parked on -- a second,
        // unrelated copy of the object competing for attention with the one
        // being dragged. Showing only the dragged pose leaves nothing to
        // mistake it for.
        //
        // Tinted towards the gizmo's own colour so it stays obviously a
        // preview and not the finished frame. The multiplier is precomputed by
        // the caller, so this stays one multiply per channel.
        float gx, gy;
        st.ghostInv.Apply(dx, dy, gx, gy);
        SampleImage(src, gx, gy, filter, edge, out);

        const float a = st.ghostOpacity;
        out[0] *= st.ghostTint[0] * a;
        out[1] *= st.ghostTint[1] * a;
        out[2] *= st.ghostTint[2] * a;
        out[3] *= a;
        return;
    }

    if (st.count <= 1)
    {
        float sx, sy;
        st.inv[0].Apply(dx, dy, sx, sy);
        SampleImage(src, sx, sy, filter, edge, out);

        const float o = st.opacity[0];
        out[0] *= o; out[1] *= o; out[2] *= o; out[3] *= o;
    }
    else
    {
        float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for (int k = 0; k < st.count; ++k)
        {
            float sx, sy;
            st.inv[k].Apply(dx, dy, sx, sy);

            float s[4];
            SampleImage(src, sx, sy, filter, edge, s);

            const float o = st.opacity[k];
            acc[0] += s[0] * o; acc[1] += s[1] * o; acc[2] += s[2] * o; acc[3] += s[3] * o;
        }

        const float inv = 1.0f / static_cast<float>(st.count);
        out[0] = acc[0] * inv;
        out[1] = acc[1] * inv;
        out[2] = acc[2] * inv;
        out[3] = acc[3] * inv;
    }

}

} // namespace mtx
