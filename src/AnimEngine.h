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
/** @brief What a stage's start and end frames are measured from. */
enum TimingAnchor
{
    kAnchorClipStart = 0,   ///< 0 is the clip's first frame
    kAnchorClipEnd   = 1,   ///< 0 is the clip's last frame; earlier frames are negative
    kAnchorTimeline  = 2,   ///< absolute timeline frames, ignoring the clip
    kAnchorStretch   = 3    ///< percentage of the clip: 0 is the first frame, 100 the last
};

struct Stage
{
    bool  enabled;

    int   anchor;       ///< TimingAnchor: what startFrame/endFrame are relative to
    float startFrame;   ///< frames from the anchor
    float endFrame;     ///< frames from the anchor

    float scaleFrom,    scaleTo;      ///< X scale, and both axes when linked
    float scaleYFrom,   scaleYTo;     ///< Y scale, used only when linkScale is false
    bool  linkScale;                  ///< Y follows X
    float posXFrom,     posXTo;       ///< normalised: 1.0 == one image width
    float posYFrom,     posYTo;
    float rotFrom,      rotTo;        ///< degrees, in the image plane
    float tiltXFrom,    tiltXTo;      ///< degrees about the horizontal axis
    float swivelYFrom,  swivelYTo;    ///< degrees about the vertical axis
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
        s.anchor     = kAnchorClipStart;
        s.startFrame = 0.0f;
        s.endFrame   = 24.0f;
        s.scaleFrom   = 1.0f; s.scaleTo   = 1.0f;
        s.scaleYFrom  = 1.0f; s.scaleYTo  = 1.0f;
        s.linkScale   = true;
        s.posXFrom    = 0.0f; s.posXTo    = 0.0f;
        s.posYFrom    = 0.0f; s.posYTo    = 0.0f;
        s.rotFrom     = 0.0f; s.rotTo     = 0.0f;
        s.tiltXFrom   = 0.0f; s.tiltXTo   = 0.0f;
        s.swivelYFrom = 0.0f; s.swivelYTo = 0.0f;
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

/** @brief A static resting pose, applied underneath the animation.
 *
 * Exists so an element can be positioned and sized *before* anything animates,
 * without spending a stage on a From==To pair.
 */
struct BasePose
{
    float scaleX, scaleY;
    bool  linkScale;
    float posX, posY;
    float rot;            ///< in-plane rotation, degrees
    float tiltX;          ///< about the horizontal axis, degrees
    float swivelY;        ///< about the vertical axis, degrees
    float opacity;        ///< 0..1
    float anchorX, anchorY;

    MTX_HD static BasePose Default()
    {
        BasePose b;
        b.scaleX = 1.0f; b.scaleY = 1.0f; b.linkScale = true;
        b.posX   = 0.0f; b.posY   = 0.0f;
        b.rot    = 0.0f; b.tiltX  = 0.0f; b.swivelY = 0.0f;
        b.opacity = 1.0f;
        b.anchorX = 0.5f; b.anchorY = 0.5f;
        return b;
    }

    /** @brief Whether this pose does nothing, so it can be skipped entirely. */
    MTX_HD bool IsNeutral() const
    {
        const float e = 1e-5f;
        return fabsf(scaleX - 1.0f) <= e && fabsf(scaleY - 1.0f) <= e
            && fabsf(posX) <= e && fabsf(posY) <= e
            && fabsf(rot) <= e && fabsf(tiltX) <= e && fabsf(swivelY) <= e
            && fabsf(opacity - 1.0f) <= e;
    }
};

/** @brief Scale factors for an orthographic axis rotation.
 *
 * An orthographic rotation about a screen-aligned axis is exactly a scale: turn
 * a plane 60 degrees about the vertical axis and it covers cos(60) = half the
 * width, with its height untouched. No foreshortening, because parallel edges
 * stay parallel under an affine transform.
 *
 * Past 90 degrees the cosine goes negative and the image mirrors, which is the
 * correct reading -- a card turned past edge-on shows its back.
 *
 * At exactly 90 degrees the factor is zero and the transform collapses. That is
 * handled where the shutter samples are built, not here: a degenerate matrix has
 * no inverse, and the renderer must draw nothing rather than fall back to an
 * untransformed frame.
 */
MTX_HD inline void OrthographicScale(float tiltXDeg, float swivelYDeg,
                                     float& outX, float& outY)
{
    const float k = static_cast<float>(kPi) / 180.0f;
    outX = cosf(swivelYDeg * k);
    outY = cosf(tiltXDeg  * k);
}

/** @brief The complete animation description. */
struct AnimParams
{
    int      stageCount;
    BasePose base;
    Stage    stages[kMaxStages];

