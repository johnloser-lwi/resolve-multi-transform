#pragma once

// Baking the plugin's animation into keyframes for a native Fusion Transform.
//
// Why this exists: Fusion concatenates adjacent Transform nodes and resamples
// the source once, so a move expressed as a native Transform stays sharp
// through downstream scaling in a way a single fixed resample cannot. Handing
// the animation over as real Transform keyframes buys that, and lets it be
// tweaked in Fusion's own tools afterwards.
//
// This half is pure: it turns an AnimParams into decimated per-channel
// keyframes and knows nothing about Fusion's file format. The format emitter is
// a separate, replaceable layer, so the part that has to be exactly right --
// the decomposition and the keyframe reduction -- is testable without a host
// or a paste into Fusion.
//
// ## What bakes cleanly, and what does not
//
// The pose is sampled with FlattenTransform, which expresses the whole
// animation about the image centre as scale, rotation, position and opacity.
// A single Transform node holds exactly those. It cannot hold **shear**, which
// FlattenTransform already drops and reports: shear appears when a rotation is
// combined with a non-uniform scale, which is what an orthographic tilt or
// swivel is. So a plain move -- scale, position, rotation, uniform scale, a
// fade -- bakes exactly (to sampling precision); tilt and swivel bake as an
// approximation, flagged so the caller can say so.
//
// Bounce is not a special case and is never dropped: a bouncy channel simply
// keeps more keyframes, because the reduction is lossy to a tolerance rather
// than by feature. A smooth ramp fits to two keys; a bounce fits to as many as
// the tolerance needs.

#include <cmath>
#include <vector>

#include "AnimEngine.h"

namespace mtx {

/** @brief One keyframe: a frame number and the channel's value there. */
struct BakedKey
{
    float frame;
    float value;
};

/** @brief One cubic keyframe: value plus a Bezier handle on each side.
 *
 *  Handles are offsets from the key point, in (frame, value): the right handle
 *  of key i and the left handle of key i+1 are the two inner control points of
 *  the cubic Bezier segment between them. An endpoint has a zero handle on its
 *  outer side. This is Fusion's own keyframe model, so the emitter writes it
 *  almost verbatim -- and it is why an eased move collapses to two keys: the
 *  ease *is* a cubic, so one segment holds it exactly. */
struct BezierKey
{
    float frame;
    float value;
    float lhFrame, lhValue;   ///< left handle offset  (lhFrame <= 0)
    float rhFrame, rhValue;   ///< right handle offset (rhFrame >= 0)
};

/** @brief The per-channel error a reduced curve may differ from the samples by.
 *
 *  In each channel's own units. Centre and size are fractions of the frame
 *  (0.001 of width is well under a pixel at any sane resolution), angle is
 *  degrees, opacity is 0..1. Generous enough to keep the keyframe count low,
 *  tight enough that the reduction is invisible. */
struct BakeTolerances
{
    float centre  = 0.0008f;   ///< ~1.5 px at 1920 wide
    float size    = 0.0008f;
    float aspect  = 0.0008f;
    float angle   = 0.05f;     ///< degrees
    float opacity = 1.0f / 512.0f;

    static BakeTolerances Default() { return {}; }
};

/** @brief A baked animation, one decimated key list per Transform channel.
 *
 *  Centre and size are in Fusion's convention: normalised, 0.5,0.5 is the frame
 *  centre; size is a uniform scale multiplier and aspect stretches it (the two
 *  together carry a non-uniform scale). Angle is degrees. Opacity is 0..1, for
 *  whatever node the caller uses to fade -- a Brightness/Contrast gain, a Merge
 *  blend. A single key in a channel means it never animated. */
struct BakedTransform
{
    std::vector<BezierKey> centreX;
    std::vector<BezierKey> centreY;
    std::vector<BezierKey> size;
    std::vector<BezierKey> aspect;
    std::vector<BezierKey> angle;
    std::vector<BezierKey> opacity;

    /// Set when any sampled frame carried shear a Transform cannot hold. The
    /// bake is then an approximation and the caller should warn. maxShear is
    /// the worst offender, relative to the pose's own size, for the message.
    bool  hadShear  = false;
    float maxShear  = 0.0f;

