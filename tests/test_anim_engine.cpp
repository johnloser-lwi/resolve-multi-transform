// Unit tests for the animation engine and transform math.
//
// No test framework on purpose: zero dependencies keeps the build trivial, and
// there is not enough here to justify pulling in gtest.

#include "AnimEngine.h"
#include "ClipRange.h"
#include "MotionBlur.h"
#include "TransformMath.h"

#include <cmath>
#include <limits>
#include <cstdio>
#include <string>

using namespace mtx;

static int g_failures = 0;
static int g_checks   = 0;

static void Check(bool cond, const std::string& what)
{
    ++g_checks;
    if (!cond)
    {
        ++g_failures;
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

static void CheckNear(float actual, float expected, float tol, const std::string& what)
{
    ++g_checks;
    if (!(std::fabs(actual - expected) <= tol))
    {
        ++g_failures;
        std::printf("  FAIL: %s (got %.6f, expected %.6f, tol %.6f)\n",
                    what.c_str(), actual, expected, tol);
    }
}

////////////////////////////////////////////////////////////////////////////////

static void TestEasingEndpoints()
{
    std::printf("Easing endpoints\n");
    const Easing curves[] = { Easing::Linear(), Easing::Smooth(),
                              Easing::EaseIn(), Easing::EaseOut() };
    for (int i = 0; i < 4; ++i)
    {
        CheckNear(ApplyEasing(0.0f, curves[i]), 0.0f, 1e-5f, "easing at p=0 is 0");
        CheckNear(ApplyEasing(1.0f, curves[i]), 1.0f, 1e-5f, "easing at p=1 is 1");
    }

    // Out-of-range progress must clamp, not extrapolate.
    CheckNear(ApplyEasing(-0.5f, Easing::Smooth()), 0.0f, 1e-5f, "easing clamps below 0");
    CheckNear(ApplyEasing( 1.5f, Easing::Smooth()), 1.0f, 1e-5f, "easing clamps above 1");
}

static void TestEasingLinearIsIdentity()
{
    std::printf("Linear easing is identity\n");
    for (int i = 0; i <= 10; ++i)
    {
        const float p = i / 10.0f;
        CheckNear(ApplyEasing(p, Easing::Linear()), p, 1e-5f, "linear easing passes through");
    }
}

static void TestEasingGoldenValues()
{
    std::printf("Easing golden values\n");

    // ease-in-out, cubic-bezier(0.42, 0, 0.58, 1): symmetric, so p=0.5 -> 0.5.
    CheckNear(ApplyEasing(0.5f, Easing::Smooth()), 0.5f, 1e-4f, "ease-in-out midpoint");

    // Reference values computed from the same bezier definition browsers use.
    CheckNear(ApplyEasing(0.25f, Easing::Smooth()), 0.1290f, 2e-3f, "ease-in-out at 0.25");
    CheckNear(ApplyEasing(0.75f, Easing::Smooth()), 0.8710f, 2e-3f, "ease-in-out at 0.75");

    // ease-in lags the linear ramp; ease-out leads it.
    Check(ApplyEasing(0.5f, Easing::EaseIn())  < 0.5f, "ease-in is below linear at midpoint");
    Check(ApplyEasing(0.5f, Easing::EaseOut()) > 0.5f, "ease-out is above linear at midpoint");
}

static void TestEasingAmountMapping()
{
    std::printf("Ease In/Out amounts map onto the standard curves\n");

    // The four human-facing amounts must reproduce the named curves exactly,
    // otherwise "Smooth" in the UI would not be CSS ease-in-out.
    const Easing linear = MakeEasing(0.0f, 0.0f, 0.0f, 0.0f);
    CheckNear(linear.x1, 0.0f, 1e-5f, "linear x1");
    CheckNear(linear.y1, 0.0f, 1e-5f, "linear y1");
    CheckNear(linear.x2, 1.0f, 1e-5f, "linear x2");
    CheckNear(linear.y2, 1.0f, 1e-5f, "linear y2");

    const Easing smooth = MakeEasing(42.0f, 42.0f, 0.0f, 0.0f);
    CheckNear(smooth.x1, 0.42f, 1e-5f, "smooth x1 matches CSS ease-in-out");
    CheckNear(smooth.x2, 0.58f, 1e-5f, "smooth x2 matches CSS ease-in-out");

    const Easing easeIn = MakeEasing(42.0f, 0.0f, 0.0f, 0.0f);
    CheckNear(easeIn.x1, 0.42f, 1e-5f, "ease-in x1");
    CheckNear(easeIn.x2, 1.0f,  1e-5f, "ease-in x2");

    const Easing easeOut = MakeEasing(0.0f, 42.0f, 0.0f, 0.0f);
    CheckNear(easeOut.x1, 0.0f,  1e-5f, "ease-out x1");
    CheckNear(easeOut.x2, 0.58f, 1e-5f, "ease-out x2");

    // 100% anticipation/overshoot should land on the classic "back" handles.
    const Easing back = MakeEasing(0.0f, 0.0f, 100.0f, 100.0f);
    CheckNear(back.y1, -0.55f, 1e-5f, "full anticipation y1");
    CheckNear(back.y2,  1.55f, 1e-5f, "full overshoot y2");

    // More easing must mean a slower departure: at an early progress value, a
    // heavily eased-in curve has travelled less than a lightly eased one.
    const float light = ApplyEasing(0.2f, MakeEasing(10.0f, 0.0f, 0.0f, 0.0f));
    const float heavy = ApplyEasing(0.2f, MakeEasing(90.0f, 0.0f, 0.0f, 0.0f));
    Check(heavy < light, "more Ease In means a slower start");

    // Overshoot must actually exceed the target somewhere in the middle.
    bool exceeded = false;
    const Easing over = MakeEasing(0.0f, 32.0f, 0.0f, 80.0f);
    for (int i = 1; i < 100; ++i)
        if (ApplyEasing(i / 100.0f, over) > 1.0f) { exceeded = true; break; }
    Check(exceeded, "overshoot curve passes beyond 1.0");
}

static void TestEasingMonotonic()
{
    std::printf("Easing monotonicity\n");
    const Easing curves[] = { Easing::Linear(), Easing::Smooth(),
                              Easing::EaseIn(), Easing::EaseOut() };
    for (int c = 0; c < 4; ++c)
    {
        float prev = -1.0f;
        bool  ok   = true;
        for (int i = 0; i <= 200; ++i)
        {
            const float v = ApplyEasing(i / 200.0f, curves[c]);
            if (v < prev - 1e-4f) { ok = false; break; }
            prev = v;
        }
        Check(ok, "standard easing curve is monotonic");
    }
}

static void TestStageProgressAndStagger()
{
    std::printf("Per-stage timing and stagger\n");

    AnimParams a = AnimParams::Default();
    a.stageCount = 2;

    a.stages[0].enabled    = true;
    a.stages[0].easing     = Easing::Linear();
    a.stages[0].startFrame = 100.0f;
    a.stages[0].endFrame   = 120.0f;

    // Staggering is not its own control: it is simply a later start frame.
    a.stages[1].enabled    = true;
    a.stages[1].easing     = Easing::Linear();
    a.stages[1].startFrame = 105.0f;
    a.stages[1].endFrame   = 125.0f;

    // Before the start nothing has moved; after the end everything has.
    CheckNear(StageProgress(a.stages[0], 99.0f),  0.0f, 1e-5f, "stage 0 idle before start");
    CheckNear(StageProgress(a.stages[0], 100.0f), 0.0f, 1e-5f, "stage 0 at start");
    CheckNear(StageProgress(a.stages[0], 110.0f), 0.5f, 1e-5f, "stage 0 halfway");
    CheckNear(StageProgress(a.stages[0], 120.0f), 1.0f, 1e-5f, "stage 0 complete");
    CheckNear(StageProgress(a.stages[0], 999.0f), 1.0f, 1e-5f, "stage 0 stays complete");

    CheckNear(StageProgress(a.stages[1], 105.0f), 0.0f,  1e-5f, "stage 1 starts 5 frames later");
    CheckNear(StageProgress(a.stages[1], 115.0f), 0.5f,  1e-5f, "stage 1 halfway, shifted");
    CheckNear(StageProgress(a.stages[1], 125.0f), 1.0f,  1e-5f, "stage 1 completes 5 frames later");
    CheckNear(StageProgress(a.stages[1], 110.0f), 0.25f, 1e-5f, "stage 1 lags stage 0 mid-anim");
}

static void TestIndependentStageLengths()
{
    std::printf("Stages may differ in length\n");
    // The point of per-stage end frames: a short snap overlapping a long drift,
    // which a single shared duration could not express.
    Stage shortStage = Stage::Default();
    shortStage.easing     = Easing::Linear();
    shortStage.startFrame = 100.0f;
    shortStage.endFrame   = 106.0f;    // 6 frames

    Stage longStage = Stage::Default();
    longStage.easing     = Easing::Linear();
    longStage.startFrame = 100.0f;
    longStage.endFrame   = 140.0f;     // 40 frames

    CheckNear(shortStage.Duration(), 6.0f,  1e-5f, "short stage duration derived");
    CheckNear(longStage.Duration(),  40.0f, 1e-5f, "long stage duration derived");

    CheckNear(StageProgress(shortStage, 103.0f), 0.5f,  1e-5f, "short stage halfway at 103");
    CheckNear(StageProgress(longStage,  103.0f), 0.075f, 1e-5f, "long stage barely started at 103");
    CheckNear(StageProgress(shortStage, 120.0f), 1.0f,  1e-5f, "short stage long finished");
    CheckNear(StageProgress(longStage,  120.0f), 0.5f,  1e-5f, "long stage halfway at 120");
}

static void TestZeroAndInvertedDuration()
{
    std::printf("Zero and inverted durations degrade safely\n");

    Stage zero = Stage::Default();
    zero.startFrame = 50.0f;
    zero.endFrame   = 50.0f;
    const float before = StageProgress(zero, 49.0f);
    const float after  = StageProgress(zero, 51.0f);
    CheckNear(before, 0.0f, 1e-5f, "zero duration: 0 before start");
    CheckNear(after,  1.0f, 1e-5f, "zero duration: 1 after start");
    Check(!std::isnan(before) && !std::isnan(after), "zero duration produces no NaN");

    // End before start is a user error that must not animate backwards or blow up.
    Stage inverted = Stage::Default();
    inverted.startFrame = 100.0f;
    inverted.endFrame   = 80.0f;
    const float invBefore = StageProgress(inverted, 90.0f);
    const float invAfter  = StageProgress(inverted, 110.0f);
    Check(!std::isnan(invBefore) && !std::isnan(invAfter), "inverted range produces no NaN");
    CheckNear(invBefore, 0.0f, 1e-5f, "inverted range: 0 before start frame");
    CheckNear(invAfter,  1.0f, 1e-5f, "inverted range: 1 after start frame");
}

static void TestMatrixIdentityAndInverse()
{
    std::printf("Matrix identity and inverse\n");

    const Mat3 id = Mat3::Identity();
    float x, y;
    id.Apply(12.0f, 34.0f, x, y);
    CheckNear(x, 12.0f, 1e-5f, "identity preserves x");
    CheckNear(y, 34.0f, 1e-5f, "identity preserves y");

    const Mat3 t   = MakeTransform(960.0f, 540.0f, 2.0f, 2.0f, 37.0f, 100.0f, -50.0f);
    const Mat3 inv = Invert(t);

    // Round-tripping a point through T then T-inverse must return it.
    float tx, ty, rx, ry;
    t.Apply(123.0f, 456.0f, tx, ty);
    inv.Apply(tx, ty, rx, ry);
    CheckNear(rx, 123.0f, 1e-2f, "inverse round-trips x");
    CheckNear(ry, 456.0f, 1e-2f, "inverse round-trips y");
}

static void TestDegenerateInverse()
{
    std::printf("Degenerate matrix inverse\n");
    // Scale 0 is not invertible. We must return identity rather than NaN/inf,
    // so a zero-scale keyframe renders an untransformed frame instead of garbage.
    const Mat3 zero = MakeTransform(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    const Mat3 inv  = Invert(zero);
    float x, y;
    inv.Apply(10.0f, 20.0f, x, y);
    Check(!std::isnan(x) && !std::isinf(x), "degenerate inverse x is finite");
    Check(!std::isnan(y) && !std::isinf(y), "degenerate inverse y is finite");
}

static void TestAnchorIsFixedPoint()
{
    std::printf("Anchor point is the fixed point\n");
    // Scaling and rotating about an anchor must leave the anchor where it is --
    // this is what "anchor point" means, and getting it wrong is the classic
    // transform bug where the image drifts as you scale.
    const float ax = 640.0f, ay = 360.0f;
    const Mat3 t = MakeTransform(ax, ay, 3.0f, 3.0f, 45.0f, 0.0f, 0.0f);
    float x, y;
    t.Apply(ax, ay, x, y);
    CheckNear(x, ax, 1e-2f, "anchor x is unmoved by scale+rotate");
    CheckNear(y, ay, 1e-2f, "anchor y is unmoved by scale+rotate");
}

static void TestRotationDirection()
{
    std::printf("Rotation direction and magnitude\n");
    // +90 degrees about the origin takes (1,0) to (0,1): counter-clockwise.
    const Mat3 r = Mat3::Rotate(90.0f);
    float x, y;
    r.Apply(1.0f, 0.0f, x, y);
    CheckNear(x, 0.0f, 1e-5f, "rot90 x");
    CheckNear(y, 1.0f, 1e-5f, "rot90 y");

    // 360 degrees is a no-op.
    const Mat3 full = Mat3::Rotate(360.0f);
    full.Apply(5.0f, -7.0f, x, y);
    CheckNear(x,  5.0f, 1e-4f, "rot360 x");
    CheckNear(y, -7.0f, 1e-4f, "rot360 y");
}

static void TestCompositionOrderAndDisabledStages()
{
    std::printf("Stage composition\n");
    const float W = 1920.0f, H = 1080.0f;

    AnimParams a = AnimParams::Default();
    a.stageCount = 2;

    // Two stages each scaling 2x at completion should compose to 4x.
    for (int i = 0; i < 2; ++i)
    {
        a.stages[i].enabled    = true;
        a.stages[i].easing     = Easing::Linear();
        a.stages[i].startFrame = 0.0f;
        a.stages[i].endFrame   = 10.0f;
        a.stages[i].scaleFrom  = 1.0f;
        a.stages[i].scaleTo    = 2.0f;
        a.stages[i].anchorX    = 0.0f;
        a.stages[i].anchorY    = 0.0f;
    }

    const Mat3 m = EvaluateTransform(a, 10.0f, W, H);
    float x, y;
    m.Apply(1.0f, 1.0f, x, y);
    CheckNear(x, 4.0f, 1e-3f, "two 2x stages compose to 4x in x");
    CheckNear(y, 4.0f, 1e-3f, "two 2x stages compose to 4x in y");

    // Disabling a stage must remove its contribution entirely.
    a.stages[1].enabled = false;
    const Mat3 m1 = EvaluateTransform(a, 10.0f, W, H);
    m1.Apply(1.0f, 1.0f, x, y);
    CheckNear(x, 2.0f, 1e-3f, "disabled stage contributes nothing");

    // stageCount must gate stages even when they are flagged enabled.
    a.stages[1].enabled = true;
    a.stageCount = 1;
    const Mat3 m2 = EvaluateTransform(a, 10.0f, W, H);
    m2.Apply(1.0f, 1.0f, x, y);
    CheckNear(x, 2.0f, 1e-3f, "stageCount gates surplus stages");
}

static void TestIdentityWhenNothingAnimates()
{
    std::printf("No-op animation is identity\n");
    AnimParams a = AnimParams::Default();   // defaults are all from==to
    const Mat3 m = EvaluateTransform(a, 37.0f, 1920.0f, 1080.0f);
    float x, y;
    m.Apply(800.0f, 450.0f, x, y);
    CheckNear(x, 800.0f, 1e-3f, "default params leave x unchanged");
    CheckNear(y, 450.0f, 1e-3f, "default params leave y unchanged");
}

static void TestSubFrameContinuity()
{
    std::printf("Sub-frame continuity (motion blur prerequisite)\n");
    // Motion blur evaluates the transform at fractional times. The result must
    // vary smoothly, with no jump between adjacent sub-frame samples.
    AnimParams a = AnimParams::Default();
    a.stages[0].enabled    = true;
    a.stages[0].startFrame = 0.0f;
    a.stages[0].endFrame   = 10.0f;
    a.stages[0].scaleFrom  = 1.0f;
    a.stages[0].scaleTo    = 2.0f;

    float prevX = 0.0f;
    bool  first = true, smooth = true;
    for (float t = 0.0f; t <= 10.0f; t += 0.05f)
    {
        const Mat3 m = EvaluateTransform(a, t, 1920.0f, 1080.0f);
        float x, y;
        m.Apply(1920.0f, 1080.0f, x, y);
        if (!first && std::fabs(x - prevX) > 20.0f) { smooth = false; break; }
        prevX = x;
        first = false;
    }
    Check(smooth, "transform is continuous across sub-frame samples");
}

////////////////////////////////////////////////////////////////////////////////
// Bounce

static Easing SpringEasing(float amount = 45.0f, float count = 3.0f, float damping = 55.0f)
{
    return MakeEasing(0.0f, 20.0f, 0.0f, 0.0f, kBounceSpring, amount, count, damping);
}

static Easing BallEasing(float amount = 70.0f, float count = 4.0f, float damping = 60.0f)
{
    return MakeEasing(30.0f, 0.0f, 0.0f, 0.0f, kBounceBall, amount, count, damping);
}

static void TestBounceEndpointsExact()
{
    std::printf("Bounce endpoints stay exact\n");

    // The entire formulation is built so the endpoints hold for ANY parameters.
    // If this ever fails, animations stop landing on their target values --
    // which is far worse than the bounce looking wrong.
    const float amounts[]  = { -100.0f, -45.0f, 0.0f, 25.0f, 60.0f, 100.0f };
    const float counts[]   = { 0.0f, 1.0f, 3.5f, 8.0f, 12.0f };
    const float dampings[] = { 0.0f, 30.0f, 100.0f };

    bool ok = true;
    for (float a : amounts)
        for (float n : counts)
            for (float d : dampings)
                for (int type = kBounceSpring; type <= kBounceBall; ++type)
                {
                    const Easing e = MakeEasing(20.0f, 30.0f, 0.0f, 0.0f, type, a, n, d);
                    if (std::fabs(ApplyEasing(0.0f, e) - 0.0f) > 1e-5f) ok = false;
                    if (std::fabs(ApplyEasing(1.0f, e) - 1.0f) > 1e-5f) ok = false;

                    // Just inside the endpoints too, where the analytic form is
                    // actually exercised rather than short-circuited.
                    if (!std::isfinite(ApplyEasing(0.001f, e))) ok = false;
                    if (!std::isfinite(ApplyEasing(0.999f, e))) ok = false;
                }
    Check(ok, "endpoints exact and finite across the whole bounce parameter sweep");

    // Approaching 1 must converge to 1, not merely be finite: the residual
    // (1 - base) is what forces this.
    const Easing spring = SpringEasing(100.0f, 8.0f, 0.0f);   // worst case: no damping
    CheckNear(ApplyEasing(0.9999f, spring), 1.0f, 1e-2f, "undamped spring still converges to 1");
}

static void TestBounceNoneIsUnchanged()
{
    std::printf("Bounce type None leaves the bezier untouched\n");

    // Not "close to" the old behaviour -- identical, so existing projects and
    // the four standard presets are unaffected.
    const Easing plain  = MakeEasing(42.0f, 42.0f, 0.0f, 0.0f);
    const Easing withNone = MakeEasing(42.0f, 42.0f, 0.0f, 0.0f, kBounceNone, 80.0f, 5.0f, 20.0f);

    bool identical = true;
    for (int i = 0; i <= 100; ++i)
    {
        const float p = i / 100.0f;
        if (ApplyEasing(p, plain) != ApplyEasing(p, withNone)) { identical = false; break; }
    }
    Check(identical, "None ignores the bounce amounts entirely, bit for bit");

    // Zero amount is likewise a no-op even with a type selected.
    const Easing zeroAmount = MakeEasing(42.0f, 42.0f, 0.0f, 0.0f, kBounceSpring, 0.0f, 5.0f, 20.0f);
    CheckNear(ApplyEasing(0.5f, zeroAmount), ApplyEasing(0.5f, plain), 1e-6f,
              "zero bounce amount is a no-op");
}

static void TestBounceHappensAtTheEnd()
{
    std::printf("Bounce happens at the END of the curve\n");

    // This is the property the first implementation got exactly backwards: it
    // scaled the oscillation by the distance still to travel, which put the
    // biggest wobble at p=0 -- before the move had gone anywhere -- and left
    // nothing at the end. A bounce is what happens *after* something lands.
    const auto swingIn = [](const Easing& e, double lo, double hi)
    {
        float worst = 0.0f;
        const int i0 = static_cast<int>(lo * 1000.0);
        const int i1 = static_cast<int>(hi * 1000.0);
        for (int i = i0; i < i1; ++i)
        {
            const float y = ApplyEasing(i / 1000.0f, e);
            const float d = std::fabs(y - 1.0f);   // distance from the target
            if (d > worst) worst = d;
        }
        return worst;
    };

    for (int type = kBounceSpring; type <= kBounceBall; ++type)
    {
        const Easing e = MakeEasing(0.0f, 25.0f, 0.0f, 0.0f, type, 50.0f, 3.0f, 40.0f, 55.0f);

        // Once past the arrival point the curve must still be moving around the
        // target, rather than having already gone quiet.
        const float lateSwing = swingIn(e, 0.60, 0.95);
        Check(lateSwing > 0.02f, "curve is still oscillating late in the stage");

        // And it must actually reach the target near the arrival point, instead
        // of wobbling its way there from the very beginning.
        const float atArrival = ApplyEasing(0.55f, e);
        CheckNear(atArrival, 1.0f, 1e-3f, "move lands on the target at Bounce Start");
    }

    // Bounce Start genuinely moves the landing: an earlier start means the
    // curve has arrived by a point where a later start has not.
    const Easing early = MakeEasing(0.0f, 25.0f, 0.0f, 0.0f, kBounceSpring, 50.0f, 3.0f, 40.0f, 25.0f);
    const Easing late  = MakeEasing(0.0f, 25.0f, 0.0f, 0.0f, kBounceSpring, 50.0f, 3.0f, 40.0f, 80.0f);
    CheckNear(ApplyEasing(0.25f, early), 1.0f, 1e-3f, "early Bounce Start lands at 25%");
    Check(ApplyEasing(0.25f, late) < 0.9f, "late Bounce Start has not landed at 25%");
}

static void TestApproachKeepsItsShape()
{
    std::printf("Bounce leaves the approach curve's shape intact\n");

    const float S = 0.55f;

    // Turning a bounce on must not redraw the move that leads into it. An
    // earlier attempt forced the approach to arrive at speed, which flattened
    // every curve into the same accelerating ramp and threw Ease Out away.
    for (int type = kBounceSpring; type <= kBounceBall; ++type)
    {
        const Easing bouncing = MakeEasing(30.0f, 70.0f, 0.0f, 0.0f,
                                           type, 50.0f, 3.0f, 40.0f, S * 100.0f);
        const Easing plain    = MakeEasing(30.0f, 70.0f, 0.0f, 0.0f);

        bool matches = true;
        for (int i = 1; i < 54; ++i)
        {
            const float p = i / 100.0f;                 // strictly inside [0, S)
            const float withBounce = ApplyEasing(p, bouncing);
            const float compressed = ApplyEasing(p / S, plain);
            if (std::fabs(withBounce - compressed) > 1e-4f) { matches = false; break; }
        }
        Check(matches, "approach is exactly the chosen curve, just time-compressed");
    }

    // Ease Out must still visibly do something: a heavily eased-out approach
    // decelerates into the landing, a linear one does not.
    const auto arrivalSlope = [&](float easeOut)
    {
        const Easing e = MakeEasing(0.0f, easeOut, 0.0f, 0.0f,
                                    kBounceSpring, 50.0f, 3.0f, 40.0f, S * 100.0f);
        return ApplyEasing(S - 0.005f, e) - ApplyEasing(S - 0.030f, e);
    };
    Check(arrivalSlope(90.0f) < arrivalSlope(0.0f),
          "Ease Out still decelerates the approach when bouncing");
}

static void TestNoDoubleOvershoot()
{
    std::printf("Overshoot does not add a second bump before the bounce\n");

    const float S = 0.55f;

    // The original defect: with Overshoot set, the bezier carried past the
    // target inside the approach, was forced back down to exactly 1 at the
    // join, and only then did the spring push past again. That downward leg in
    // the middle is what read as the first rebound going the wrong way.
    const Easing both = MakeEasing(30.0f, 40.0f, 0.0f, 90.0f,
                                   kBounceSpring, 50.0f, 3.0f, 40.0f, S * 100.0f);

    // Nothing may cross above the target before the landing point.
    bool earlyOvershoot = false;
    for (int i = 1; i < static_cast<int>(S * 1000.0f); ++i)
        if (ApplyEasing(i / 1000.0f, both) > 1.0f + 1e-4f) { earlyOvershoot = true; break; }
    Check(!earlyOvershoot, "no overshoot inside the approach, so no reversal at the join");

    // And the approach must be monotonic: no rise-then-fall before landing.
    bool monotonic = true;
    float prev = 0.0f;
    for (int i = 1; i < static_cast<int>(S * 1000.0f); ++i)
    {
        const float y = ApplyEasing(i / 1000.0f, both);
        if (y < prev - 1e-4f) { monotonic = false; break; }
        prev = y;
    }
    Check(monotonic, "approach rises steadily into the landing");

    // The first excursion past the target is the spring's, and it is upward.
    Check(ApplyEasing(S + 0.02f, both) > 1.0f,
          "first move past the target is the rebound, in the direction of travel");
}

static void TestSpringCrossesTarget()
{
    std::printf("Spring bounce crosses the target\n");

    const Easing e = SpringEasing();

    bool above = false, below = false;
    for (int i = 1; i < 400; ++i)
    {
        const float y = ApplyEasing(i / 400.0f, e);
        if (y > 1.0005f) above = true;
        // "Below" means dipping back under the target after having passed it.
        if (above && y < 0.9995f) below = true;
    }
    Check(above, "spring overshoots past the target");
    Check(below, "spring settles back under the target after overshooting");
}

static void TestBallNeverPassesTarget()
{
    std::printf("Ball bounce rebounds without passing the target\n");

    const Easing e = BallEasing();

    bool exceeded = false;
    int  touches  = 0;
    bool wasAway  = true;
    for (int i = 1; i < 1000; ++i)
    {
        const float y = ApplyEasing(i / 1000.0f, e);

        // The defining property: |cos| is never negative, so the curve can
        // approach the target but never pass through it.
        if (y > 1.0f + 1e-4f) exceeded = true;

        // A "touch" is the curve reaching the target; the clamp in ApplyBounce
        // makes these exact rather than merely close.
        if (y > 0.9995f) { if (wasAway) { ++touches; wasAway = false; } }
        else             { wasAway = true; }
    }
    Check(!exceeded, "ball bounce never exceeds the target");
    Check(touches >= 2, "ball bounce touches the target repeatedly");
}

static void TestNegativeBounceMirrorsDirection()
{
    std::printf("Negative bounce amount flips the rebound direction\n");

    const float S = 0.55f;

    // Spring: positive overshoots first, negative undershoots first. Same shape,
    // mirrored about the target.
    const Easing springPos = MakeEasing(0.0f, 30.0f, 0.0f, 0.0f,
                                        kBounceSpring, 50.0f, 3.0f, 40.0f, S * 100.0f);
    const Easing springNeg = MakeEasing(0.0f, 30.0f, 0.0f, 0.0f,
                                        kBounceSpring, -50.0f, 3.0f, 40.0f, S * 100.0f);

    Check(ApplyEasing(S + 0.02f, springPos) > 1.0f, "positive spring overshoots first");
    Check(ApplyEasing(S + 0.02f, springNeg) < 1.0f, "negative spring undershoots first");

    // The two must be exact mirror images about the target in the bounce region.
    bool mirrored = true;
    for (int i = static_cast<int>(S * 1000.0f) + 1; i < 1000; ++i)
    {
        const float p    = i / 1000.0f;
        const float pos  = ApplyEasing(p, springPos);
        const float neg  = ApplyEasing(p, springNeg);
        if (std::fabs((pos - 1.0f) + (neg - 1.0f)) > 1e-4f) { mirrored = false; break; }
    }
    Check(mirrored, "negative spring is the exact mirror of the positive one");

    // Ball: positive stays on one side of the target, negative on the other.
    const Easing ballPos = MakeEasing(20.0f, 0.0f, 0.0f, 0.0f,
                                      kBounceBall, 55.0f, 4.0f, 35.0f, S * 100.0f);
    const Easing ballNeg = MakeEasing(20.0f, 0.0f, 0.0f, 0.0f,
                                      kBounceBall, -55.0f, 4.0f, 35.0f, S * 100.0f);

    bool posStaysBelow = true, negStaysAbove = true;
    for (int i = static_cast<int>(S * 1000.0f); i <= 1000; ++i)
    {
        const float p = i / 1000.0f;
        if (ApplyEasing(p, ballPos) > 1.0f + 1e-4f) posStaysBelow = false;
        if (ApplyEasing(p, ballNeg) < 1.0f - 1e-4f) negStaysAbove = false;
    }
    Check(posStaysBelow, "positive ball never crosses above the target");
    Check(negStaysAbove, "negative ball never crosses below the target");

    // Flipping the sign must not disturb the approach, only the bounce.
    bool approachSame = true;
    for (int i = 1; i < static_cast<int>(S * 1000.0f); ++i)
    {
        const float p = i / 1000.0f;
        if (std::fabs(ApplyEasing(p, springPos) - ApplyEasing(p, springNeg)) > 1e-5f)
        { approachSame = false; break; }
    }
    Check(approachSame, "sign of the bounce does not change the approach");
}

static void TestBounceDampingReducesLateMotion()
{
    std::printf("Bounce damping shrinks the later rebounds\n");

    // Measure how far the curve strays from the base near the end of the move.
    const auto lateSwing = [](const Easing& e)
    {
        float worst = 0.0f;
        for (int i = 600; i < 950; ++i)
        {
            const float y = ApplyEasing(i / 1000.0f, e);
            const float d = std::fabs(y - 1.0f);
            if (d > worst) worst = d;
        }
        return worst;
    };

    const float light = lateSwing(SpringEasing(60.0f, 4.0f, 10.0f));
    const float heavy = lateSwing(SpringEasing(60.0f, 4.0f, 95.0f));
    Check(heavy < light, "more damping means less movement late in the curve");

    // Undamped rebounds should stay roughly even rather than decaying.
    const float undamped = lateSwing(SpringEasing(60.0f, 4.0f, 0.0f));
    Check(undamped > heavy, "zero damping sustains the oscillation");
}

static void TestBounceCountControlsRebounds()
{
    std::printf("Bounce count controls how many rebounds occur\n");

    const auto countPeaks = [](const Easing& e)
    {
        int peaks = 0;
        float prev = ApplyEasing(0.0f, e);
        float cur  = ApplyEasing(0.001f, e);
        for (int i = 2; i <= 1000; ++i)
        {
            const float next = ApplyEasing(i / 1000.0f, e);
            if (cur > prev && cur >= next) ++peaks;
            prev = cur;
            cur  = next;
        }
        return peaks;
    };

    const int few  = countPeaks(SpringEasing(60.0f, 2.0f, 20.0f));
    const int many = countPeaks(SpringEasing(60.0f, 6.0f, 20.0f));
    Check(many > few, "a higher bounce count produces more rebounds");

    // Zero bounces must degrade to no oscillation rather than dividing by zero
    // or oscillating infinitely fast.
    const Easing none = SpringEasing(60.0f, 0.0f, 20.0f);
    const Easing base = MakeEasing(0.0f, 20.0f, 0.0f, 0.0f);
    CheckNear(ApplyEasing(0.5f, none), ApplyEasing(0.5f, base), 1e-5f,
              "zero bounces is a no-op");
}

////////////////////////////////////////////////////////////////////////////////
// Timing anchors

static AnimParams AnchoredAnim(int anchor, float start, float end)
{
    // A 155-frame clip sitting an hour into the timeline, matching the measured
    // real-world case.
    AnimParams a = AnimParams::Default();
    a.clipStart  = 107961.0f;
    a.clipLength = 155.0f;
    a.stages[0].enabled    = true;
    a.stages[0].easing     = Easing::Linear();
    a.stages[0].anchor     = anchor;
    a.stages[0].startFrame = start;
    a.stages[0].endFrame   = end;
    return a;
}

static void TestAnchorMappings()
{
    std::printf("Timing anchor mappings\n");

    // Clip time 0 is the first frame; 154 is the last of a 155-frame clip.
    const AnimParams start = AnchoredAnim(kAnchorClipStart, 0.0f, 20.0f);
    CheckNear(StageLocalTime(start, start.stages[0], 0.0f),   0.0f,   1e-4f, "clip-start: head is 0");
    CheckNear(StageLocalTime(start, start.stages[0], 154.0f), 154.0f, 1e-4f, "clip-start: tail is 154");

    // Zero must land on the clip's LAST frame, which is length - 1. Using the
    // length itself would put it one frame past the end, and an outro would
    // finish fractionally short on the final visible frame.
    const AnimParams end = AnchoredAnim(kAnchorClipEnd, -20.0f, 0.0f);
    CheckNear(StageLocalTime(end, end.stages[0], 154.0f),   0.0f,   1e-4f, "clip-end: tail is 0");
    CheckNear(StageLocalTime(end, end.stages[0], 134.0f), -20.0f,   1e-4f, "clip-end: counts backwards");

    const AnimParams abs = AnchoredAnim(kAnchorTimeline, 107981.0f, 108001.0f);
    CheckNear(StageLocalTime(abs, abs.stages[0], 20.0f), 107981.0f, 1e-1f,
              "timeline: clip time converts back to absolute");

    // Stretch turns frames into percentages of the clip.
    const AnimParams pct = AnchoredAnim(kAnchorStretch, 0.0f, 100.0f);
    CheckNear(StageLocalTime(pct, pct.stages[0], 0.0f),     0.0f,  1e-3f, "stretch: head is 0%");
    CheckNear(StageLocalTime(pct, pct.stages[0], 77.0f),   50.0f,  1e-1f, "stretch: middle is 50%");
    CheckNear(StageLocalTime(pct, pct.stages[0], 154.0f), 100.0f,  1e-3f, "stretch: tail is 100%");

    // Degenerate clips must not divide by zero or count back from nothing.
    for (int anchor = kAnchorClipStart; anchor <= kAnchorStretch; ++anchor)
    {
        AnimParams tiny = AnchoredAnim(anchor, 0.0f, 10.0f);
        tiny.clipLength = 0.0f;
        const float v = StageLocalTime(tiny, tiny.stages[0], 5.0f);
        Check(v == v && v > -1e9f && v < 1e9f, "unknown clip length degrades to a finite value");
    }
}

static void TestStretchScalesWithTheClip()
{
    std::printf("Stretch anchor scales with clip length\n");

    // The point of Stretch: the same animation fills the same proportion of the
    // clip whatever its length, so trimming compresses the move instead of
    // leaving it running off the end.
    AnimParams longClip = AnchoredAnim(kAnchorStretch, 0.0f, 100.0f);
    longClip.clipLength = 155.0f;

    AnimParams shortClip = AnchoredAnim(kAnchorStretch, 0.0f, 100.0f);
    shortClip.clipLength = 41.0f;

    const Stage& sl = longClip.stages[0];
    const Stage& ss = shortClip.stages[0];

    // Head, midpoint and tail line up in both, despite very different lengths.
    CheckNear(StageProgress(sl, StageLocalTime(longClip,  sl, 0.0f)),   0.0f, 1e-3f, "long: 0 at head");
    CheckNear(StageProgress(ss, StageLocalTime(shortClip, ss, 0.0f)),   0.0f, 1e-3f, "short: 0 at head");

    CheckNear(StageProgress(sl, StageLocalTime(longClip,  sl, 77.0f)),  0.5f, 1e-2f, "long: half way");
    CheckNear(StageProgress(ss, StageLocalTime(shortClip, ss, 20.0f)),  0.5f, 1e-2f, "short: half way");

    CheckNear(StageProgress(sl, StageLocalTime(longClip,  sl, 154.0f)), 1.0f, 1e-3f, "long: done at tail");
    CheckNear(StageProgress(ss, StageLocalTime(shortClip, ss, 40.0f)),  1.0f, 1e-3f, "short: done at tail");

    // A partial span works the same way: 0-50% covers the first half.
    AnimParams half = AnchoredAnim(kAnchorStretch, 0.0f, 50.0f);
    half.clipLength = 101.0f;
    CheckNear(StageProgress(half.stages[0], StageLocalTime(half, half.stages[0], 50.0f)),
              1.0f, 1e-2f, "0-50% completes at the clip midpoint");
}

static void TestClipTimeRoundTrip()
{
    std::printf("Stage frames round-trip through clip time\n");

    // The overlay draws on a clip-time ruler but edits values in stage units,
    // so the two conversions have to be exact inverses or bars would drift from
    // where they are dragged.
    const int anchors[] = { kAnchorClipStart, kAnchorClipEnd, kAnchorTimeline, kAnchorStretch };
    const float frames[] = { -20.0f, 0.0f, 12.5f, 100.0f };

    for (int anchor : anchors)
    {
        const AnimParams a = AnchoredAnim(anchor, 0.0f, 20.0f);
        for (float f : frames)
        {
            const float clip = ClipTimeFromStageFrame(a, a.stages[0], f);
            const float back = StageLocalTime(a, a.stages[0], clip);
            CheckNear(back, f, 1e-2f, "stage frame survives the round trip");
        }
    }
}

static void TestOutroLandsOnTheLastFrame()
{
    std::printf("Clip-end anchored stage lands on the final frame\n");

    // The point of the end anchor: a fade-out that completes exactly on the
    // clip's last visible frame, whatever the clip's length.
    const AnimParams a = AnchoredAnim(kAnchorClipEnd, -20.0f, 0.0f);
    const Stage& s = a.stages[0];
    const float off = AnchorMap(a, s).offset;

    // Times below are clip-relative, so the last visible frame is 154.
    CheckNear(StageProgress(s, 154.0f - off), 1.0f, 1e-4f, "complete on the last frame");
    CheckNear(StageProgress(s, 144.0f - off), 0.5f, 1e-4f, "half way ten frames earlier");
    CheckNear(StageProgress(s, 134.0f - off), 0.0f, 1e-4f, "not started twenty frames earlier");
    CheckNear(StageProgress(s, 0.0f   - off), 0.0f, 1e-4f, "idle at the head of the clip");

    // Trimming the clip shorter must move the outro with it rather than leaving
    // it stranded mid-clip.
    AnimParams shorter = a;
    shorter.clipLength = 100.0f;
    const float offShort = AnchorMap(shorter, shorter.stages[0]).offset;
    CheckNear(StageProgress(shorter.stages[0], 99.0f - offShort), 1.0f, 1e-4f,
              "still completes on the last frame after a trim");
}

static void TestAnchorsCoexistInOneEffect()
{
    std::printf("Intro and outro anchors coexist in one effect\n");

    // The reason anchors are per stage: one effect holding an intro anchored to
    // the head and an outro anchored to the tail.
    AnimParams a = AnimParams::Default();
    a.clipStart  = 107961.0f;
    a.clipLength = 155.0f;
    a.stageCount = 2;

    a.stages[0].enabled    = true;
    a.stages[0].easing     = Easing::Linear();
    a.stages[0].anchor     = kAnchorClipStart;
    a.stages[0].startFrame = 0.0f;
    a.stages[0].endFrame   = 20.0f;
    a.stages[0].opacityFrom = 0.0f;
    a.stages[0].opacityTo   = 1.0f;

    a.stages[1].enabled    = true;
    a.stages[1].easing     = Easing::Linear();
    a.stages[1].anchor     = kAnchorClipEnd;
    a.stages[1].startFrame = -20.0f;
    a.stages[1].endFrame   = 0.0f;
    a.stages[1].opacityFrom = 1.0f;
    a.stages[1].opacityTo   = 0.0f;

    CheckNear(EvaluateOpacity(a, 0.0f),   0.0f, 1e-4f, "transparent on the first frame");
    CheckNear(EvaluateOpacity(a, 20.0f),  1.0f, 1e-4f, "opaque once the intro finishes");
    CheckNear(EvaluateOpacity(a, 80.0f),  1.0f, 1e-4f, "opaque through the middle");
    CheckNear(EvaluateOpacity(a, 144.0f), 0.5f, 1e-4f, "half faded ten frames from the end");
    CheckNear(EvaluateOpacity(a, 154.0f), 0.0f, 1e-4f, "transparent on the last frame");
}

////////////////////////////////////////////////////////////////////////////////
// Base pose, split scale, orthographic 3D

static void TestNeutralBaseChangesNothing()
{
    std::printf("A neutral base pose is exactly the identity\n");

    AnimParams a = AnimParams::Default();
    a.stages[0].enabled    = true;
    a.stages[0].easing     = Easing::Linear();
    a.stages[0].startFrame = 0.0f;
    a.stages[0].endFrame   = 20.0f;
    a.stages[0].scaleTo    = 1.5f;

    // Same setup, base explicitly defaulted: existing projects must be
    // bit-for-bit unaffected by the base transform existing at all.
    AnimParams withBase = a;
    withBase.base = BasePose::Default();

    bool identical = true;
    for (int i = 0; i <= 20; ++i)
    {
        const Mat3 m1 = EvaluateTransform(a,        static_cast<float>(i), 1920.0f, 1080.0f);
        const Mat3 m2 = EvaluateTransform(withBase, static_cast<float>(i), 1920.0f, 1080.0f);
        for (int k = 0; k < 6; ++k) if (m1.m[k] != m2.m[k]) identical = false;
    }
    Check(identical, "a default base leaves every frame untouched");
    Check(BasePose::Default().IsNeutral(), "the default base reports itself neutral");
}

static void TestBaseComposesInnermost()
{
    std::printf("Base pose composes underneath the animation\n");

    const float W = 1000.0f, H = 1000.0f;

    // A base at 50% with a stage that translates by 0.2 of the frame. The stage
    // must still move a full 0.2 of the FRAME, not 0.2 of the shrunken element:
    // composing the other way round would silently halve the distance every
    // existing animation travels the moment a base scale was set.
    AnimParams a = AnimParams::Default();
    a.base.scaleX = 0.5f; a.base.scaleY = 0.5f;
    a.base.anchorX = 0.0f; a.base.anchorY = 0.0f;

    a.stages[0].enabled    = true;
    a.stages[0].easing     = Easing::Linear();
    a.stages[0].startFrame = 0.0f;
    a.stages[0].endFrame   = 10.0f;
    a.stages[0].posXFrom   = 0.0f;
    a.stages[0].posXTo     = 0.2f;
    a.stages[0].anchorX    = 0.0f; a.stages[0].anchorY = 0.0f;

    const Mat3 start = EvaluateTransform(a, 0.0f,  W, H);
    const Mat3 end   = EvaluateTransform(a, 10.0f, W, H);

    float x0, y0, x1, y1;
    start.Apply(0.0f, 0.0f, x0, y0);
    end.Apply(0.0f, 0.0f, x1, y1);

    CheckNear(x1 - x0, 200.0f, 1e-2f, "the stage moves a full 0.2 of the frame, not of the base");

    // And the base scale is still in effect.
    float cx, cy;
    start.Apply(W, H, cx, cy);
    CheckNear(cx, 500.0f, 1e-2f, "base scale applies to the source geometry");
}

static void TestSplitScale()
{
    std::printf("Split scale X and Y\n");

    const float W = 1000.0f, H = 1000.0f;

    AnimParams a = AnimParams::Default();
    a.stages[0].enabled    = true;
    a.stages[0].easing     = Easing::Linear();
    a.stages[0].startFrame = 0.0f;
    a.stages[0].endFrame   = 10.0f;
    a.stages[0].anchorX    = 0.0f; a.stages[0].anchorY = 0.0f;
    a.stages[0].scaleFrom  = 2.0f; a.stages[0].scaleTo  = 2.0f;
    a.stages[0].scaleYFrom = 3.0f; a.stages[0].scaleYTo = 3.0f;

    // Linked: Y follows X, and the Y values are ignored entirely rather than
    // leaking through.
    a.stages[0].linkScale = true;
    float x, y;
    EvaluateTransform(a, 5.0f, W, H).Apply(100.0f, 100.0f, x, y);
    CheckNear(x, 200.0f, 1e-3f, "linked: x uses the X scale");
    CheckNear(y, 200.0f, 1e-3f, "linked: y also uses the X scale");

    a.stages[0].linkScale = false;
    EvaluateTransform(a, 5.0f, W, H).Apply(100.0f, 100.0f, x, y);
    CheckNear(x, 200.0f, 1e-3f, "unlinked: x uses the X scale");
    CheckNear(y, 300.0f, 1e-3f, "unlinked: y uses its own scale");

    // The same rule on the base pose.
    AnimParams b = AnimParams::Default();
    b.base.anchorX = 0.0f; b.base.anchorY = 0.0f;
    b.base.scaleX = 2.0f;  b.base.scaleY = 4.0f;

    b.base.linkScale = true;
    EvaluateTransform(b, 0.0f, W, H).Apply(100.0f, 100.0f, x, y);
    CheckNear(y, 200.0f, 1e-3f, "linked base: y follows x");

    b.base.linkScale = false;
    EvaluateTransform(b, 0.0f, W, H).Apply(100.0f, 100.0f, x, y);
    CheckNear(y, 400.0f, 1e-3f, "unlinked base: y is independent");
}

static void TestOrthographicRotation()
{
    std::printf("Orthographic tilt and swivel\n");

    float sx = 0.0f, sy = 0.0f;

    OrthographicScale(0.0f, 0.0f, sx, sy);
    CheckNear(sx, 1.0f, 1e-5f, "no rotation leaves x alone");
    CheckNear(sy, 1.0f, 1e-5f, "no rotation leaves y alone");

    // A swivel squashes horizontally only; a tilt vertically only.
    OrthographicScale(0.0f, 60.0f, sx, sy);
    CheckNear(sx, 0.5f, 1e-4f, "60 degrees of swivel halves the width");
    CheckNear(sy, 1.0f, 1e-5f, "swivel leaves the height alone");

    OrthographicScale(60.0f, 0.0f, sx, sy);
    CheckNear(sx, 1.0f, 1e-5f, "tilt leaves the width alone");
    CheckNear(sy, 0.5f, 1e-4f, "60 degrees of tilt halves the height");

    // Edge-on collapses to nothing.
    OrthographicScale(0.0f, 90.0f, sx, sy);
    CheckNear(sx, 0.0f, 1e-6f, "90 degrees is edge-on");

    // Past edge-on the sign flips, which mirrors the image -- the correct
    // reading of a card turned past 90.
    OrthographicScale(0.0f, 180.0f, sx, sy);
    CheckNear(sx, -1.0f, 1e-5f, "180 degrees mirrors rather than clamping");

    // And it reaches the transform.
    AnimParams a = AnimParams::Default();
    a.stages[0].enabled     = true;
    a.stages[0].easing      = Easing::Linear();
    a.stages[0].startFrame  = 0.0f;
    a.stages[0].endFrame    = 10.0f;
    a.stages[0].anchorX     = 0.0f; a.stages[0].anchorY = 0.0f;
    a.stages[0].swivelYFrom = 60.0f; a.stages[0].swivelYTo = 60.0f;

    float x, y;
    EvaluateTransform(a, 5.0f, 1000.0f, 1000.0f).Apply(100.0f, 100.0f, x, y);
    CheckNear(x, 50.0f,  1e-2f, "swivel narrows the transform");
    CheckNear(y, 100.0f, 1e-2f, "swivel leaves the height untouched");
}

static void TestEdgeOnRendersNothing()
{
    std::printf("A collapsed transform renders nothing, not a full-size frame\n");

    // Invert() falls back to the identity for a degenerate matrix, so without
    // special handling a swivel passing through exactly 90 degrees would snap
    // the image to full size for a frame -- a pop in the middle of every flip.
    AnimParams a = AnimParams::Default();
    a.stages[0].enabled     = true;
    a.stages[0].easing      = Easing::Linear();
    a.stages[0].startFrame  = 0.0f;
    a.stages[0].endFrame    = 10.0f;
    a.stages[0].swivelYFrom = 90.0f;
    a.stages[0].swivelYTo   = 90.0f;

    BlurParams off = BlurParams::Default();
    off.enabled = false;

    const SampleTransforms st = BuildSampleTransforms(a, off, 5.0f, 1920.0f, 1080.0f);
    CheckNear(st.opacity[0], 0.0f, 1e-6f, "edge-on is fully transparent");

    // The inverse must be neutralised too, not just the opacity. Invert() only
    // falls back to the identity below 1e-12, and cosf(90 degrees) is -4.4e-8 --
    // far above it -- so the matrix really is inverted and 1/det throws the
    // sampling coordinates out by seven orders of magnitude. A zero opacity
    // hides the colour but does not stop the fetch, and on the CUDA path that
    // fetch faults and kills the context, blacking out every frame that follows.
    const Mat3 ident = Mat3::Identity();
    for (int e = 0; e < 6; ++e)
        CheckNear(st.inv[0].m[e], ident.m[e], 1e-6f,
                  "a collapsed sample keeps an identity inverse, so nothing is fetched out of range");

    // Either side of edge-on it must be visible again, so the collapse is a
    // single instant rather than a hole in the animation.
    a.stages[0].swivelYFrom = 80.0f; a.stages[0].swivelYTo = 80.0f;
    const SampleTransforms before = BuildSampleTransforms(a, off, 5.0f, 1920.0f, 1080.0f);
    Check(before.opacity[0] > 0.9f, "just short of edge-on is still visible");

    a.stages[0].swivelYFrom = 100.0f; a.stages[0].swivelYTo = 100.0f;
    const SampleTransforms past = BuildSampleTransforms(a, off, 5.0f, 1920.0f, 1080.0f);
    Check(past.opacity[0] > 0.9f, "past edge-on is visible again, mirrored");

    // The other side of the threshold: a deliberately tiny scale is not a
    // collapse and must still render. A 1% scale has a determinant of 1e-4,
    // hundreds of times the one-pixel cutoff.
    AnimParams smallScale = AnimParams::Default();
    smallScale.stages[0].enabled   = true;
    smallScale.stages[0].startFrame = 0.0f;
    smallScale.stages[0].endFrame   = 10.0f;
    smallScale.stages[0].scaleFrom  = 0.01f;
    smallScale.stages[0].scaleTo    = 0.01f;
    Check(BuildSampleTransforms(smallScale, off, 5.0f, 1920.0f, 1080.0f).opacity[0] > 0.9f,
          "a 1% scale is small, not collapsed, and stays visible");

    // A tilt collapses the same way.
    AnimParams t = AnimParams::Default();
    t.stages[0].enabled    = true;
    t.stages[0].startFrame = 0.0f;
    t.stages[0].endFrame   = 10.0f;
    t.stages[0].tiltXFrom  = 90.0f;
    t.stages[0].tiltXTo    = 90.0f;
    CheckNear(BuildSampleTransforms(t, off, 5.0f, 1920.0f, 1080.0f).opacity[0], 0.0f, 1e-6f,
              "an edge-on tilt is also transparent");
}

static void TestSyncPeakShift()
{
    std::printf("Syncing the acceleration peak moves the stage without reshaping it\n");

    // The arithmetic the Sync Acceleration button performs, checked here rather
    // than only through the host: peak = start + progress * span, then both ends
    // shift by (playhead - peak).
    const Easing e = Easing::EaseOut();
    const float  p = PeakVelocityProgress(e);

    const double start = 10.0, end = 34.0;
    const double span  = end - start;
    const double peak  = start + static_cast<double>(p) * span;

    const double playhead = 50.0;
    const double delta    = std::floor((playhead - peak) + 0.5);

    const double newStart = start + delta;
    const double newEnd   = end   + delta;

    // The invariant that matters: the span is preserved exactly, so duration,
    // easing and the shape of the move cannot have changed.
    CheckNear(newEnd - newStart, span, 1e-9, "the duration is unchanged by the shift");

    // And the peak now lands on the playhead, within the rounding to a frame.
    const double newPeak = newStart + static_cast<double>(p) * span;
    Check(std::fabs(newPeak - playhead) <= 0.5,
          "the peak lands on the playhead, to the nearest frame");

    // Applying it a second time is a no-op, so the button is safe to press
    // repeatedly rather than walking the stage along.
    const double delta2 = std::floor((playhead - newPeak) + 0.5);
    CheckNear(delta2, 0.0, 1e-9, "syncing an already-synced stage changes nothing");

    // The peak really is where the timeline lane marks it: an Ease Out peaks
    // early, so the stage ends up starting close to the playhead, while an Ease
    // In peaks late and ends up finishing near it. Same button, opposite result,
    // which is the whole point of syncing to the peak rather than to an end.
    Check(p < 0.3f, "Ease Out peaks early, so the stage lands mostly after the playhead");
    Check(PeakVelocityProgress(Easing::EaseIn()) > 0.7f,
          "Ease In peaks late, so the stage lands mostly before it");
}

static void TestSolvePeakBalance()
{
    std::printf("Rebalancing the easing moves the peak without changing how much easing there is\n");

    const Easing smooth = Easing::Smooth();       // 42 / 42, total 84

    // The peak can be placed across a wide span, and the total easing survives
    // every one of them -- that is the promise the button makes.
    const float targets[5] = { 0.15f, 0.3f, 0.5f, 0.7f, 0.85f };
    for (float target : targets)
    {
        float easeIn = 0.0f, easeOut = 0.0f, peak = 0.0f;
        Check(SolvePeakBalance(smooth, target, easeIn, easeOut, peak),
              "a curve with easing can be rebalanced");

        CheckNear(easeIn + easeOut, 84.0f, 0.5f,
                  "the combined easing is preserved, so the move stays as soft as it was");

        // Verify against the real evaluator rather than trusting the returned
        // value: rebuild the curve the way the plugin will and re-measure it.
        const Easing rebuilt = MakeEasing(easeIn, easeOut, 0.0f, 0.0f);
        CheckNear(PeakVelocityProgress(rebuilt), peak, 1e-3f,
                  "the reported peak is what the rebuilt curve actually does");
        Check(std::fabs(PeakVelocityProgress(rebuilt) - target) < 0.1f,
              "and it lands close to the requested position");
    }

    // The direction has to be right, or the button would move the peak the wrong
    // way and still report success: weighting towards Ease In delays the
    // acceleration, towards Ease Out brings it forward.
    float lateIn = 0.0f, lateOut = 0.0f, latePeak = 0.0f;
    float earlyIn = 0.0f, earlyOut = 0.0f, earlyPeak = 0.0f;
    SolvePeakBalance(smooth, 0.85f, lateIn,  lateOut,  latePeak);
    SolvePeakBalance(smooth, 0.15f, earlyIn, earlyOut, earlyPeak);

    Check(lateIn > earlyIn,   "a later peak takes more Ease In");
    Check(earlyOut > lateOut, "an earlier peak takes more Ease Out");
    Check(latePeak > earlyPeak, "and the peaks really do end up in that order");

    // Linear has no easing to redistribute, and must be refused rather than
    // silently producing a curve out of nothing.
    float a = 0.0f, b = 0.0f, c = 0.0f;
    Check(!SolvePeakBalance(Easing::Linear(), 0.5f, a, b, c),
          "a linear curve is refused, not invented");

    // A heavily eased curve cannot be pushed all the way to one side, because
    // neither amount may exceed 100. It must still return the closest legal
    // answer rather than failing or producing an out-of-range value.
    Easing heavy = MakeEasing(90.0f, 90.0f, 0.0f, 0.0f);   // total 180
    float hIn = 0.0f, hOut = 0.0f, hPeak = 0.0f;
    Check(SolvePeakBalance(heavy, 0.95f, hIn, hOut, hPeak),
          "a heavily eased curve still solves");
    Check(hIn <= 100.0f && hOut <= 100.0f && hIn >= 0.0f && hOut >= 0.0f,
          "and stays inside the range both amounts are allowed");
    CheckNear(hIn + hOut, 180.0f, 0.5f, "still preserving the total");

    // Bounce curves are the reason this samples instead of bisecting: the
    // oscillation can break monotonicity. It only has to return something legal
    // and self-consistent.
    const Easing bouncy = MakeEasing(30.0f, 30.0f, 0.0f, 0.0f,
                                     kBounceSpring, 50.0f, 3.0f, 45.0f, 60.0f);
    float bIn = 0.0f, bOut = 0.0f, bPeak = 0.0f;
    Check(SolvePeakBalance(bouncy, 0.4f, bIn, bOut, bPeak), "a bounce curve solves");
    CheckNear(bIn + bOut, 60.0f, 0.5f, "preserving its total too");

    Easing bRebuilt = bouncy;
    bRebuilt.x1 = bIn * 0.01f;
    bRebuilt.x2 = 1.0f - bOut * 0.01f;
    CheckNear(PeakVelocityProgress(bRebuilt), bPeak, 1e-3f,
              "and the reported peak matches the rebuilt bounce curve");
}

static void TestFlattenReproducesTheTransform()
{
    std::printf("Flattening an animation reproduces the pose it collapsed\n");

    const float W = 1920.0f, H = 1080.0f;

    // Rotations and uniform scales compose without shear, so for these the
    // flattened pose must rebuild the original matrix exactly -- this is the
    // whole promise of "continue the move on the next clip".
    AnimParams a = AnimParams::Default();
    a.stageCount = 3;
    for (int i = 0; i < 3; ++i)
    {
        Stage& s = a.stages[i];
        s.enabled    = true;
        s.easing     = Easing::Linear();
        s.startFrame = 0.0f;
        s.endFrame   = 20.0f;
        s.scaleTo    = 1.0f + 0.15f * static_cast<float>(i + 1);
        s.rotTo      = 12.0f * static_cast<float>(i + 1);
        s.posXTo     = 0.05f * static_cast<float>(i + 1);
        s.posYTo     = -0.04f * static_cast<float>(i + 1);
        s.anchorX    = 0.35f;
        s.anchorY    = 0.7f;
    }
    a.base.scaleX = 0.8f;
    a.base.rot    = -20.0f;
    a.base.posX   = 0.1f;

    const float t = 20.0f;
    const FlatPose f = FlattenTransform(a, t, W, H);

    CheckNear(f.shear, 0.0f, 1e-4f,
              "rotation and uniform scale compose without shear");

    // Rebuild the pose the way a stage would, and compare against what the
    // renderer actually produces.
    const Mat3 original = EvaluateTransform(a, t, W, H);
    const Mat3 rebuilt  = MakeTransform(0.5f * W, 0.5f * H,
                                        f.scaleX, f.scaleY, f.rot,
                                        f.posX * W, f.posY * H);

    for (int e = 0; e < 6; ++e)
        CheckNear(rebuilt.m[e], original.m[e], 1e-2f,
                  "the flattened pose rebuilds the composed transform");

    // A corner is the practical test: sub-pixel agreement on where the image
    // actually lands is what stops a visible jump at the cut.
    const float cx[4] = { 0.0f, W, 0.0f, W };
    const float cy[4] = { 0.0f, 0.0f, H, H };
    for (int k = 0; k < 4; ++k)
    {
        float ox, oy, rx, ry;
        original.Apply(cx[k], cy[k], ox, oy);
        rebuilt.Apply (cx[k], cy[k], rx, ry);
        CheckNear(rx, ox, 0.05f, "a corner lands in the same place");
        CheckNear(ry, oy, 0.05f, "a corner lands in the same place vertically");
    }

    // Opacity multiplies through, so a half-faded animation flattens to half.
    AnimParams fade = AnimParams::Default();
    fade.stageCount = 2;
    for (int i = 0; i < 2; ++i)
    {
        fade.stages[i].enabled     = true;
        fade.stages[i].easing      = Easing::Linear();
        fade.stages[i].startFrame  = 0.0f;
        fade.stages[i].endFrame    = 10.0f;
        fade.stages[i].opacityFrom = 1.0f;
        fade.stages[i].opacityTo   = 0.5f;
    }
    CheckNear(FlattenTransform(fade, 10.0f, W, H).opacity, 0.25f, 1e-4f,
              "opacity flattens as the product of the stages");

    // Shear is reported, not hidden.
    //
    // Note the stage order. Stages compose as S0 * S1 * ... , so the *later*
    // stage is applied first. A rotate stage followed by a scale stage is
    // therefore scale-then-rotate = R*S, which is precisely the form
    // MakeTransform builds and carries no shear at all. Shear needs the
    // opposite: scale applied after the rotation, so the scale stage has to be
    // the outer (lower-numbered) one.
    AnimParams sheared = AnimParams::Default();
    sheared.stageCount = 2;
    sheared.stages[0].enabled = true;                       // outer: scales
    sheared.stages[0].easing  = Easing::Linear();
    sheared.stages[0].startFrame = 0.0f; sheared.stages[0].endFrame = 10.0f;
    sheared.stages[0].linkScale  = false;
    sheared.stages[0].scaleFrom  = 2.0f; sheared.stages[0].scaleTo  = 2.0f;
    sheared.stages[0].scaleYFrom = 0.5f; sheared.stages[0].scaleYTo = 0.5f;
    sheared.stages[1].enabled = true;                       // inner: rotates
    sheared.stages[1].easing  = Easing::Linear();
    sheared.stages[1].startFrame = 0.0f; sheared.stages[1].endFrame = 10.0f;
    sheared.stages[1].rotFrom = 45.0f;   sheared.stages[1].rotTo    = 45.0f;

    Check(std::fabs(FlattenTransform(sheared, 10.0f, W, H).shear) > 0.1f,
          "a non-uniform scale applied after a rotation reports its shear");

    // And the benign order really is benign, which is what makes the warning
    // worth trusting when it does appear.
    AnimParams benign = sheared;
    benign.stages[0] = sheared.stages[1];
    benign.stages[1] = sheared.stages[0];
    CheckNear(FlattenTransform(benign, 10.0f, W, H).shear, 0.0f, 1e-4f,
              "rotate-then-scale is exactly representable and reports none");

    // The end of the animation is the latest end frame of any enabled stage,
    // since that is when the composed pose stops moving.
    AnimParams staggered = AnimParams::Default();
    staggered.stageCount = 3;
    for (int i = 0; i < 3; ++i)
    {
        staggered.stages[i].enabled    = true;
        staggered.stages[i].startFrame = static_cast<float>(i) * 5.0f;
        staggered.stages[i].endFrame   = staggered.stages[i].startFrame + 12.0f;
    }
    staggered.stages[1].enabled = false;      // a disabled stage does not count
    CheckNear(AnimationEndTime(staggered, 0.0f), 22.0f, 1e-3f,
              "the animation ends with the last enabled stage");
}

static void TestStageContextRebuildsTheWhole()
{
    std::printf("A stage's context reconstitutes the full transform\n");

    // The overlay draws its controls through outer * stage * inner. If that
    // product is not exactly what the renderer computes, the gizmo sits beside
    // the picture instead of on it -- which is the bug this exists to prevent.
    AnimParams a = AnimParams::Default();
    a.stageCount = 4;
    for (int i = 0; i < 4; ++i)
    {
        Stage& s = a.stages[i];
        s.enabled    = true;
        s.startFrame = static_cast<float>(i) * 3.0f;
        s.endFrame   = s.startFrame + 20.0f;
        s.scaleTo    = 1.0f + 0.2f * static_cast<float>(i + 1);
        s.rotTo      = 10.0f * static_cast<float>(i + 1);
        s.posXTo     = 0.05f * static_cast<float>(i + 1);
        s.posYTo     = -0.03f * static_cast<float>(i + 1);
        s.anchorX    = 0.4f;
        s.anchorY    = 0.6f;
    }
    a.base.scaleX  = 0.7f;
    a.base.posX    = 0.2f;
    a.base.rot     = -15.0f;
    a.base.swivelY = 25.0f;

    const float W = 1920.0f, H = 1080.0f;
    const float t = 11.0f;

    const Mat3 full = EvaluateTransform(a, t, W, H);

    for (int i = 0; i < 4; ++i)
    {
        const StageContext ctx = EvaluateStageContext(a, i, t, W, H);
        const Mat3 own   = EvaluateStage(a.stages[i], StageLocalTime(a, a.stages[i], t), W, H);
        const Mat3 rebuilt = ctx.outer * own * ctx.inner;

        for (int e = 0; e < 6; ++e)
            CheckNear(rebuilt.m[e], full.m[e], 2e-2f,
                      "stage " + std::to_string(i + 1) + " context rebuilds the full transform");
    }

    // A disabled stage contributes nothing, so removing it from the middle must
    // not shift where the others believe they are.
    a.stages[1].enabled = false;
    const Mat3 fullNo1 = EvaluateTransform(a, t, W, H);
    const StageContext ctx2 = EvaluateStageContext(a, 2, t, W, H);
    const Mat3 own2 = EvaluateStage(a.stages[2], StageLocalTime(a, a.stages[2], t), W, H);
    const Mat3 rebuilt2 = ctx2.outer * own2 * ctx2.inner;
    for (int e = 0; e < 6; ++e)
        CheckNear(rebuilt2.m[e], fullNo1.m[e], 2e-2f,
                  "a disabled neighbour drops out of the context too");

    // With one stage and a neutral base there is nothing around it, so the
    // context must be the identity -- the overlay's behaviour in the simple
    // case has to be exactly what it always was.
    AnimParams solo = AnimParams::Default();
    solo.stageCount = 1;
    solo.stages[0].enabled = true;
    const StageContext plain = EvaluateStageContext(solo, 0, 5.0f, W, H);
    const Mat3 ident = Mat3::Identity();
    for (int e = 0; e < 6; ++e)
    {
        CheckNear(plain.outer.m[e], ident.m[e], 1e-6f, "a lone stage has an identity outer");
        CheckNear(plain.inner.m[e], ident.m[e], 1e-6f, "a lone stage has an identity inner");
    }
}

static void TestSwappingEndsReversesThePathExactly()
{
    std::printf("Swapping a stage's ends reverses its route without reshaping it\n");

    // Swap FROM and TO also trades the two path handles. The claim is that this
    // is exact rather than approximate: the first handle is an offset from one
    // third along the straight line and the second from two thirds, so
    // reversing the line maps each precisely onto the other. If that were
    // wrong, a bent path would turn inside out on every swap.
    Stage a = Stage::Default();
    a.posXFrom = -0.30f; a.posYFrom =  0.10f;
    a.posXTo   =  0.40f; a.posYTo   = -0.20f;
    a.pathC1X  =  0.15f; a.pathC1Y  =  0.35f;
    a.pathC2X  = -0.20f; a.pathC2Y  =  0.10f;

    Stage b = a;
    b.posXFrom = a.posXTo;  b.posYFrom = a.posYTo;
    b.posXTo   = a.posXFrom; b.posYTo  = a.posYFrom;
    b.pathC1X  = a.pathC2X; b.pathC1Y  = a.pathC2Y;
    b.pathC2X  = a.pathC1X; b.pathC2Y  = a.pathC1Y;

    for (int i = 0; i <= 20; ++i)
    {
        const float p = static_cast<float>(i) / 20.0f;

        float ax, ay, bx, by;
        EvaluatePath(a, p,        ax, ay);
        EvaluatePath(b, 1.0f - p, bx, by);

        CheckNear(bx, ax, 1e-5f, "the swapped route retraces the original in X");
        CheckNear(by, ay, 1e-5f, "the swapped route retraces the original in Y");
    }

    // Swapping twice must return the original exactly, or repeated presses would
    // drift the shape.
    Stage back = b;
    back.posXFrom = b.posXTo;  back.posYFrom = b.posYTo;
    back.posXTo   = b.posXFrom; back.posYTo  = b.posYFrom;
    back.pathC1X  = b.pathC2X; back.pathC1Y  = b.pathC2Y;
    back.pathC2X  = b.pathC1X; back.pathC2Y  = b.pathC1Y;

    CheckNear(back.posXFrom, a.posXFrom, 1e-6f, "two swaps restore the start");
    CheckNear(back.posXTo,   a.posXTo,   1e-6f, "two swaps restore the end");
    CheckNear(back.pathC1X,  a.pathC1X,  1e-6f, "two swaps restore the first handle");
    CheckNear(back.pathC2X,  a.pathC2X,  1e-6f, "two swaps restore the second handle");
}

static void TestEndpointsAreExact()
{
    std::printf("A stage is exactly at its From and To on its own frames\n");

    // The overlay pins its controls to the stage's start and end frames rather
    // than to the playhead, so they mark fixed places instead of drifting as you
    // scrub. That only puts them ON the image if a stage is exactly at its From
    // on its start frame and exactly at its To on its end frame -- so no easing,
    // however far it wanders in between, may leave the endpoints off target.
    const Easing curves[6] = {
        Easing::Linear(), Easing::Smooth(), Easing::EaseIn(), Easing::EaseOut(),
        MakeEasing(0.0f, 30.0f, 0.0f, 0.0f, kBounceSpring, 60.0f, 3.0f, 45.0f, 60.0f),
        MakeEasing(20.0f, 0.0f, 0.0f, 80.0f, kBounceBall, 80.0f, 4.0f, 35.0f, 45.0f)
    };

    for (int e = 0; e < 6; ++e)
    {
        CheckNear(ApplyEasing(0.0f, curves[e]), 0.0f, 1e-5f, "easing starts exactly at 0");
        CheckNear(ApplyEasing(1.0f, curves[e]), 1.0f, 1e-5f, "easing lands exactly on 1");
    }

    // And the same through the anchor mapping, for every anchor mode: the
    // endpoint frame has to round-trip to progress 0 and 1 whether the stage is
    // measured from the clip start, the clip end, the timeline, or stretched.
    const int anchors[4] = { kAnchorClipStart, kAnchorClipEnd, kAnchorTimeline, kAnchorStretch };
    for (int k = 0; k < 4; ++k)
    {
        AnimParams a = AnimParams::Default();
        a.stageCount = 1;
        a.clipStart  = 107961.0f;
        a.clipLength = 155.0f;

        Stage& s = a.stages[0];
        s.enabled = true;
        s.easing  = Easing::Smooth();
        s.anchor  = anchors[k];

        if (anchors[k] == kAnchorStretch)      { s.startFrame = 10.0f;  s.endFrame = 80.0f; }
        else if (anchors[k] == kAnchorClipEnd) { s.startFrame = -24.0f; s.endFrame =  0.0f; }
        else                                   { s.startFrame = 6.0f;   s.endFrame = 30.0f; }

        const float tA = ClipTimeFromStageFrame(a, s, s.startFrame);
        const float tB = ClipTimeFromStageFrame(a, s, s.endFrame);

        CheckNear(StageProgress(s, StageLocalTime(a, s, tA)), 0.0f, 1e-4f,
                  "the start frame is progress 0 under anchor " + std::to_string(k));
        CheckNear(StageProgress(s, StageLocalTime(a, s, tB)), 1.0f, 1e-4f,
                  "the end frame is progress 1 under anchor " + std::to_string(k));
    }
}

static void TestPeakVelocity()
{
    std::printf("Peak velocity of an easing curve\n");

    // The whole point of marking this on the timeline is that the fastest moment
    // is NOT the middle of the stage for anything but linear easing, so each
    // preset has to land where its shape says it should.
    const float easeOut = PeakVelocityProgress(Easing::EaseOut());
    Check(easeOut < 0.3f, "Ease Out is fastest early, where it leaves at full speed");

    const float easeIn = PeakVelocityProgress(Easing::EaseIn());
    Check(easeIn > 0.7f, "Ease In is fastest late, having accelerated the whole way");

    const float smooth = PeakVelocityProgress(Easing::Smooth());
    Check(smooth > 0.35f && smooth < 0.65f, "an ease in-out peaks in the middle");

    // Linear has no peak: every step covers the same ground. Reported as the
    // start, and the important part is that the curve really is flat rather than
    // that any particular step wins the tie.
    const Easing lin = Easing::Linear();
    const float  v0  = EasingSpeed(lin, 0);
    for (int k = 1; k < kVelocitySteps; ++k)
        CheckNear(EasingSpeed(lin, k), v0, 1e-5f, "linear easing moves at a constant speed");

    // Speed sums to the total distance travelled: a sanity check that these are
    // really differences of the curve and not some unrelated quantity. Overshoot
    // is excluded here because it doubles back, so its path is longer than 1.
    float total = 0.0f;
    for (int k = 0; k < kVelocitySteps; ++k) total += EasingSpeed(Easing::Smooth(), k);
    CheckNear(total, 1.0f, 1e-4f, "a monotonic curve's speeds sum to the full move");
}

static void TestStageMoves()
{
    std::printf("Stages that hold a pose are distinguished from stages that move\n");

    // Distinct from IsNoOp, which asks whether a stage is neutral. A stage held
    // at a constant 1.5x is not neutral but does not move, and plotting a
    // velocity for it would point at a moment when nothing happens.
    Stage held = Stage::Default();
    held.scaleFrom = 1.5f; held.scaleTo = 1.5f;
    Check(!StageMoves(held), "a stage held at a constant scale does not move");

    Stage moves = Stage::Default();
    moves.scaleFrom = 1.0f; moves.scaleTo = 1.5f;
    Check(StageMoves(moves), "a changing scale moves");

    Stage tilt = Stage::Default();
    tilt.tiltXFrom = 0.0f; tilt.tiltXTo = 40.0f;
    Check(StageMoves(tilt), "a tilt moves");

    Stage swivel = Stage::Default();
    swivel.swivelYFrom = -90.0f; swivel.swivelYTo = 0.0f;
    Check(StageMoves(swivel), "a swivel moves");

    Stage fade = Stage::Default();
    fade.opacityFrom = 0.0f; fade.opacityTo = 1.0f;
    Check(StageMoves(fade), "a fade moves");

    // Scale Y only counts while the axes are unlinked, or a stale Y value left
    // behind under a link would light up a lane that is not going anywhere.
    Stage linkedY = Stage::Default();
    linkedY.linkScale = true;
    linkedY.scaleYFrom = 1.0f; linkedY.scaleYTo = 3.0f;
    Check(!StageMoves(linkedY), "Scale Y is ignored while the axes are linked");

    linkedY.linkScale = false;
    Check(StageMoves(linkedY), "the same Scale Y counts once unlinked");

    // Endpoints that coincide still move if the path between them bulges.
    Stage bent = Stage::Default();
    bent.pathC1X = 0.4f;
    Check(StageMoves(bent), "a bent path moves even between identical endpoints");
}

static void TestIsNoOpCoversNewChannels()
{
    std::printf("No-op detection covers the new channels\n");

    // Same class of bug as the earlier per-frame identity test: a stage that
    // only tilts would otherwise be declared a pass-through and skipped.
    struct Case { const char* name; void (*apply)(Stage&); };
    const Case cases[] = {
        { "tilt",     [](Stage& s) { s.tiltXTo   = 30.0f; } },
        { "swivel",   [](Stage& s) { s.swivelYTo = 30.0f; } },
    };

    for (const Case& c : cases)
    {
        AnimParams a = AnimParams::Default();
        a.stages[0].enabled = true;
        c.apply(a.stages[0]);
        Check(!IsNoOp(a), std::string("animating ") + c.name + " is not a no-op");
    }

    // An unlinked Y scale counts, but only while it is actually unlinked.
    AnimParams unlinked = AnimParams::Default();
    unlinked.stages[0].enabled    = true;
    unlinked.stages[0].linkScale  = false;
    unlinked.stages[0].scaleYTo   = 2.0f;
    Check(!IsNoOp(unlinked), "an unlinked Y scale is not a no-op");

    AnimParams linked = unlinked;
    linked.stages[0].linkScale = true;
    Check(IsNoOp(linked), "a stale Y scale behind a link is still a no-op");

    // A base pose that does anything is enough on its own.
    AnimParams based = AnimParams::Default();
    based.base.posX = 0.1f;
    Check(!IsNoOp(based), "a non-neutral base is not a no-op");

    AnimParams basedOpacity = AnimParams::Default();
    basedOpacity.base.opacity = 0.5f;
    Check(!IsNoOp(basedOpacity), "a base opacity is not a no-op");
}

////////////////////////////////////////////////////////////////////////////////
// Clip time

static void TestClipRangeValidation()
{
    std::printf("Clip range validation\n");

    double start = -1.0, length = -1.0;

    // The measured real answer from timeLineGetBounds: a 155-frame clip sitting
    // an hour into the timeline.
    Check(ValidateClipRange(107961.0, 108116.0, start, length), "accepts a real clip range");
    CheckNear(static_cast<float>(start),  107961.0f, 1e-3f, "clip start is the lower bound");
    CheckNear(static_cast<float>(length), 155.0f,    1e-3f, "clip length is the span");

    // Resolve's getFrameRange sentinel: exactly 1000 minutes at 29.97. Accepting
    // it would place every clip's frame 0 at timeline zero and mis-time
    // everything, so it has to be rejected rather than used.
    Check(!ValidateClipRange(0.0, 1798200.0, start, length), "rejects the unbounded sentinel");

    // getUnmappedFrameRange came back as [0, 0].
    Check(!ValidateClipRange(0.0, 0.0, start, length), "rejects an empty range");

    // Inverted or nonsensical input must not be trusted.
    Check(!ValidateClipRange(500.0, 100.0, start, length), "rejects an inverted range");

    const double nan = std::numeric_limits<double>::quiet_NaN();
    Check(!ValidateClipRange(nan, 100.0, start, length), "rejects NaN");

    // A single-frame clip is legitimate and must survive.
    Check(ValidateClipRange(10.0, 11.0, start, length), "accepts a one-frame clip");

    // A long but plausible clip -- an hour at 30fps -- must not trip the
    // sentinel guard.
    Check(ValidateClipRange(0.0, 108000.0, start, length), "accepts an hour-long clip");
}

static void TestLegacyTimingDetection()
{
    std::printf("Legacy absolute timing detection\n");

    // A Resolve timeline starts at 01:00:00:00, so a clip's start sits around
    // 107892 frames and any legacy value sits near it.
    const double clipStart = 107961.0;

    Check(LooksTimelineAbsolute(107961.0, clipStart), "clip start itself is absolute");
    Check(LooksTimelineAbsolute(107981.0, clipStart), "a value just inside the clip is absolute");
    Check(LooksTimelineAbsolute(108116.0, clipStart), "the clip end is absolute");

    // Already-converted values must be left alone, or a second pass would
    // subtract the clip start twice and throw the animation an hour backwards.
    Check(!LooksTimelineAbsolute(0.0,    clipStart), "frame 0 is already relative");
    Check(!LooksTimelineAbsolute(20.0,   clipStart), "a small value is already relative");
    Check(!LooksTimelineAbsolute(3000.0, clipStart), "even a long clip's value stays relative");

    // The margin between the two populations is enormous: a relative value
    // would have to be half an hour into the clip to be misread.
    Check(!LooksTimelineAbsolute(53000.0, clipStart), "the threshold sits far above any real clip");

    // Hosts whose timelines start at zero need no conversion, and must not get
    // one -- absolute and relative already coincide there.
    Check(!LooksTimelineAbsolute(50.0,  0.0),   "no conversion when the clip starts at zero");
    Check(!LooksTimelineAbsolute(150.0, 100.0), "no conversion for a small clip start");
}

static void TestNormaliseStageFrameIsIdempotent()
{
    std::printf("Frame normalisation is idempotent\n");

    const double clipStart = 107961.0;

    // A legacy absolute value converts to its clip-relative equivalent.
    CheckNear(static_cast<float>(NormaliseStageFrame(107981.0, clipStart)), 20.0f, 1e-3f,
              "absolute value converts to clip-relative");

    // Applying it again must not convert a second time. The render path runs
    // this on every read, so a value that shrank each pass would walk the
    // animation backwards frame by frame.
    double v = 107981.0;
    for (int i = 0; i < 10; ++i) v = NormaliseStageFrame(v, clipStart);
    CheckNear(static_cast<float>(v), 20.0f, 1e-3f, "repeated normalisation is stable");

    // Values already relative pass through untouched.
    CheckNear(static_cast<float>(NormaliseStageFrame(0.0,   clipStart)), 0.0f,   1e-3f, "0 passes through");
    CheckNear(static_cast<float>(NormaliseStageFrame(155.0, clipStart)), 155.0f, 1e-3f, "155 passes through");

    // With no clip information the value must be left exactly alone rather than
    // silently shifted toward zero.
    CheckNear(static_cast<float>(NormaliseStageFrame(107981.0, 0.0)), 107981.0f, 1e-3f,
              "unknown clip start leaves the value untouched");

    // The placeholder range the host returns before the clip is connected --
    // [0, 1999] -- must likewise change nothing.
    CheckNear(static_cast<float>(NormaliseStageFrame(107981.0, 0.0)), 107981.0f, 1e-3f,
              "placeholder clip start converts nothing");
}

////////////////////////////////////////////////////////////////////////////////
// No-op detection

static void TestIsNoOpIsTimeIndependent()
{
    std::printf("No-op detection does not depend on the frame\n");

    // The bug this guards: judging "does nothing" by evaluating the transform at
    // the current frame reports a pass-through on every frame outside the
    // animation's range, because progress pins to 0 or 1 there and the pose is
    // usually the identity. Hosts cache that verdict per frame, so moving the
    // stage's start or end over such a frame left them convinced nothing
    // happened there and the picture stopped responding to edits.
    AnimParams a = AnimParams::Default();
    a.stages[0].enabled    = true;
    a.stages[0].startFrame = 100.0f;
    a.stages[0].endFrame   = 120.0f;
    a.stages[0].scaleFrom  = 1.0f;
    a.stages[0].scaleTo    = 1.5f;

    Check(!IsNoOp(a), "a stage that scales is never a no-op");

    // Explicitly including frames far outside the range, which is where the
    // old per-frame test went wrong.
    const float times[] = { -500.0f, 0.0f, 50.0f, 99.0f, 100.0f, 110.0f,
                            120.0f, 121.0f, 5000.0f };
    bool stable = true;
    for (float t : times)
    {
        // The verdict must not change with t at all -- IsNoOp does not even
        // take a time, and this asserts the transform agrees on the endpoints.
        (void)t;
        if (IsNoOp(a)) { stable = false; break; }
    }
    Check(stable, "verdict is the same at every frame, inside the range or outside");

    // Default parameters really are a no-op, so an unconfigured instance is
    // still skipped.
    Check(IsNoOp(AnimParams::Default()), "an unconfigured effect is a no-op");
}

static void TestIsNoOpCatchesEveryChannel()
{
    std::printf("No-op detection covers every animated channel\n");

    // Anything that can move a pixel has to defeat the no-op test, or that
    // effect would be silently skipped.
    struct Case { const char* name; void (*apply)(Stage&); };
    const Case cases[] = {
        { "scale",       [](Stage& s) { s.scaleTo   = 1.2f; } },
        { "position x",  [](Stage& s) { s.posXTo    = 0.1f; } },
        { "position y",  [](Stage& s) { s.posYTo    = 0.1f; } },
        { "rotation",    [](Stage& s) { s.rotTo     = 5.0f; } },
        { "opacity",     [](Stage& s) { s.opacityTo = 0.5f; } },
        { "path bend 1", [](Stage& s) { s.pathC1Y   = 0.2f; } },
        { "path bend 2", [](Stage& s) { s.pathC2X   = 0.2f; } },
    };

    for (const Case& c : cases)
    {
        AnimParams a = AnimParams::Default();
        a.stages[0].enabled = true;
        c.apply(a.stages[0]);
        Check(!IsNoOp(a), std::string("animating ") + c.name + " is not a no-op");
    }

    // A path bend with identical endpoints still moves the image, which is easy
    // to miss because From and To match.
    AnimParams bulge = AnimParams::Default();
    bulge.stages[0].enabled = true;
    bulge.stages[0].pathC1Y = 0.3f;
    bulge.stages[0].pathC2Y = 0.3f;
    Check(!IsNoOp(bulge), "a curved path with matching endpoints still moves");

    // A disabled stage cannot contribute, so it must not block the skip.
    AnimParams disabled = AnimParams::Default();
    disabled.stages[0].enabled = false;
    disabled.stages[0].scaleTo = 3.0f;
    Check(IsNoOp(disabled), "a disabled stage does not defeat the no-op test");

    // Nor may a stage beyond the stage count.
    AnimParams beyond = AnimParams::Default();
    beyond.stageCount = 1;
    beyond.stages[1].enabled = true;
    beyond.stages[1].scaleTo = 3.0f;
    Check(IsNoOp(beyond), "a stage beyond Stage Count does not defeat the no-op test");
}

////////////////////////////////////////////////////////////////////////////////
// Motion path

static Stage MovingStage()
{
    Stage s = Stage::Default();
    s.enabled    = true;
    s.easing     = Easing::Linear();
    s.startFrame = 0.0f;
    s.endFrame   = 20.0f;
    s.posXFrom   = -0.3f; s.posYFrom = 0.1f;
    s.posXTo     =  0.4f; s.posYTo   = -0.2f;
    return s;
}

static void TestStraightPathIsExact()
{
    std::printf("Zero path offsets reproduce a straight line exactly\n");

    // The control points sit at one and two thirds along the segment, which
    // makes the cubic the degree-elevated form of the linear interpolation.
    // That is what lets the motion path be added without changing a single
    // existing animation -- it must match Lerp to the bit, not approximately.
    const Stage s = MovingStage();

    bool exact = true;
    for (int i = 0; i <= 100; ++i)
    {
        const float e = i / 100.0f;
        float px, py;
        EvaluatePath(s, e, px, py);

        if (std::fabs(px - Lerp(s.posXFrom, s.posXTo, e)) > 1e-6f) exact = false;
        if (std::fabs(py - Lerp(s.posYFrom, s.posYTo, e)) > 1e-6f) exact = false;
    }
    Check(exact, "straight path matches linear interpolation exactly");

    // And therefore the composed transform is unchanged too.
    AnimParams a = AnimParams::Default();
    a.stages[0] = s;
    const Mat3 m = EvaluateTransform(a, 10.0f, 1920.0f, 1080.0f);
    float x, y;
    m.Apply(0.0f, 0.0f, x, y);
    CheckNear(x, Lerp(s.posXFrom, s.posXTo, 0.5f) * 1920.0f, 1e-2f,
              "transform translation follows the straight path");
}

static void TestPathEndpointsAreFixed()
{
    std::printf("Path handles never move the endpoints\n");

    Stage s = MovingStage();
    s.pathC1X = 0.5f;  s.pathC1Y = -0.6f;
    s.pathC2X = -0.4f; s.pathC2Y = 0.35f;

    float px, py;
    EvaluatePath(s, 0.0f, px, py);
    CheckNear(px, s.posXFrom, 1e-6f, "path starts at From, x");
    CheckNear(py, s.posYFrom, 1e-6f, "path starts at From, y");

    EvaluatePath(s, 1.0f, px, py);
    CheckNear(px, s.posXTo, 1e-6f, "path ends at To, x");
    CheckNear(py, s.posYTo, 1e-6f, "path ends at To, y");
}

static void TestPathBends()
{
    std::printf("Path handles bend the trajectory\n");

    const Stage straight = MovingStage();
    Stage bent = straight;
    bent.pathC1Y = 0.5f;
    bent.pathC2Y = 0.5f;

    float sx, sy, bx, by;
    EvaluatePath(straight, 0.5f, sx, sy);
    EvaluatePath(bent,     0.5f, bx, by);

    Check(std::fabs(by - sy) > 0.1f, "offsetting the handles moves the midpoint off the line");
    CheckNear(bx, sx, 1e-5f, "a purely vertical offset does not shift the path in x");

    // Offsets are relative to the straight line, so moving the endpoints carries
    // the bend along rather than leaving the handles stranded.
    Stage shifted = bent;
    shifted.posXFrom += 1.0f;
    shifted.posXTo   += 1.0f;
    float ox, oy, nx, ny;
    EvaluatePath(bent,    0.5f, ox, oy);
    EvaluatePath(shifted, 0.5f, nx, ny);
    CheckNear(nx, ox + 1.0f, 1e-5f, "bend travels with the endpoints, x");
    CheckNear(ny, oy,        1e-5f, "bend travels with the endpoints, y");
}

static void TestEasingDrivesSpeedNotShape()
{
    std::printf("Easing sets speed along the path, not its shape\n");

    Stage linear = MovingStage();
    linear.pathC1Y = 0.4f;
    linear.pathC2Y = 0.4f;

    Stage eased = linear;
    eased.easing = Easing::Smooth();

    // The two stages trace the *same* curve through space; only the timing
    // along it differs. Sampling by eased progress must therefore land on
    // points that also exist on the linear version.
    const float e = ApplyEasing(0.3f, eased.easing);
    float ex, ey, lx, ly;
    EvaluatePath(eased,  e,     ex, ey);
    EvaluatePath(linear, e,     lx, ly);
    CheckNear(ex, lx, 1e-6f, "same path point for the same parameter, x");
    CheckNear(ey, ly, 1e-6f, "same path point for the same parameter, y");

    // But at the same *time* they are in different places, because the easing
    // has moved the object further along.
    float atX, atY, alX, alY;
    EvaluatePath(eased,  ApplyEasing(0.3f, eased.easing),  atX, atY);
    EvaluatePath(linear, ApplyEasing(0.3f, linear.easing), alX, alY);
    Check(std::fabs(atX - alX) > 1e-3f || std::fabs(atY - alY) > 1e-3f,
          "easing changes where along the path the object is at a given time");
}

////////////////////////////////////////////////////////////////////////////////
// Opacity

static void TestOpacityFade()
{
    std::printf("Opacity fade\n");

    AnimParams a = AnimParams::Default();
    a.stages[0].enabled     = true;
    a.stages[0].easing      = Easing::Linear();
    a.stages[0].startFrame  = 100.0f;
    a.stages[0].endFrame    = 120.0f;
    a.stages[0].opacityFrom = 0.0f;
    a.stages[0].opacityTo   = 1.0f;

    CheckNear(EvaluateOpacity(a, 99.0f),  0.0f, 1e-5f, "fade in: transparent before start");
    CheckNear(EvaluateOpacity(a, 110.0f), 0.5f, 1e-5f, "fade in: half way");
    CheckNear(EvaluateOpacity(a, 120.0f), 1.0f, 1e-5f, "fade in: opaque at end");
    CheckNear(EvaluateOpacity(a, 999.0f), 1.0f, 1e-5f, "fade in: stays opaque");

    // Fade out is the same control run the other way.
    a.stages[0].opacityFrom = 1.0f;
    a.stages[0].opacityTo   = 0.0f;
    CheckNear(EvaluateOpacity(a, 110.0f), 0.5f, 1e-5f, "fade out: half way");
    CheckNear(EvaluateOpacity(a, 120.0f), 0.0f, 1e-5f, "fade out: transparent at end");

    // Default parameters must be fully opaque, or every clip would be dimmed.
    AnimParams plain = AnimParams::Default();
    CheckNear(EvaluateOpacity(plain, 50.0f), 1.0f, 1e-5f, "default animation is fully opaque");
}

static void TestOpacityComposition()
{
    std::printf("Opacity composition and clamping\n");

    AnimParams a = AnimParams::Default();
    a.stageCount = 2;
    for (int i = 0; i < 2; ++i)
    {
        a.stages[i].enabled     = true;
        a.stages[i].easing      = Easing::Linear();
        a.stages[i].startFrame  = 0.0f;
        a.stages[i].endFrame    = 10.0f;
        a.stages[i].opacityFrom = 1.0f;
        a.stages[i].opacityTo   = 0.5f;
    }
    // Stages multiply, exactly as their transforms do.
    CheckNear(EvaluateOpacity(a, 10.0f), 0.25f, 1e-5f, "two stages at 0.5 compose to 0.25");

    a.stages[1].enabled = false;
    CheckNear(EvaluateOpacity(a, 10.0f), 0.5f, 1e-5f, "disabled stage does not fade");

    a.stages[1].enabled = true;
    a.stageCount = 1;
    CheckNear(EvaluateOpacity(a, 10.0f), 0.5f, 1e-5f, "stageCount gates opacity too");

    // Overshoot easing drives interpolation past the endpoints; opacity must
    // still land inside 0..1 rather than producing out-of-range pixels.
    AnimParams over = AnimParams::Default();
    over.stages[0].enabled     = true;
    over.stages[0].startFrame  = 0.0f;
    over.stages[0].endFrame    = 10.0f;
    over.stages[0].opacityFrom = 0.0f;
    over.stages[0].opacityTo   = 1.0f;
    over.stages[0].easing      = MakeEasing(0.0f, 32.0f, 60.0f, 80.0f);

    bool inRange = true;
    for (int i = 0; i <= 100; ++i)
    {
        const float o = EvaluateOpacity(over, i / 10.0f);
        if (o < 0.0f || o > 1.0f) { inRange = false; break; }
    }
    Check(inRange, "overshoot easing keeps opacity within 0..1");
}

static void TestOpacityBlursAcrossShutter()
{
    std::printf("Opacity is sampled per shutter sample\n");

    AnimParams a = AnimParams::Default();
    a.stages[0].enabled     = true;
    a.stages[0].easing      = Easing::Linear();
    a.stages[0].startFrame  = 0.0f;
    a.stages[0].endFrame    = 20.0f;
    a.stages[0].opacityFrom = 0.0f;
    a.stages[0].opacityTo   = 1.0f;

    BlurParams b = BlurParams::Default();
    b.enabled  = true;
    b.adaptive = false;
    b.samples  = 8;
    b.shutterAngle = 360.0f;

    const SampleTransforms st = BuildSampleTransforms(a, b, 10.0f, 1920.0f, 1080.0f);
    Check(st.count == 8, "eight shutter samples");

    // A fade in progress must vary across the shutter rather than being frozen
    // at the frame-centre value.
    Check(st.opacity[0] < st.opacity[st.count - 1],
          "opacity increases across the shutter during a fade in");

    // With no blur, the single sample carries the frame's opacity.
    BlurParams off = BlurParams::Default();
    off.enabled = false;
    const SampleTransforms plain = BuildSampleTransforms(a, off, 10.0f, 1920.0f, 1080.0f);
    CheckNear(plain.opacity[0], 0.5f, 1e-4f, "un-blurred opacity is the frame value");
}

////////////////////////////////////////////////////////////////////////////////
// Motion blur

static AnimParams MovingAnim()
{
    AnimParams a = AnimParams::Default();
    a.stages[0].enabled    = true;
    a.stages[0].easing     = Easing::Linear();
    a.stages[0].startFrame = 0.0f;
    a.stages[0].endFrame   = 20.0f;
    a.stages[0].posXFrom   = 0.0f;
    a.stages[0].posXTo     = 1.0f;
    return a;
}

static void TestShutterSampleTimes()
{
    std::printf("Shutter sample times\n");

    BlurParams b = BlurParams::Default();
    b.shutterAngle = 360.0f;   // one full frame
    b.shutterPhase = 0.0f;

    // A single sample must sit exactly on the frame, so that disabling blur is
    // bit-identical rather than merely close.
    CheckNear(BlurSampleTime(10.0f, b, 0, 1), 10.0f, 1e-6f, "single sample sits on the frame");

    // Midpoint sampling: with 2 samples over a 1-frame shutter centred on frame
    // 10, they land at 9.75 and 10.25 -- not at the 9.5/10.5 extremes, which
    // would double-weight the ends of the blur.
    CheckNear(BlurSampleTime(10.0f, b, 0, 2), 9.75f,  1e-5f, "two samples: first at 9.75");
    CheckNear(BlurSampleTime(10.0f, b, 1, 2), 10.25f, 1e-5f, "two samples: second at 10.25");

    // Samples must stay strictly inside the shutter interval.
    bool inside = true;
    for (int k = 0; k < 16; ++k)
    {
        const float tk = BlurSampleTime(10.0f, b, k, 16);
        if (tk <= 9.5f || tk >= 10.5f) { inside = false; break; }
    }
    Check(inside, "all samples lie within the shutter interval");

    // Samples must be ordered and symmetric about the frame.
    CheckNear(BlurSampleTime(10.0f, b, 0, 8) + BlurSampleTime(10.0f, b, 7, 8),
              20.0f, 1e-4f, "shutter samples are symmetric about the frame");

    // A 180 degree shutter covers half the span of a 360 degree one.
    b.shutterAngle = 180.0f;
    const float span180 = BlurSampleTime(10.0f, b, 7, 8) - BlurSampleTime(10.0f, b, 0, 8);
    b.shutterAngle = 360.0f;
    const float span360 = BlurSampleTime(10.0f, b, 7, 8) - BlurSampleTime(10.0f, b, 0, 8);
    CheckNear(span180 * 2.0f, span360, 1e-4f, "180deg shutter spans half of 360deg");

    // Phase shifts the whole interval without changing its width.
    b.shutterPhase = 180.0f;   // half a frame later
    CheckNear(BlurSampleTime(10.0f, b, 0, 2), 9.75f + 0.5f, 1e-5f, "phase shifts the interval");
}

static void TestBlurDisabledIsExactlyUnblurred()
{
    std::printf("Disabled blur collapses to the un-blurred path\n");

    const AnimParams a = MovingAnim();
    const float W = 1920.0f, H = 1080.0f;

    const Mat3 plain = Invert(EvaluateTransform(a, 10.0f, W, H));

    // Three different ways of saying "no blur" must all produce exactly one
    // sample equal to the plain transform, not an approximation of it.
    BlurParams off = BlurParams::Default();
    off.enabled = false;
    off.samples = 32;

    BlurParams zeroAngle = BlurParams::Default();
    zeroAngle.enabled = true;
    zeroAngle.shutterAngle = 0.0f;

    BlurParams oneSample = BlurParams::Default();
    oneSample.enabled = true;
    oneSample.samples = 1;

    const BlurParams variants[] = { off, zeroAngle, oneSample };
    const char* names[] = { "disabled", "zero shutter angle", "one sample" };

    for (int v = 0; v < 3; ++v)
    {
        const SampleTransforms st = BuildSampleTransforms(a, variants[v], 10.0f, W, H);
        Check(st.count == 1, std::string(names[v]) + ": exactly one sample");
        for (int i = 0; i < 6; ++i)
            CheckNear(st.inv[0].m[i], plain.m[i], 1e-6f,
                      std::string(names[v]) + ": matrix identical to un-blurred");
    }
}

static void TestBlurSampleCounts()
{
    std::printf("Blur sample counts\n");

    const float W = 1920.0f, H = 1080.0f;
    const AnimParams moving = MovingAnim();

    BlurParams fixed = BlurParams::Default();
    fixed.enabled  = true;
    fixed.adaptive = false;
    fixed.samples  = 24;
    const SampleTransforms stFixed = BuildSampleTransforms(moving, fixed, 10.0f, W, H);
    Check(stFixed.count == 24, "fixed mode honours the requested sample count");

    // Requesting more than the ceiling must clamp, not overflow the array.
    fixed.samples = 10000;
    const SampleTransforms stClamped = BuildSampleTransforms(moving, fixed, 10.0f, W, H);
    Check(stClamped.count == kMaxBlurSamples, "sample count clamps to the maximum");

    // Adaptive: a fast move should use more samples than a slow one, and a
    // static frame should collapse to a single sample.
    BlurParams adaptive = BlurParams::Default();
    adaptive.enabled  = true;
    adaptive.adaptive = true;
    adaptive.samples  = kMaxBlurSamples;

    AnimParams slow = MovingAnim();
    slow.stages[0].posXTo = 0.01f;

    AnimParams still = AnimParams::Default();   // nothing animates

    const int nFast  = BuildSampleTransforms(moving, adaptive, 10.0f, W, H).count;
    const int nSlow  = BuildSampleTransforms(slow,   adaptive, 10.0f, W, H).count;
    const int nStill = BuildSampleTransforms(still,  adaptive, 10.0f, W, H).count;

    Check(nFast > nSlow,   "adaptive uses more samples for faster motion");
    Check(nStill == 1,     "adaptive collapses a static frame to one sample");
    Check(nFast <= kMaxBlurSamples, "adaptive respects the sample ceiling");
}

////////////////////////////////////////////////////////////////////////////////

int main()
{
    std::printf("=== MultiTransform animation engine tests ===\n");

    TestEasingEndpoints();
    TestEasingLinearIsIdentity();
    TestEasingGoldenValues();
    TestEasingAmountMapping();
    TestEasingMonotonic();
    TestStageProgressAndStagger();
    TestIndependentStageLengths();
    TestZeroAndInvertedDuration();
    TestMatrixIdentityAndInverse();
    TestDegenerateInverse();
    TestAnchorIsFixedPoint();
    TestRotationDirection();
    TestCompositionOrderAndDisabledStages();
    TestIdentityWhenNothingAnimates();
    TestSubFrameContinuity();
    TestBounceEndpointsExact();
    TestBounceNoneIsUnchanged();
    TestBounceHappensAtTheEnd();
    TestApproachKeepsItsShape();
    TestNoDoubleOvershoot();
    TestSpringCrossesTarget();
    TestBallNeverPassesTarget();
    TestNegativeBounceMirrorsDirection();
    TestBounceDampingReducesLateMotion();
    TestBounceCountControlsRebounds();
    TestAnchorMappings();
    TestStretchScalesWithTheClip();
    TestClipTimeRoundTrip();
    TestOutroLandsOnTheLastFrame();
    TestAnchorsCoexistInOneEffect();
    TestNeutralBaseChangesNothing();
    TestBaseComposesInnermost();
    TestSplitScale();
    TestOrthographicRotation();
    TestEdgeOnRendersNothing();
    TestIsNoOpCoversNewChannels();
    TestSyncPeakShift();
    TestSolvePeakBalance();
    TestFlattenReproducesTheTransform();
    TestStageContextRebuildsTheWhole();
    TestEndpointsAreExact();
    TestSwappingEndsReversesThePathExactly();
    TestPeakVelocity();
    TestStageMoves();
    TestClipRangeValidation();
    TestLegacyTimingDetection();
    TestNormaliseStageFrameIsIdempotent();
    TestIsNoOpIsTimeIndependent();
    TestIsNoOpCatchesEveryChannel();
    TestStraightPathIsExact();
    TestPathEndpointsAreFixed();
    TestPathBends();
    TestEasingDrivesSpeedNotShape();
    TestOpacityFade();
    TestOpacityComposition();
    TestOpacityBlursAcrossShutter();
    TestShutterSampleTimes();
    TestBlurDisabledIsExactlyUnblurred();
    TestBlurSampleCounts();

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("ALL TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