    /// The clip, so a stage can resolve its anchor. Times handed to the
    /// evaluator are clip-relative, meaning 0 is the clip's first frame.
    float clipStart;    ///< timeline frame the clip begins on
    float clipLength;   ///< clip length in frames; 0 when the host will not say

    MTX_HD static AnimParams Default()
    {
        AnimParams a;
        a.stageCount = 1;
        a.base       = BasePose::Default();
        a.clipStart  = 0.0f;
        a.clipLength = 0.0f;
        for (int i = 0; i < kMaxStages; ++i) a.stages[i] = Stage::Default();
        a.stages[0].enabled = true;
        return a;
    }
};

/** @brief How a clip-relative time maps onto a stage's own units.
 *
 * Anchoring is an affine change of variable, `local = (t - offset) * scale`:
 *
 *   Clip Start  offset 0        scale 1            already clip-relative
 *   Clip End    offset L - 1    scale 1            zero on the clip's LAST frame
 *   Timeline    offset -start   scale 1            undoes the clip-relative shift
 *   Stretch     offset 0        scale 100/(L - 1)  frames become percent of clip
 *
 * Stretch is the reason this is a scale and not merely an offset. Its Start and
 * End are percentages, so the animation occupies the same *proportion* of the
 * clip however long the clip is -- trim it shorter and the move compresses to
 * match instead of running off the end.
 *
 * The `L - 1` in two places is deliberate. A clip spanning `length` frames has
 * its last visible frame at `length - 1`; using the length itself would put the
 * reference one frame past the end, so an outro written `-20 -> 0` would finish
 * fractionally short on the final visible frame rather than landing exactly.
 */
struct AnchorMapping { float offset; float scale; };

MTX_HD inline AnchorMapping AnchorMap(const AnimParams& a, const Stage& s)
{
    AnchorMapping m{ 0.0f, 1.0f };

    // A clip of one frame or less has no span to divide by or count back from,
    // so every anchor degenerates safely to the clip start.
    const bool haveSpan = a.clipLength > 1.0f;

    if (s.anchor == kAnchorTimeline)
    {
        m.offset = -a.clipStart;
    }
    else if (s.anchor == kAnchorClipEnd)
    {
        m.offset = haveSpan ? a.clipLength - 1.0f : 0.0f;
    }
    else if (s.anchor == kAnchorStretch)
    {
        m.scale = haveSpan ? 100.0f / (a.clipLength - 1.0f) : 1.0f;
    }
    return m;
}

/** @brief A clip-relative time expressed in one stage's own units. */
MTX_HD inline float StageLocalTime(const AnimParams& a, const Stage& s, float clipTime)
{
    const AnchorMapping m = AnchorMap(a, s);
    return (clipTime - m.offset) * m.scale;
}

/** @brief The inverse: where one of a stage's frames sits in clip time.
 *  Used by the overlay, whose ruler is clip time. */
MTX_HD inline float ClipTimeFromStageFrame(const AnimParams& a, const Stage& s, float stageFrame)
{
    const AnchorMapping m = AnchorMap(a, s);
    const float scale = (m.scale > 1e-9f || m.scale < -1e-9f) ? m.scale : 1.0f;
    return stageFrame / scale + m.offset;
}

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

    const float rot = Lerp(s.rotFrom, s.rotTo, e);

    // Y follows X while linked, so the unused Y values can sit at whatever they
    // were without leaking into the result.
    const float scaleX = Lerp(s.scaleFrom, s.scaleTo, e);
    const float scaleY = s.linkScale ? scaleX : Lerp(s.scaleYFrom, s.scaleYTo, e);

