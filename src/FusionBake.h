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
    std::vector<BakedKey> centreX;
    std::vector<BakedKey> centreY;
    std::vector<BakedKey> size;
    std::vector<BakedKey> aspect;
    std::vector<BakedKey> angle;
    std::vector<BakedKey> opacity;

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
    b.centreX = ReduceChannel(cx, f0, tol.centre);
    b.centreY = ReduceChannel(cy, f0, tol.centre);
    b.size    = ReduceChannel(sz, f0, tol.size);
    b.aspect  = ReduceChannel(as, f0, tol.aspect);
    b.angle   = ReduceChannel(an, f0, tol.angle);
    b.opacity = ReduceChannel(op, f0, tol.opacity);
    return b;
}

/** @brief Total keyframes across every channel, for reporting. */
inline int TotalKeys(const BakedTransform& b)
{
    return static_cast<int>(b.centreX.size() + b.centreY.size() + b.size.size()
                          + b.aspect.size() + b.angle.size() + b.opacity.size());
}

} // namespace mtx
