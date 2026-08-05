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