    // Orthographic tilt and swivel are scale factors, so they simply multiply.
    float orthoX, orthoY;
    OrthographicScale(Lerp(s.tiltXFrom,   s.tiltXTo,   e),
                      Lerp(s.swivelYFrom, s.swivelYTo, e), orthoX, orthoY);

    float px, py;
    EvaluatePath(s, e, px, py);
    const float posX = px * width;
    const float posY = py * height;

    return MakeTransform(s.anchorX * width, s.anchorY * height,
                         scaleX * orthoX, scaleY * orthoY, rot, posX, posY);
}

/** @brief The static base pose as a matrix, in pixel space. */
MTX_HD inline Mat3 EvaluateBase(const BasePose& b, float width, float height)
{
    const float scaleX = b.scaleX;
    const float scaleY = b.linkScale ? b.scaleX : b.scaleY;

    float orthoX, orthoY;
    OrthographicScale(b.tiltX, b.swivelY, orthoX, orthoY);

    return MakeTransform(b.anchorX * width, b.anchorY * height,
                         scaleX * orthoX, scaleY * orthoY, b.rot,
                         b.posX * width, b.posY * height);
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
        result = result * EvaluateStage(a.stages[i],
                                        StageLocalTime(a, a.stages[i], t),
                                        width, height);
    }

    // The base goes rightmost, which means applied first: it is the resting pose
    // the animation acts upon. The other order would put every stage's
    // translation into the base's scaled space, so a base at 50% would silently
    // halve the distance every existing animation travels.
    if (!a.base.IsNeutral())
        result = result * EvaluateBase(a.base, width, height);

    return result;
}

/** @brief Everything one stage's transform is sandwiched between.
 *
 * `EvaluateTransform` composes `S0 * S1 * ... * Sn * Base`, so a single stage's
 * own matrix says nothing about where the image actually ends up -- the stages
 * around it move it somewhere else. Editing stage 2 with a gizmo drawn from
 * stage 2 alone therefore means dragging a box that is nowhere near the picture.
 *
 * Splitting the composition at stage @p stageIndex gives the two halves needed
 * to put a control back on the image:
 *
 *     full = outer * Sᵢ * inner
 *
 * @c outer is applied *after* the stage (the stages to its left), @c inner
 * before it (the stages to its right, plus the base pose). A control drawn
 * through the whole product lands on the picture; a drag mapped back through
 * `Invert(outer)` still writes the stage's own numbers.
 */
struct StageContext
{
    Mat3 outer;   ///< stages composed to the left of this one
    Mat3 inner;   ///< stages to the right, and the base pose
};

/** @brief The context surrounding the base pose at time @p t.
 *
 * The base composes rightmost, so *every* stage is outside it and there is
 * nothing inside. Not expressible through EvaluateStageContext, which always
 * folds the base into `inner` -- here the base is the thing being edited.
 */
inline StageContext EvaluateBaseContext(const AnimParams& a, float t,
                                        float width, float height)
{
    StageContext ctx;
    ctx.outer = Mat3::Identity();
    ctx.inner = Mat3::Identity();

    const int count = a.stageCount < kMaxStages ? a.stageCount : kMaxStages;
    for (int i = 0; i < count; ++i)
    {
        if (!a.stages[i].enabled) continue;
        ctx.outer = ctx.outer * EvaluateStage(a.stages[i],
                                              StageLocalTime(a, a.stages[i], t), width, height);
    }
    return ctx;
}

/** @brief The context surrounding @p stageIndex at time @p t.
 *
 * Every other stage is evaluated at the current time, not at the edited stage's
 * endpoints: the question a gizmo answers is "where would this end sit, given
 * everything else as it is right now", which is what makes the box coincide
 * with the rendered image when the playhead is parked on that end.
 */
