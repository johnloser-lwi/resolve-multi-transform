#pragma once

// The animation engine: cubic-bezier easing, per-stage staggered timing, and
// composition of the stages into a single transform.
//
// This is the heart of the plugin. Resolve supports neither parametric (curve)
// parameters nor custom parameter-panel interacts, so the host cannot own this
// animation -- the plugin does. That is not merely a workaround: owning it is
// what makes motion blur exact, because the transform can be evaluated
// analytically at any sub-frame time rather than sampled from host keyframes.
//
// Header-only and device-safe: the CUDA kernel evaluates the identical code.

#include "TransformMath.h"

namespace mtx {

/** Maximum stages. OFX cannot add parameters dynamically, so the ceiling is
 *  compiled in and surplus stages are hidden via a Stage Count dropdown. */
constexpr int kMaxStages = 4;

////////////////////////////////////////////////////////////////////////////////
// Easing

/** @brief A CSS-style cubic-bezier easing curve.
 *
 * Control points are P0=(0,0), P1=(x1,y1), P2=(x2,y2), P3=(1,1). Progress is
 * the x axis, eased output the y axis. y may legitimately leave [0,1] -- that
 * is what produces overshoot / "back" easing, and is deliberately not clamped.
 */
struct Easing
{
    float x1, y1, x2, y2;

    MTX_HD static Easing Linear()   { return { 0.0f,  0.0f,  1.0f,  1.0f  }; }
    MTX_HD static Easing Smooth()   { return { 0.42f, 0.0f,  0.58f, 1.0f  }; } // ease-in-out
    MTX_HD static Easing EaseOut()  { return { 0.0f,  0.0f,  0.58f, 1.0f  }; }
    MTX_HD static Easing EaseIn()   { return { 0.42f, 0.0f,  1.0f,  1.0f  }; }
};

/** @brief Build an easing curve from four values a human can reason about.
 *
 * Raw bezier handles are precise but unusable as a UI: nobody thinks in terms
 * of "X1 = 0.42". These four are the controls every animation tool actually
 * exposes, and they map exactly onto the bezier.
 *
 * @param easeInPct       0 = starts at full speed, 100 = starts very gradually.
 *                        This is the damping on the way in.
 * @param easeOutPct      0 = stops abruptly, 100 = glides to a halt.
 * @param anticipationPct pulls back before moving (negative y on the first handle).
 * @param overshootPct    goes past the target and settles back (y > 1 on the second).
 *
 * The 0.55 scale on the y handles is what makes 100% land on the classic CSS
 * "back" easing, cubic-bezier(.68, -.55, .27, 1.55).
 */
MTX_HD inline Easing MakeEasing(float easeInPct, float easeOutPct,
                                float anticipationPct, float overshootPct)
{
    Easing e;
    e.x1 =        easeInPct  * 0.01f;
    e.x2 = 1.0f - easeOutPct * 0.01f;
    e.y1 =       -anticipationPct * 0.01f * 0.55f;
    e.y2 = 1.0f + overshootPct    * 0.01f * 0.55f;
    return e;
}

namespace detail {

MTX_HD inline float BezA(float p1, float p2) { return 1.0f - 3.0f * p2 + 3.0f * p1; }
MTX_HD inline float BezB(float p1, float p2) { return 3.0f * p2 - 6.0f * p1; }
MTX_HD inline float BezC(float p1)           { return 3.0f * p1; }

/** Evaluate the polynomial form of one bezier axis at parameter u. */
MTX_HD inline float BezEval(float u, float p1, float p2)
{
    return ((BezA(p1, p2) * u + BezB(p1, p2)) * u + BezC(p1)) * u;
}

MTX_HD inline float BezSlope(float u, float p1, float p2)
{
    return (3.0f * BezA(p1, p2) * u + 2.0f * BezB(p1, p2)) * u + BezC(p1);
}

/** Solve x(u) = targetX for u. Newton-Raphson, falling back to bisection when
 *  the slope is too flat for Newton to be trustworthy. */
MTX_HD inline float SolveBezierParam(float targetX, float x1, float x2)
{
    float u = targetX;

    // Newton first: converges in a couple of iterations for typical curves.
    for (int i = 0; i < 8; ++i)
    {
        const float err = BezEval(u, x1, x2) - targetX;
        if (err > -1e-6f && err < 1e-6f) return u;

        const float slope = BezSlope(u, x1, x2);
        if (slope > -1e-6f && slope < 1e-6f) break;

        u -= err / slope;
    }

    // Bisection: slower but cannot diverge, so guarantees termination on the
    // pathological curves Newton gives up on.
    float lo = 0.0f, hi = 1.0f;
    u = targetX;
    if (u < lo) u = lo;
    if (u > hi) u = hi;

    for (int i = 0; i < 24; ++i)
    {
        const float x = BezEval(u, x1, x2);
        if (x > targetX - 1e-6f && x < targetX + 1e-6f) break;
        if (x > targetX) hi = u; else lo = u;
        u = 0.5f * (lo + hi);
    }
    return u;
}

} // namespace detail

/** @brief Apply an easing curve to a normalised progress value.
 *  @param p progress in [0,1]; values outside are clamped.
 *  @return eased value, which may exceed [0,1] for overshoot curves.
 */
MTX_HD inline float ApplyEasing(float p, const Easing& e)
{
    p = Clamp01(p);
    if (p <= 0.0f) return 0.0f;
    if (p >= 1.0f) return 1.0f;

    // Linear is both the common case and exactly the one where the solver is
    // pointless, so short-circuit it.
    if (e.x1 == e.y1 && e.x2 == e.y2) return p;

    const float u = detail::SolveBezierParam(p, e.x1, e.x2);
    return detail::BezEval(u, e.y1, e.y2);
}

////////////////////////////////////////////////////////////////////////////////
// Stages

/** @brief One transform stage: its own start and end frame, a from/to pair for
 *  every channel, and its own easing curve.
 *
 * Each stage owns its timing outright. Staggering is not a separate control --
 * it is simply what happens when stages have different start frames. Stage 1
 * running 100->120 with stage 2 running 106->126 is a six-frame stagger, and
 * the two can also differ in length, which a single shared duration could not
 * express.
 *
 * @note Frames are timeline-absolute, because that is the only time base
 *       Resolve gives a plugin (it reports a useless clip frame range, so the
 *       clip's own start is unknowable). The UI sets these from the playhead
 *       rather than making anyone type frame numbers.
 */
struct Stage
{
    bool  enabled;