    /// Set when the pose mirrored (an orthographic swivel past 90 degrees, or a
    /// negative scale). A Transform's Size cannot go negative; a faithful bake
    /// of this needs a Flip, which v1 does not emit. Rare in ordinary moves.
    bool  hadMirror = false;

    int frameCount = 0;   ///< how many frames were sampled, for reporting
};

// --- Sign and axis conventions ---------------------------------------------
//
// These map the engine's coordinate choices onto Fusion's, and each is the
// single place its axis is decided so a paste test can correct one line rather
// than hunt through the maths.
//
//  * Position: the engine's posX/posY are fractions of width/height added to
//    the centre, matching Fusion's normalised Centre directly. Fusion's Y runs
//    bottom-up; the engine's does too (OFX canonical), so no flip -- but this
//    is exactly the assumption a paste test confirms, hence the constant.
//  * Angle: Mat3::Rotate is counter-clockwise for positive degrees, as is
//    Fusion's Rotation Angle, so the sign passes straight through.
constexpr float kFusionPosYSign = 1.0f;
constexpr float kFusionAngleSign = 1.0f;

/** @brief Reduce a per-frame sample list to the fewest keyframes whose linear
 *  interpolation stays within @p tol of every sample.
 *
 *  Douglas-Peucker on the (frame, value) polyline, measuring vertical distance
 *  because the frames are monotonic. The endpoints are always kept, so a flat
 *  channel returns its two ends and an empty or single-sample input returns
 *  what it was given. The guarantee is exact for linear-interpolated keys,
 *  which is what the emitter should write; a channel later smoothed in Fusion
 *  drifts by at most the curvature it adds, which for a dense-enough bake is
 *  nothing anyone can see.
 */
inline std::vector<BakedKey> ReduceChannel(const std::vector<float>& samples,
                                           float firstFrame, float tol)
{
    const int n = static_cast<int>(samples.size());
    std::vector<BakedKey> out;
    if (n == 0) return out;
    if (n == 1) { out.push_back({ firstFrame, samples[0] }); return out; }

    std::vector<char> keep(static_cast<size_t>(n), 0);
    keep[0] = 1;
    keep[n - 1] = 1;

    // Iterative Douglas-Peucker: a stack of [lo, hi] spans, so a long bouncy
    // channel cannot overflow the call stack.
    std::vector<std::pair<int, int>> stack;
    stack.push_back({ 0, n - 1 });

    while (!stack.empty())
    {
        const int lo = stack.back().first;
        const int hi = stack.back().second;
        stack.pop_back();
        if (hi <= lo + 1) continue;

        const float v0   = samples[lo];
        const float span = static_cast<float>(hi - lo);
        const float dv   = samples[hi] - v0;

        int   worst = -1;
        float worstDev = tol;
        for (int i = lo + 1; i < hi; ++i)
        {
            const float lin = v0 + dv * (static_cast<float>(i - lo) / span);
            const float dev = std::fabs(samples[i] - lin);
            if (dev > worstDev) { worstDev = dev; worst = i; }
        }

        if (worst >= 0)
        {
            keep[worst] = 1;
            stack.push_back({ lo, worst });
            stack.push_back({ worst, hi });
        }
    }

    for (int i = 0; i < n; ++i)
        if (keep[i]) out.push_back({ firstFrame + static_cast<float>(i), samples[i] });
    return out;
}



/** @brief Evaluate a fitted channel at a frame, as Fusion's spline would.
 *  Exposed so callers and tests measure against the same cubic the emitter
 *  will write, rather than re-deriving it. */
inline float EvalBezierChannel(const std::vector<BezierKey>& k, float frame)
{
    if (k.empty()) return 0.0f;
    if (frame <= k.front().frame) return k.front().value;
    if (frame >= k.back().frame)  return k.back().value;

    for (size_t i = 1; i < k.size(); ++i)
    {
        if (frame > k[i].frame) continue;

        const BezierKey& a = k[i - 1];
        const BezierKey& b = k[i];
        const float span = b.frame - a.frame;
        if (span <= 1e-6f) return b.value;

        // Control points in (frame, value); the segment's time curve is a cubic
        // too, so a frame maps to the parameter u by Newton on the time cubic.
        const float p0f = a.frame,            p0v = a.value;
        const float p1f = a.frame + a.rhFrame, p1v = a.value + a.rhValue;
        const float p2f = b.frame + b.lhFrame, p2v = b.value + b.lhValue;
        const float p3f = b.frame,            p3v = b.value;

        float u = (frame - a.frame) / span;   // good first guess
        for (int it = 0; it < 8; ++it)
        {
            const float mu = 1.0f - u;
            const float ft = mu*mu*mu*p0f + 3*mu*mu*u*p1f + 3*mu*u*u*p2f + u*u*u*p3f;
            const float dft = 3*mu*mu*(p1f-p0f) + 6*mu*u*(p2f-p1f) + 3*u*u*(p3f-p2f);
            if (std::fabs(dft) < 1e-6f) break;
            u -= (ft - frame) / dft;
            if (u < 0.0f) u = 0.0f; if (u > 1.0f) u = 1.0f;
        }
        const float mu = 1.0f - u;
        return mu*mu*mu*p0v + 3*mu*mu*u*p1v + 3*mu*u*u*p2v + u*u*u*p3v;
    }
    return k.back().value;
}

namespace detail {

/** Least-squares fit of the two inner control *values* of a cubic Bezier whose
 *  time control points are evenly spaced (so time is linear in the parameter
 *  and u = (frame - frameLo)/span). Closed form: a 2x2 normal equation. */
inline void FitSegment(const std::vector<float>& s, int lo, int hi,
                       float& c1, float& c2)
{
    const float v0 = s[lo], v3 = s[hi];
    const float span = static_cast<float>(hi - lo);

    // Fewer than two interior samples: place the handles on the straight line,
    // which is what a linear key pair is.
    if (hi - lo < 2)
    {
        c1 = v0 + (v3 - v0) / 3.0f;
        c2 = v0 + (v3 - v0) * 2.0f / 3.0f;
        return;
    }

    double a11 = 0, a12 = 0, a22 = 0, b1 = 0, b2 = 0;
    for (int i = lo + 1; i < hi; ++i)
    {
        const double u  = (i - lo) / span;
        const double mu = 1.0 - u;
        const double B1 = 3.0 * mu * mu * u;   // weight on c1
        const double B2 = 3.0 * mu * u  * u;   // weight on c2
        const double r  = s[i] - (mu*mu*mu*v0 + u*u*u*v3);
        a11 += B1 * B1;  a12 += B1 * B2;  a22 += B2 * B2;
        b1  += B1 * r;   b2  += B2 * r;
    }

    const double det = a11 * a22 - a12 * a12;
    if (std::fabs(det) < 1e-12)
    {
        c1 = v0 + (v3 - v0) / 3.0f;
        c2 = v0 + (v3 - v0) * 2.0f / 3.0f;
        return;
    }
    c1 = static_cast<float>(( b1 * a22 - b2 * a12) / det);
    c2 = static_cast<float>((-b1 * a12 + b2 * a11) / det);
}

} // namespace detail

/** @brief Fit a per-frame sample list with the fewest cubic Bezier keyframes
 *  whose spline stays within @p tol of every sample.
 *
 *  One cubic is fitted across the whole range; if it holds, the channel is two
 *  keys with handles -- which is where an eased move lands, because the ease is
 *  itself a cubic. Otherwise it splits at the worst frame and recurses, so a
 *  bounce becomes as many segments as it needs and nothing is dropped. The
 *  same lossy-to-a-tolerance rule as the linear reducer, but against curves the
 *  channels are actually made of, so the key count is a fraction of it.
 */
inline std::vector<BezierKey> FitChannel(const std::vector<float>& samples,
                                         float firstFrame, float tol)
{
    const int n = static_cast<int>(samples.size());
    std::vector<BezierKey> out;
    if (n == 0) return out;
    if (n == 1)
    {
        out.push_back({ firstFrame, samples[0], 0, 0, 0, 0 });
        return out;
    }

    // Which frames become keys. Endpoints always; the fitter adds the rest.
    std::vector<char> keep(static_cast<size_t>(n), 0);
    keep[0] = 1; keep[n - 1] = 1;

    std::vector<std::pair<int,int>> stack;
    stack.push_back({ 0, n - 1 });
    while (!stack.empty())
    {
        const int lo = stack.back().first, hi = stack.back().second;
        stack.pop_back();
        if (hi <= lo + 1) continue;

        float c1, c2;
        detail::FitSegment(samples, lo, hi, c1, c2);

        const float v0 = samples[lo], v3 = samples[hi];
        const float span = static_cast<float>(hi - lo);

        int   worst = -1;
        float worstDev = tol;
        for (int i = lo + 1; i < hi; ++i)
        {
            const float u  = (i - lo) / span;
            const float mu = 1.0f - u;
            const float fit = mu*mu*mu*v0 + 3*mu*mu*u*c1 + 3*mu*u*u*c2 + u*u*u*v3;
            const float dev = std::fabs(samples[i] - fit);
            if (dev > worstDev) { worstDev = dev; worst = i; }
        }

        if (worst >= 0)
        {
            keep[worst] = 1;
            stack.push_back({ lo, worst });
            stack.push_back({ worst, hi });
        }
    }

    // Gather kept frames, then fit each final segment once more to set the
    // handles the emitter writes. A key's handles come from the two segments
    // touching it; an endpoint has only one.
    std::vector<int> idx;
    for (int i = 0; i < n; ++i) if (keep[i]) idx.push_back(i);

    out.resize(idx.size());
    for (size_t j = 0; j < idx.size(); ++j)
    {
        out[j].frame = firstFrame + static_cast<float>(idx[j]);
        out[j].value = samples[idx[j]];
        out[j].lhFrame = out[j].lhValue = 0.0f;
        out[j].rhFrame = out[j].rhValue = 0.0f;
    }

    for (size_t seg = 0; seg + 1 < idx.size(); ++seg)
    {
        const int lo = idx[seg], hi = idx[seg + 1];
        float c1, c2;
        detail::FitSegment(samples, lo, hi, c1, c2);

        const float third = static_cast<float>(hi - lo) / 3.0f;
        out[seg].rhFrame     =  third;
        out[seg].rhValue     =  c1 - samples[lo];
        out[seg + 1].lhFrame = -third;
        out[seg + 1].lhValue =  c2 - samples[hi];
    }
    return out;
}

/** @brief Sample the animation frame by frame and reduce each channel.
 *
 *  @param startFrame,endFrame inclusive, clip-relative -- the same frame numbers
 *         the stage timing is authored in. Use 0 and AnimationEndTime for the
 *         whole move.
 */
inline BakedTransform BakeToFusion(const AnimParams& a, float width, float height,
                                   int startFrame, int endFrame,
                                   const BakeTolerances& tol = BakeTolerances::Default())
{
    BakedTransform b;
    if (endFrame < startFrame) return b;

    const int n = endFrame - startFrame + 1;
    b.frameCount = n;

    std::vector<float> cx, cy, sz, as, an, op;
    cx.reserve(n); cy.reserve(n); sz.reserve(n);
    as.reserve(n); an.reserve(n); op.reserve(n);

    for (int f = startFrame; f <= endFrame; ++f)
    {
        const FlatPose p = FlattenTransform(a, static_cast<float>(f), width, height);

        cx.push_back(0.5f + p.posX);
        cy.push_back(0.5f + kFusionPosYSign * p.posY);

        // Size is the uniform component; aspect carries the difference between
        // the axes. Both are taken in magnitude -- a mirror is recorded as a
        // flag rather than a negative size, which Fusion's Transform rejects.
        const float sx = std::fabs(p.scaleX);
        const float sy = std::fabs(p.scaleY);
        sz.push_back(sx);
        as.push_back(sx > 1e-6f ? sy / sx : 1.0f);
        if (p.scaleX < 0.0f || p.scaleY < 0.0f) b.hadMirror = true;

        an.push_back(kFusionAngleSign * p.rot);
        op.push_back(p.opacity);

        const float sh = std::fabs(p.shear);
        if (sh > 0.001f) { b.hadShear = true; if (sh > b.maxShear) b.maxShear = sh; }
    }

    const float f0 = static_cast<float>(startFrame);
    b.centreX = FitChannel(cx, f0, tol.centre);
    b.centreY = FitChannel(cy, f0, tol.centre);
    b.size    = FitChannel(sz, f0, tol.size);
    b.aspect  = FitChannel(as, f0, tol.aspect);
    b.angle   = FitChannel(an, f0, tol.angle);
    b.opacity = FitChannel(op, f0, tol.opacity);
    return b;
}

/** @brief A bake reduced to linear keyframes, for hosts whose keys the plugin
 *  cannot shape.
 *
 *  OFX lets a plugin write a parameter's value at a time, but not the handles
 *  between keys -- the host owns the interpolation. So the output-link
 *  publisher wants plain keys placed densely enough that any sane host
 *  interpolation lands within tolerance: exactly what ReduceChannel produces.
 *  Channels and conventions match BakedTransform (Fusion Centre/Size/Angle,
 *  opacity 0..1), so a value read from these keys agrees with a pasted bake. */
struct BakedLinear
{
    std::vector<BakedKey> centreX;
    std::vector<BakedKey> centreY;
    std::vector<BakedKey> size;
    std::vector<BakedKey> angle;
    std::vector<BakedKey> opacity;
    int frameCount = 0;
};

/** @brief Sample the animation per frame into dense keyframes: one key on
 *  every frame, except that a channel which never moves collapses to a single
 *  key. Aspect, shear and mirror are not carried: this feeds single-value
 *  outputs, and the caller warns via the full bake.
 *
 *  Dense deliberately, not reduced. The output-link keys are consumed by the
 *  host's own interpolation, which the plugin cannot shape -- Resolve gives
 *  plugin-written keys smooth auto tangents, and between sparse, unevenly
 *  spaced Douglas-Peucker knots those tangents overshoot. Worse, each channel
 *  reduced alone puts its knots in different places, and a zoom toward a side
 *  multiplies centre by size, so the two channels' interpolation errors beat
 *  against each other as visible wobble. A key on every frame leaves the host
 *  nothing to invent: exact wherever a render happens. Nobody hand-edits
 *  these keys, so their count costs nothing but a slightly longer publish. */
inline BakedLinear BakeToDenseKeys(const AnimParams& a, float width, float height,
                                   int startFrame, int endFrame)
{
    BakedLinear b;
    if (endFrame < startFrame) return b;

    const int n = endFrame - startFrame + 1;
    b.frameCount = n;

    std::vector<float> cx, cy, sz, an, op;
    cx.reserve(n); cy.reserve(n); sz.reserve(n); an.reserve(n); op.reserve(n);

    for (int f = startFrame; f <= endFrame; ++f)
    {
        const FlatPose p = FlattenTransform(a, static_cast<float>(f), width, height);
        cx.push_back(0.5f + p.posX);
        cy.push_back(0.5f + kFusionPosYSign * p.posY);
        sz.push_back(std::fabs(p.scaleX));
        an.push_back(kFusionAngleSign * p.rot);
        op.push_back(p.opacity);
    }

    const float f0 = static_cast<float>(startFrame);
    auto dense = [f0](const std::vector<float>& v) {
        std::vector<BakedKey> out;
        // A channel that never moves is one key, not n copies of it.
        bool flat = true;
        for (float s : v)
            if (std::fabs(s - v.front()) > 1e-6f) { flat = false; break; }
        if (flat) { out.push_back({ f0, v.front() }); return out; }
        out.reserve(v.size());
        for (size_t i = 0; i < v.size(); ++i)
            out.push_back({ f0 + static_cast<float>(i), v[i] });
        return out;
    };
    b.centreX = dense(cx);
    b.centreY = dense(cy);
    b.size    = dense(sz);
    b.angle   = dense(an);
    b.opacity = dense(op);
    return b;
}

/** @brief Total keyframes across every channel, for reporting. */
inline int TotalKeys(const BakedTransform& b)
{
    return static_cast<int>(b.centreX.size() + b.centreY.size() + b.size.size()
                          + b.aspect.size() + b.angle.size() + b.opacity.size());
}

} // namespace mtx
