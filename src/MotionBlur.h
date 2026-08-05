#pragma once

// Motion blur.
//
// The blur is analytic, not temporal: every sample is a *different transform of
// the same source frame*, so no neighbouring frames are fetched and there is no
// dependence on the host's temporal clip access. This is only possible because
// the plugin owns its animation and can therefore evaluate the transform at any
// fractional time -- the payoff for not relying on host keyframes.

#include "AnimEngine.h"

namespace mtx {

/** Ceiling on shutter samples. Also bounds the size of SampleTransforms, which
 *  is passed to the CUDA kernel by value. */
constexpr int kMaxBlurSamples = 64;

struct BlurParams
{
    bool  enabled;
    float shutterAngle;   ///< degrees; 180 is the film convention, 360 blurs a whole frame
    float shutterPhase;   ///< degrees; 0 centres the shutter on the frame
    int   samples;        ///< upper bound when adaptive, exact count otherwise
    bool  adaptive;       ///< scale sample count with how far the image actually moves
    float pixelsPerSample;///< adaptive density: larger is cheaper and coarser

    MTX_HD static BlurParams Default()
    {
        BlurParams b;
        b.enabled         = false;
        b.shutterAngle    = 180.0f;
        b.shutterPhase    = 0.0f;
        b.samples         = 16;
        b.adaptive        = true;
        b.pixelsPerSample = 2.0f;
        return b;
    }
};

/** @brief Time of shutter sample @p k of @p n, in frames.
 *
 * Samples sit at the midpoint of each sub-interval rather than at the interval
 * edges. Edge sampling would place two samples at the shutter's extremes and
 * effectively double-weight them, which shows up as faint hard lines at the
 * head and tail of a fast move.
 */
MTX_HD inline float BlurSampleTime(float t, const BlurParams& b, int k, int n)
{
    if (n <= 1) return t;

    const float shutterFrames = b.shutterAngle / 360.0f;
    const float phaseFrames   = b.shutterPhase / 360.0f;

    return t + phaseFrames + shutterFrames * ((static_cast<float>(k) + 0.5f)
                                              / static_cast<float>(n) - 0.5f);
}

/** @brief The inverse transforms for every shutter sample.
 *
 * Precomputed once per render rather than per pixel: the matrices are identical
 * for every pixel, and recomputing the easing solver millions of times would
 * dominate the cost of the blur.
 */
struct SampleTransforms
{
    int   count;
    Mat3  inv[kMaxBlurSamples];
    /// Opacity per shutter sample. Held alongside the matrices because a fade
    /// that happens mid-shutter must blur along with the movement, rather than
    /// snapping to whatever the value is at frame centre.
    float opacity[kMaxBlurSamples];
};

/** @brief Greatest on-screen movement of any image corner across the shutter,
 *  in pixels. This is the measure of "how much blur is actually happening". */
inline float MaxShutterDisplacement(const AnimParams& a, const BlurParams& b,
                                    float t, float width, float height)
{
    const Mat3 mA = EvaluateTransform(a, BlurSampleTime(t, b, 0, 2), width, height);
    const Mat3 mB = EvaluateTransform(a, BlurSampleTime(t, b, 1, 2), width, height);

    const float cx[4] = { 0.0f, width, 0.0f,   width  };
    const float cy[4] = { 0.0f, 0.0f,  height, height };

    float maxDisp = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        float ax, ay, bx, by;
        mA.Apply(cx[i], cy[i], ax, ay);
        mB.Apply(cx[i], cy[i], bx, by);
        const float dx = bx - ax;
        const float dy = by - ay;
        const float d  = sqrtf(dx * dx + dy * dy);
        if (d > maxDisp) maxDisp = d;
    }
    return maxDisp;
}

/** @brief Whether opacity changes measurably across the shutter.
 *
 * A fade with no movement still needs multiple samples, so displacement alone
 * is not enough to decide that a frame is static.
 */
inline bool OpacityMovesAcrossShutter(const AnimParams& a, const BlurParams& b, float t)
{
    const float oA = EvaluateOpacity(a, BlurSampleTime(t, b, 0, 2));
    const float oB = EvaluateOpacity(a, BlurSampleTime(t, b, 1, 2));
    return fabsf(oB - oA) > 1e-4f;
}

/** @brief Sample count scaled to how far the image actually moves.
 *
 * A static frame needs one sample no matter what the slider says; a whip pan
 * needs many. Allocating roughly one sample per @p pixelsPerSample of corner
 * travel keeps quality constant instead of paying a fixed worst-case cost on
 * every frame.
 */
inline int AdaptiveSampleCount(const AnimParams& a, const BlurParams& b,
                               float t, float width, float height)
{
    const int maxSamples = b.samples < kMaxBlurSamples ? b.samples : kMaxBlurSamples;
    if (maxSamples <= 1) return 1;

    const float maxDisp = MaxShutterDisplacement(a, b, t, width, height);

    const float perSample = b.pixelsPerSample > 0.25f ? b.pixelsPerSample : 0.25f;
    int n = static_cast<int>(maxDisp / perSample) + 1;
    if (n < 1)          n = 1;
    if (n > maxSamples) n = maxSamples;
    return n;
}

/** @brief Build the per-sample inverse transforms for one render. */
inline SampleTransforms BuildSampleTransforms(const AnimParams& a, const BlurParams& b,
                                              float t, float width, float height)
{
    SampleTransforms st;

    // Disabled, or a closed shutter, must collapse to exactly the un-blurred
    // path -- not "one sample that happens to be close".
    bool blurring = b.enabled && b.shutterAngle > 1e-4f && b.samples > 1;

    // Nothing moving means every shutter sample would be the same image, so
    // averaging them is an expensive way to compute the frame unchanged. This
    // applies even with adaptive sampling turned off: a fixed 64-sample setting
    // otherwise costs ~17x on a frame that is standing still, which is the
    // dominant waste when several layers each carry this effect.
    //
    // Opacity is checked too, because a fade with no movement genuinely does
    // vary across the shutter and must not be collapsed.
    if (blurring &&
        MaxShutterDisplacement(a, b, t, width, height) < 0.01f &&
        !OpacityMovesAcrossShutter(a, b, t))
    {
        blurring = false;
    }

    st.count = blurring ? (b.adaptive ? AdaptiveSampleCount(a, b, t, width, height)
                                      : (b.samples < kMaxBlurSamples ? b.samples
                                                                     : kMaxBlurSamples))
                        : 1;
    if (st.count < 1) st.count = 1;

    for (int k = 0; k < st.count; ++k)
    {
        const float tk = BlurSampleTime(t, b, k, st.count);
        st.inv[k]     = Invert(EvaluateTransform(a, tk, width, height));
        st.opacity[k] = EvaluateOpacity(a, tk);
    }
    return st;
}

} // namespace mtx