inline StageContext EvaluateStageContext(const AnimParams& a, int stageIndex,
                                         float t, float width, float height)
{
    StageContext ctx;
    ctx.outer = Mat3::Identity();
    ctx.inner = Mat3::Identity();

    const int count = a.stageCount < kMaxStages ? a.stageCount : kMaxStages;
    for (int i = 0; i < count; ++i)
    {
        if (i == stageIndex) continue;
        if (!a.stages[i].enabled) continue;

        const Mat3 m = EvaluateStage(a.stages[i],
                                     StageLocalTime(a, a.stages[i], t), width, height);
        if (i < stageIndex) ctx.outer = ctx.outer * m;
        else                ctx.inner = ctx.inner * m;
    }

    if (!a.base.IsNeutral())
        ctx.inner = ctx.inner * EvaluateBase(a.base, width, height);

    return ctx;
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
        opacity *= Lerp(s.opacityFrom, s.opacityTo,
                        StageProgress(s, StageLocalTime(a, s, t)));
    }

    opacity *= a.base.opacity;

    // Overshoot easing can legitimately drive the interpolation past its
    // endpoints; a negative or >1 opacity is meaningless, so clamp here rather
    // than producing out-of-range pixels.
    return Clamp01(opacity);
}

/** @brief A whole animation collapsed to one static pose.
 *
 * Always expressed about the image centre, because the anchor is a free choice
 * here -- fixing it at 0.5, 0.5 removes an unknown and makes the translation
 * solvable.
 */
struct FlatPose
{
    float scaleX, scaleY;
    float rot;              ///< degrees
    float posX, posY;       ///< normalised, as the position parameters are
    float opacity;          ///< 0..1

    /// How much shear had to be discarded, relative to the pose's own size.
    /// Zero for anything built from rotations and uniform scales; non-zero only
    /// when a rotation is combined with a non-uniform scale, an orthographic
    /// tilt or a swivel. See FlattenTransform.
    float shear;
};

/** @brief Collapse the entire animation at time @p t into a single pose.
 *
 * For carrying a move across a cut: the composed result at the end of one
 * clip's animation becomes the starting pose of the next, so the movement
 * continues instead of snapping back.
 *
 * The maths is a Gram-Schmidt (QR) decomposition of the composed matrix. A
 * transform built as anchor/scale/rotate/translate has orthogonal columns, and
 * recovering the parts is then exact. Composition does not always preserve
 * that: rotate, then scale one axis, and the result contains **shear**, which
 * no combination of this plugin's parameters can express. That component is
 * dropped and reported in @c shear so the caller can say so rather than quietly
 * producing a pose that does not match what was on screen.
 */
inline FlatPose FlattenTransform(const AnimParams& a, float t, float width, float height)
{
    const Mat3 m = EvaluateTransform(a, t, width, height);

    // Columns of the linear part. The first is the image's X axis after the
    // transform, the second its Y axis.
    const float c1x = m.m[0], c1y = m.m[3];
    const float c2x = m.m[1], c2y = m.m[4];

    FlatPose f;
    f.scaleX = sqrtf(c1x * c1x + c1y * c1y);
    f.rot    = atan2f(c1y, c1x) * 180.0f / static_cast<float>(kPi);

    if (f.scaleX > 1e-9f)
    {
        // Unit vector along the first column, and the one at right angles to it
        // -- which is where an unsheared second column would lie.
        const float q1x = c1x / f.scaleX;
        const float q1y = c1y / f.scaleX;
        const float q2x = -q1y;
        const float q2y =  q1x;

        // Signed, so a mirrored transform (a swivel past 90 degrees) survives
        // as a negative scale rather than as a rotation of 180 degrees.
        f.scaleY = c2x * q2x + c2y * q2y;
        f.shear  = c2x * q1x + c2y * q1y;
    }
    else
    {
        // Collapsed to nothing: no axis to measure the second column against.
        f.scaleY = sqrtf(c2x * c2x + c2y * c2y);
        f.shear  = 0.0f;
        f.rot    = 0.0f;
    }

    // Translation, solved for an anchor at the centre. MakeTransform maps
    //     p -> L(p - anchor) + anchor + translation
    // so the matrix's own translation is  anchor - L*anchor + translation.
    const float ax = 0.5f * width;
    const float ay = 0.5f * height;
    const float lax = m.m[0] * ax + m.m[1] * ay;
    const float lay = m.m[3] * ax + m.m[4] * ay;

    const float tx = m.m[2] - ax + lax;
    const float ty = m.m[5] - ay + lay;

    f.posX = width  > 1e-9f ? tx / width  : 0.0f;
    f.posY = height > 1e-9f ? ty / height : 0.0f;

    f.opacity = EvaluateOpacity(a, t);

    // Relative to the pose's size, so the caller can judge "is this a lot"
    // without knowing the frame dimensions.
    const float scale = fabsf(f.scaleX) > 1e-6f ? fabsf(f.scaleX) : 1.0f;
    f.shear /= scale;

    return f;
}

