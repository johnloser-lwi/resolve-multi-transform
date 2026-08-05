// Unit tests for the animation engine and transform math.
//
// No test framework on purpose: zero dependencies keeps the build trivial, and
// there is not enough here to justify pulling in gtest.

#include "AnimEngine.h"
#include "MotionBlur.h"
#include "TransformMath.h"

#include <cmath>
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
