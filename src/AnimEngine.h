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

/** @brief How the curve behaves once it reaches the target.
 *
 * A cubic bezier has at most two turning points, so it can produce exactly one
 * overshoot and one undershoot -- it cannot rebound repeatedly no matter where
 * the handles go. Bouncing therefore needs a procedural oscillation added on
 * top of the bezier rather than a different handle position.
 */
enum BounceType
{
    kBounceNone   = 0,
    kBounceSpring = 1,   ///< crosses the target, settling above and below it
    kBounceBall   = 2    ///< rebounds off the target, never passing through it
};

/** @brief A CSS-style cubic-bezier easing curve, optionally with a bounce.
 *
 * Control points are P0=(0,0), P1=(x1,y1), P2=(x2,y2), P3=(1,1). Progress is
 * the x axis, eased output the y axis. y may legitimately leave [0,1] -- that
 * is what produces overshoot / "back" easing, and is deliberately not clamped.
 */
struct Easing
{
    float x1, y1, x2, y2;

    // Bounce. Zero-initialised by the presets below, where kBounceNone == 0
    // means the bezier behaves exactly as it always has.
    int   bounceType;
    float bounceAmount;    ///< A: 0 = no oscillation, 1 = full
    float bounceCount;     ///< N: how many rebounds
    float bounceDamping;   ///< D: how quickly they die away
    float bounceStart;     ///< S: fraction of the stage at which the move lands

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
                                float anticipationPct, float overshootPct,
                                int bounceType = kBounceNone,
                                float bounceAmountPct = 0.0f,
                                float bounceCount = 3.0f,
                                float bounceDampingPct = 50.0f,
                                float bounceStartPct = 55.0f)
{
    Easing e;
    e.x1 =        easeInPct  * 0.01f;
    e.x2 = 1.0f - easeOutPct * 0.01f;
    e.y1 =       -anticipationPct * 0.01f * 0.55f;
    e.y2 = 1.0f + overshootPct    * 0.01f * 0.55f;

    e.bounceType   = bounceType;
    e.bounceAmount = bounceAmountPct * 0.01f;
    e.bounceCount  = bounceCount;
    // Damping is exposed as a percentage but used as an exponential rate. 100%
    // maps to a decay of 6, by which point the oscillation is under 0.3% of its
    // starting amplitude -- effectively dead by the end of the move.
    e.bounceDamping = bounceDampingPct * 0.06f;
    e.bounceStart   = bounceStartPct * 0.01f;
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

namespace detail {

/** @brief The plain bezier easing, before any bounce is applied. */
MTX_HD inline float EvalBezier(float p, const Easing& e)
{
    // Linear is both the common case and exactly the one where the solver is
    // pointless, so short-circuit it.
    if (e.x1 == e.y1 && e.x2 == e.y2) return p;

    const float u = SolveBezierParam(p, e.x1, e.x2);
    return BezEval(u, e.y1, e.y2);
}

} // namespace detail

/** @brief Apply an easing curve to a normalised progress value.
 *  @param p progress in [0,1]; values outside are clamped.
 *  @return eased value, which may exceed [0,1] for overshoot and spring curves.
 *
 * ## How bounce is shaped
 *
 * A bounce happens *after* the move lands, not during it. So when a bounce type
 * is active the curve is split in two at the arrival point `S`:
 *
 *     p <  S :  the bezier, compressed into [0, S], so the move reaches the
 *               target early and leaves room for what follows
 *     p >= S :  the target, plus a decaying oscillation that returns to exactly
 *               zero at p = 1
 *
 * With `q = (p - S) / (1 - S)` running 0..1 across the bounce region:
 *
 *     env(q) = (1 - q) * exp(-D * q)          amplitude, 1 at q=0, 0 at q=1
 *     spring : y = 1 + A * sin(pi*N*q) * env(q)
 *     ball   : y = 1 - A * |sin(pi*N*q)| * env(q)
 *
 * Properties this buys, all of which matter:
 *
 *  - **The bounce is at the end**, which is the entire point. An earlier version
 *    scaled the oscillation by the distance still to travel, which put the
 *    largest wobble at p=0 before the move had gone anywhere and left nothing at
 *    the end. That was backwards.
 *  - **Endpoints are exact for any A, N, D and S.** At p=0 the compressed bezier
 *    gives 0. At p=1, `env(1) = 0`, so y is exactly 1 no matter what the
 *    oscillation is doing. An animation has to land on its target value.
 *  - **Continuous at the join.** `sin(0) = 0`, so the bounce region starts at
 *    exactly 1, which is where the compressed bezier finishes.
 *  - **Ball only ever leaves the target on one side**, because `|sin|` never
 *    changes sign, so its term is always applied in the same direction. It
 *    touches the target exactly at each zero crossing and rebounds away. The
 *    spring uses signed `sin` so it alternates either side.
 *
 * A negative Bounce Amount mirrors both models: the spring undershoots before
 * it overshoots, and the ball rebounds off the near side of the target instead
 * of the far side.
 */
MTX_HD inline float ApplyEasing(float p, const Easing& e)
{
    p = Clamp01(p);
    if (p <= 0.0f) return 0.0f;
    if (p >= 1.0f) return 1.0f;

    // Signed: the sign chooses which side of the target the rebound leaves from.
    // Positive springs overshoot first then undershoot; negative does the
    // reverse. Positive balls rebound away from the target on the far side,
    // negative on the near side -- bouncing off a ceiling rather than a floor.
    const float A = e.bounceAmount < -1.0f ? -1.0f : (e.bounceAmount > 1.0f ? 1.0f : e.bounceAmount);
    const bool bouncing = (e.bounceType != kBounceNone)
                       && (A > 0.0f || A < 0.0f)
                       && e.bounceCount > 0.0f;

    if (!bouncing) return detail::EvalBezier(p, e);

    // Arrival point. Clamped away from both ends: at 0 there would be no move
    // left to ease, at 1 no room left to bounce in.
    float S = e.bounceStart;
    if (S < 0.05f) S = 0.05f;
    if (S > 0.95f) S = 0.95f;

    // Before arrival: the user's easing curve, unchanged in shape, simply
    // compressed into [0, S]. Ease In and Ease Out both still apply, so the
    // approach looks exactly like the curve that was drawn.
    //
    // The one thing suppressed is *overshoot* in the approach. A y2 above 1
    // would carry the bezier past the target, force it back down to exactly 1
    // at the join, and only then let the bounce push past a second time. That
    // reversal in the middle is what made the first rebound look like it went
    // the wrong way. Bounce owns the overshoot behaviour; the bezier must not
    // also try to provide it.
    if (p < S)
    {
        Easing approach = e;
        if (approach.y2 > 1.0f) approach.y2 = 1.0f;
        return detail::EvalBezier(p / S, approach);
    }

    const float q    = (p - S) / (1.0f - S);
    const float kPiF = static_cast<float>(kPi);

    const float env = (1.0f - q) * expf(-e.bounceDamping * q);
    const float osc = sinf(kPiF * e.bounceCount * q);

    return (e.bounceType == kBounceSpring) ? 1.0f + A * osc * env
                                           : 1.0f - A * fabsf(osc) * env;
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

    // Motion path. Offsets, in normalised position units, of the two bezier
    // control points away from where they would sit on a straight line. Zero
    // means a straight line -- see EvaluatePath for why that is exact.
    float pathC1X, pathC1Y;
    float pathC2X, pathC2Y;

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
        s.pathC1X     = 0.0f; s.pathC1Y   = 0.0f;
        s.pathC2X     = 0.0f; s.pathC2Y   = 0.0f;
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

/** @brief The two motion-path control points, in normalised position units.
 *
 * They sit one third and two thirds of the way along the straight line from
 * From to To, displaced by the stage's stored offsets. That placement is what
 * makes a zero offset mean *exactly* a straight line: a cubic bezier whose
 * control points are evenly spaced along a segment is the degree-elevated form
 * of the linear interpolation, so it reproduces `Lerp` identically -- same
 * positions, same uniform speed. A path that is "off" therefore costs nothing
 * and changes nothing, and existing projects are untouched.
 */
MTX_HD inline void PathControlPoints(const Stage& s,
                                     float& c1x, float& c1y, float& c2x, float& c2y)
{
    const float dx = s.posXTo - s.posXFrom;
    const float dy = s.posYTo - s.posYFrom;

    c1x = s.posXFrom + dx * (1.0f / 3.0f) + s.pathC1X;
    c1y = s.posYFrom + dy * (1.0f / 3.0f) + s.pathC1Y;
    c2x = s.posXFrom + dx * (2.0f / 3.0f) + s.pathC2X;
    c2y = s.posYFrom + dy * (2.0f / 3.0f) + s.pathC2Y;
}

/** @brief Position along the motion path at eased progress @p e.
 *
 * @param e the *eased* progress, not raw time. Easing therefore governs speed
 *          along the path while the control points govern its shape -- the
 *          standard separation, and the reason bounce and path compose: a
 *          bounce makes the object run back and forth along the curve rather
 *          than distorting it.
 */
MTX_HD inline void EvaluatePath(const Stage& s, float e, float& outX, float& outY)
{
    float c1x, c1y, c2x, c2y;
    PathControlPoints(s, c1x, c1y, c2x, c2y);

    const float u  = 1.0f - e;
    const float w0 = u * u * u;
    const float w1 = 3.0f * u * u * e;
    const float w2 = 3.0f * u * e * e;
    const float w3 = e * e * e;

    outX = w0 * s.posXFrom + w1 * c1x + w2 * c2x + w3 * s.posXTo;
    outY = w0 * s.posYFrom + w1 * c1y + w2 * c2y + w3 * s.posYTo;
}

/** @brief The transform contributed by a single stage, in pixel space. */
MTX_HD inline Mat3 EvaluateStage(const Stage& s, float t, float width, float height)
{
    const float e = StageProgress(s, t);

    const float scale = Lerp(s.scaleFrom, s.scaleTo, e);
    const float rot   = Lerp(s.rotFrom,   s.rotTo,   e);

    float px, py;
    EvaluatePath(s, e, px, py);
    const float posX = px * width;
    const float posY = py * height;

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

/** @brief Whether the animation does nothing at *any* time.
 *
 * Deliberately time-independent, and that is the whole point.
 *
 * The obvious way to answer "is this effect a no-op" is to evaluate the
 * transform at the current frame and compare it with the identity. That is a
 * trap. Outside a stage's frame range the progress pins to 0 or 1, so the
 * transform collapses to the From or To pose -- which is usually the identity --
 * and the effect would report itself as a pass-through on exactly those frames.
 * Hosts cache that verdict per frame. Move the stage's start or end so that a
 * previously-outside frame is now mid-animation, and the host may never ask
 * again: it still believes the effect does nothing there, and the picture stops
 * responding to edits. That reproduces as "adjustments work while the playhead
 * is inside the range and are ignored outside it".
 *
 * Answering the same for every frame removes the failure mode entirely. The
 * cost is giving up the skip on frames that merely happen to be neutral, which
 * measures at about 0.3 ms per frame -- not worth a stale picture.
 */
inline bool IsNoOp(const AnimParams& a)
{
    const float kEps = 1e-5f;
    const auto near = [kEps](float v, float target) { return fabsf(v - target) <= kEps; };

    const int count = a.stageCount < kMaxStages ? a.stageCount : kMaxStages;
    for (int i = 0; i < count; ++i)
    {
        const Stage& s = a.stages[i];
        if (!s.enabled) continue;

        if (!near(s.scaleFrom, 1.0f)   || !near(s.scaleTo, 1.0f))   return false;
        if (!near(s.posXFrom, 0.0f)    || !near(s.posXTo, 0.0f))    return false;
        if (!near(s.posYFrom, 0.0f)    || !near(s.posYTo, 0.0f))    return false;
        if (!near(s.rotFrom, 0.0f)     || !near(s.rotTo, 0.0f))     return false;
        if (!near(s.opacityFrom, 1.0f) || !near(s.opacityTo, 1.0f)) return false;

        // Path offsets bulge the trajectory away from the endpoints, so a stage
        // that starts and ends at the same place can still move in between.
        if (!near(s.pathC1X, 0.0f) || !near(s.pathC1Y, 0.0f)) return false;
        if (!near(s.pathC2X, 0.0f) || !near(s.pathC2Y, 0.0f)) return false;
    }
    return true;
}

/** @brief The inverse transform, which is what a renderer actually needs:
 *  for each destination pixel it gives the source location to sample. */
MTX_HD inline Mat3 EvaluateInverseTransform(const AnimParams& a, float t,
                                            float width, float height)
{
    return Invert(EvaluateTransform(a, t, width, height));
}

} // namespace mtx