/** @brief Clip time at which the animation has finished.
 *
 * The latest end frame of any enabled stage, which is the moment the composed
 * pose stops changing and therefore the one worth carrying to the next clip.
 */
inline float AnimationEndTime(const AnimParams& a, float fallback)
{
    const int count = a.stageCount < kMaxStages ? a.stageCount : kMaxStages;

    bool  any  = false;
    float last = 0.0f;
    for (int i = 0; i < count; ++i)
    {
        const Stage& s = a.stages[i];
        if (!s.enabled) continue;

        const float e = ClipTimeFromStageFrame(a, s, s.endFrame > s.startFrame ? s.endFrame
                                                                               : s.startFrame);
        if (!any || e > last) { last = e; any = true; }
    }
    return any ? last : fallback;
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
/** Steps used to sample an easing curve's speed. Shared so the shading and the
 *  peak marker on the timeline are measured the same way and cannot disagree. */
constexpr int kVelocitySteps = 48;

/** @brief How much the eased value moves across step @p k of kVelocitySteps.
 *
 * Sampled as a difference rather than differentiated analytically: the curve is
 * not merely a bezier once a bounce multiplies an oscillation into it, and a
 * difference is both simpler and correct for every easing the plugin offers.
 */
inline float EasingSpeed(const Easing& e, int k)
{
    const float p0 = static_cast<float>(k)     / static_cast<float>(kVelocitySteps);
    const float p1 = static_cast<float>(k + 1) / static_cast<float>(kVelocitySteps);
    return fabsf(ApplyEasing(p1, e) - ApplyEasing(p0, e));
}

/** @brief Progress in 0..1 at which the move is at its fastest.
 *
 * This is the frame an edit usually wants to land on -- the middle of the
 * action rather than the middle of the stage, which are the same thing only for
 * linear easing.
 *
 * Ties go to the earliest step. A linear curve is one long tie, and reporting
 * its start is more honest than picking an arbitrary point in the middle and
 * implying the motion peaks there.
 */
inline float PeakVelocityProgress(const Easing& e)
{
    float best = -1.0f;
    int   peak = 0;
    for (int k = 0; k < kVelocitySteps; ++k)
    {
        const float v = EasingSpeed(e, k);
        if (v > best) { best = v; peak = k; }
    }
    return (static_cast<float>(peak) + 0.5f) / static_cast<float>(kVelocitySteps);
}

/** @brief Reshape a curve so its fastest moment lands at @p target progress.
 *
 * The companion to sliding a stage: same goal, but the stage stays exactly
 * where it is and the *curve* moves instead.
 *
 * The total amount of easing is preserved and only its **balance** is changed.
 * Ease In and Ease Out are what decide where the peak sits -- weight the curve
 * towards Ease In and it accelerates late, towards Ease Out and it peaks early
 * -- so redistributing between them moves the peak while leaving the curve
 * about as soft as it was. Anticipation, overshoot and any bounce are untouched,
 * which matters because those are the parts that give a move its character.
 *
 * Solved by sampling rather than by bisection. The peak is *usually* monotonic
 * in the balance, but a bounce multiplies an oscillation into the curve and can
 * break that; a search that assumed monotonicity would then converge on the
 * wrong side. Sampling the whole range costs a few thousand evaluations once,
 * on a button press, and cannot be fooled that way.
 *
 * @param outPeak where the peak actually ended up, which is not always @p target
 *        -- a curve with little easing to redistribute simply cannot reach every
 *        position, and the caller should say so rather than pretend.
 * @return false when there is no easing at all to redistribute.
 */
inline bool SolvePeakBalance(const Easing& current, float target,
                             float& outEaseInPct, float& outEaseOutPct, float& outPeak)
{
    // Recovered from the handles, which is where the two amounts live.
    const float total = current.x1 * 100.0f + (1.0f - current.x2) * 100.0f;
    if (total < 1e-3f) return false;      // linear: nothing to move around

    constexpr int kSteps = 200;

    float bestBalance = 0.5f;
    float bestErr     = 1e9f;
    float bestPeak    = 0.5f;
    bool  any         = false;

    for (int k = 0; k <= kSteps; ++k)
    {
        const float b      = static_cast<float>(k) / static_cast<float>(kSteps);
        const float inPct  = total * b;
        const float outPct = total * (1.0f - b);

        // Neither amount may exceed its own range, so a heavily eased curve can
        // only be rebalanced within the band where both ends stay legal.
        if (inPct > 100.0f || outPct > 100.0f) continue;

        Easing e = current;
        e.x1 = inPct * 0.01f;
        e.x2 = 1.0f - outPct * 0.01f;

        const float peak = PeakVelocityProgress(e);
        const float err  = fabsf(peak - target);
        if (!any || err < bestErr)
        {
            any         = true;
            bestErr     = err;
            bestBalance = b;
            bestPeak    = peak;
        }
    }

    if (!any) return false;

    outEaseInPct  = total * bestBalance;
    outEaseOutPct = total * (1.0f - bestBalance);
    outPeak       = bestPeak;
    return true;
}

/** @brief Whether a stage actually changes anything between its two ends.
 *
 * Distinct from IsNoOp, which asks whether a stage is *neutral*. A stage held at
 * a constant 1.5x scale is far from neutral but does not move, so plotting a
 * velocity for it would be meaningless -- the easing curve still has a steepest
 * point, but nothing happens there.
 */
inline bool StageMoves(const Stage& s)
{
    const float kEps = 1e-5f;
    const auto differs = [kEps](float a, float b) { return fabsf(a - b) > kEps; };

    if (differs(s.scaleFrom, s.scaleTo))     return true;
    if (!s.linkScale && differs(s.scaleYFrom, s.scaleYTo)) return true;
    if (differs(s.posXFrom, s.posXTo))       return true;
    if (differs(s.posYFrom, s.posYTo))       return true;
    if (differs(s.rotFrom, s.rotTo))         return true;
    if (differs(s.tiltXFrom, s.tiltXTo))     return true;
    if (differs(s.swivelYFrom, s.swivelYTo)) return true;
    if (differs(s.opacityFrom, s.opacityTo)) return true;

    // A bent path moves even when the two endpoints coincide.
    if (fabsf(s.pathC1X) > kEps || fabsf(s.pathC1Y) > kEps) return true;
    if (fabsf(s.pathC2X) > kEps || fabsf(s.pathC2Y) > kEps) return true;

    return false;
}

inline bool IsNoOp(const AnimParams& a)
{
    const float kEps = 1e-5f;
    const auto near = [kEps](float v, float target) { return fabsf(v - target) <= kEps; };

    // A base pose that does anything is enough on its own.
    if (!a.base.IsNeutral()) return false;

    const int count = a.stageCount < kMaxStages ? a.stageCount : kMaxStages;
    for (int i = 0; i < count; ++i)
    {
        const Stage& s = a.stages[i];
        if (!s.enabled) continue;

        // Every channel that can move a pixel has to be able to defeat this, or
        // a stage that only tilts would report itself as doing nothing and be
        // skipped outright.
        if (!near(s.tiltXFrom, 0.0f)   || !near(s.tiltXTo, 0.0f))   return false;
        if (!near(s.swivelYFrom, 0.0f) || !near(s.swivelYTo, 0.0f)) return false;
        if (!s.linkScale)
        {
            if (!near(s.scaleYFrom, 1.0f) || !near(s.scaleYTo, 1.0f)) return false;
        }

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
