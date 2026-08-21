#pragma once

// Image sampling, shared between the CPU reference renderer and the CUDA kernel.
// Device-safe for the same reason as TransformMath.h: one implementation.

#include "TransformMath.h"

namespace mtx {

enum EdgeMode
{
    kEdgeBlack  = 0,   ///< outside the source is transparent black
    kEdgeClamp  = 1,   ///< outside repeats the border pixel
    kEdgeMirror = 2    ///< outside mirrors back into the image
};

/** @brief A non-owning view of an RGBA float image. */
struct ImageView
{
    const float* data;
    int   width;
    int   height;
    int   rowStrideFloats;   ///< floats per row, not bytes and not always width*4

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

    MTX_HD const float* Pixel(int x, int y) const
    {
        return data + static_cast<size_t>(y) * rowStrideFloats + static_cast<size_t>(x) * 4;
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

    const float* p = img.Pixel(cx, cy);
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
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