    float startFrame;   ///< timeline-absolute frame this stage begins
    float endFrame;     ///< timeline-absolute frame this stage completes

    float scaleFrom,    scaleTo;      ///< uniform scale multiplier
    float posXFrom,     posXTo;       ///< normalised: 1.0 == one image width
    float posYFrom,     posYTo;
    float rotFrom,      rotTo;        ///< degrees
    float opacityFrom,  opacityTo;    ///< 0..1
    float anchorX,      anchorY;      ///< normalised, 0.5,0.5 == image centre

    Easing easing;

    MTX_HD static Stage Default()
    {
        Stage s;
        s.enabled    = false;
        s.startFrame = 0.0f;
        s.endFrame   = 24.0f;
        s.scaleFrom   = 1.0f; s.scaleTo   = 1.0f;
        s.posXFrom    = 0.0f; s.posXTo    = 0.0f;
        s.posYFrom    = 0.0f; s.posYTo    = 0.0f;
        s.rotFrom     = 0.0f; s.rotTo     = 0.0f;
        s.opacityFrom = 1.0f; s.opacityTo = 1.0f;
        s.anchorX     = 0.5f; s.anchorY   = 0.5f;
        s.easing      = Easing::Smooth();
        return s;
    }

    /** @brief Derived, never stored: the UI shows this but does not own it. */
    MTX_HD float Duration() const { return endFrame - startFrame; }
};

/** @brief The complete animation description. */
struct AnimParams
{
    int   stageCount;
    Stage stages[kMaxStages];

    MTX_HD static AnimParams Default()
    {
        AnimParams a;
        a.stageCount = 1;
        for (int i = 0; i < kMaxStages; ++i) a.stages[i] = Stage::Default();
        a.stages[0].enabled = true;
        return a;
    }
};

/** @brief Normalised, eased progress of one stage at time @p t.
 *  @param t timeline-absolute frame (may be fractional for motion blur).
 */
MTX_HD inline float StageProgress(const Stage& s, float t)
{
    const float dur = s.endFrame - s.startFrame;

    // A zero-length or inverted stage is an instant cut at its start frame
    // rather than a division by zero or a backwards animation.
    if (dur <= 1e-6f) return t < s.startFrame ? 0.0f : 1.0f;

    return ApplyEasing((t - s.startFrame) / dur, s.easing);
}

/** @brief The transform contributed by a single stage, in pixel space. */
MTX_HD inline Mat3 EvaluateStage(const Stage& s, float t, float width, float height)
{
    const float e = StageProgress(s, t);

    const float scale = Lerp(s.scaleFrom, s.scaleTo, e);
    const float rot   = Lerp(s.rotFrom,   s.rotTo,   e);
    const float posX  = Lerp(s.posXFrom,  s.posXTo,  e) * width;
    const float posY  = Lerp(s.posYFrom,  s.posYTo,  e) * height;

    return MakeTransform(s.anchorX * width, s.anchorY * height,
                         scale, scale, rot, posX, posY);
}

/** @brief The composed forward transform (source -> destination) at time @p t.
 *
 * Stages compose in order, so stage 1 is the outermost transform. Disabled
 * stages and stages beyond stageCount contribute nothing.
 */
MTX_HD inline Mat3 EvaluateTransform(const AnimParams& a, float t, float width, float height)
{
    Mat3 result = Mat3::Identity();

    const int count = a.stageCount < kMaxStages ? a.stageCount : kMaxStages;
    for (int i = 0; i < count; ++i)
    {
        if (!a.stages[i].enabled) continue;
        result = result * EvaluateStage(a.stages[i], t, width, height);
    }
    return result;
}

/** @brief Combined opacity of every contributing stage at time @p t.
 *
 * Stages multiply, exactly as their transforms do: two stages each fading to
 * 0.5 leave 0.25. Keeping opacity consistent with the transform composition
 * means a stage is one coherent thing rather than two rules to remember.
 */
MTX_HD inline float EvaluateOpacity(const AnimParams& a, float t)
{
    float opacity = 1.0f;

    const int count = a.stageCount < kMaxStages ? a.stageCount : kMaxStages;
    for (int i = 0; i < count; ++i)
    {
        const Stage& s = a.stages[i];
        if (!s.enabled) continue;
        opacity *= Lerp(s.opacityFrom, s.opacityTo, StageProgress(s, t));
    }

    // Overshoot easing can legitimately drive the interpolation past its
    // endpoints; a negative or >1 opacity is meaningless, so clamp here rather
    // than producing out-of-range pixels.
    return Clamp01(opacity);
}

/** @brief The inverse transform, which is what a renderer actually needs:
 *  for each destination pixel it gives the source location to sample. */
MTX_HD inline Mat3 EvaluateInverseTransform(const AnimParams& a, float t,
                                            float width, float height)
{
    return Invert(EvaluateTransform(a, t, width, height));
}

} // namespace mtx
