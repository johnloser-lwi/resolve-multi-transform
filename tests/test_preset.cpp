// Unit tests for preset serialisation and timing rescale.
//
// These matter more than most: a preset that half-loads or mis-scales leaves a
// setup in a state that never existed, which is harder to spot and harder to
// undo than an outright failure.

#include "Preset.h"
#include "Settings.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace mtx;

static int g_failures = 0;
static int g_checks   = 0;

static void Check(bool cond, const std::string& what)
{
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL: %s\n", what.c_str()); }
}

static void CheckNear(double actual, double expected, double tol, const std::string& what)
{
    ++g_checks;
    if (!(std::fabs(actual - expected) <= tol))
    {
        ++g_failures;
        std::printf("  FAIL: %s (got %.6f, expected %.6f)\n", what.c_str(), actual, expected);
    }
}

////////////////////////////////////////////////////////////////////////////////

/** A preset with every field set to something distinctive, so a value that is
 *  dropped or swapped in transit shows up rather than matching by luck. */
static PresetData BusyPreset()
{
    PresetData d = PresetData::Default();
    d.name             = "Push In / Fade Out";
    d.wholeEffect      = true;
    d.sourceClipLength = 155.0f;
    d.stageCount       = 4;
    d.filterMode       = 0;
    d.edgeMode         = 2;
    d.blurEnabled      = true;
    d.shutterAngle     = 172.5f;
    d.shutterPhase     = -90.0f;
    d.blurSamples      = 23;
    d.blurAdaptive     = false;

    d.base.scaleX    = 0.6f;
    d.base.scaleY    = 1.4f;
    d.base.linkScale = false;
    d.base.posX      = -0.3f;
    d.base.posY      = 0.15f;
    d.base.rot       = 8.25f;
    d.base.tiltX     = -18.0f;
    d.base.swivelY   = 42.5f;
    d.base.opacity   = 0.75f;
    d.base.anchorX   = 0.2f;
    d.base.anchorY   = 0.9f;

    const int anchors[4] = { kAnchorClipStart, kAnchorClipEnd, kAnchorStretch, kAnchorTimeline };
    for (int i = 0; i < kMaxStages; ++i)
    {
        PresetStage& s = d.stages[i];
        s.enabled      = (i != 2);
        s.anchor       = anchors[i];
        s.startFrame   = -20.0f + i * 7.0f;
        s.endFrame     = 30.0f + i * 11.0f;
        s.scaleFrom    = 1.0f + i * 0.1f;
        s.scaleTo      = 1.5f + i * 0.1f;
        s.posXFrom     = -0.25f + i * 0.05f;
        s.posYFrom     = 0.1f;
        s.posXTo       = 0.4f;
        s.posYTo       = -0.2f - i * 0.01f;
        s.rotFrom      = -12.5f;
        s.rotTo        = 37.25f + i;
        s.linkScale    = (i % 2) == 0;
        s.scaleYFrom   = 0.8f + i * 0.05f;
        s.scaleYTo     = 2.25f - i * 0.1f;
        s.tiltXFrom    = -35.0f + i;
        s.tiltXTo      = 12.5f;
        s.swivelYFrom  = -90.0f;
        s.swivelYTo    = 15.75f + i;
        s.opacityFrom  = 0.0f;
        s.opacityTo    = 1.0f;
        s.anchorX      = 0.25f;
        s.anchorY      = 0.75f;
        s.pathC1X      = 0.3f;  s.pathC1Y = -0.4f;
        s.pathC2X      = -0.15f; s.pathC2Y = 0.55f;
        s.easingPreset = 7;      // Custom
        s.easeIn       = 33.0f;
        s.easeOut      = 66.0f;
        s.anticipation = -25.0f;
        s.overshoot    = 80.0f;
        s.bounceType   = (i % 3);
        s.bounceAmount = -45.0f + i * 10.0f;
        s.bounceCount  = 3.5f;
        s.bounceDamping = 40.0f;
        s.bounceStart  = 62.0f;
    }
    return d;
}

static void CheckStagesEqual(const PresetStage& a, const PresetStage& b, const std::string& what)
{
    Check(a.enabled == b.enabled, what + ": enabled");
    Check(a.anchor  == b.anchor,  what + ": anchor");
    CheckNear(a.startFrame,    b.startFrame,    1e-3, what + ": startFrame");
    CheckNear(a.endFrame,      b.endFrame,      1e-3, what + ": endFrame");
    CheckNear(a.scaleFrom,     b.scaleFrom,     1e-4, what + ": scaleFrom");
    CheckNear(a.scaleTo,       b.scaleTo,       1e-4, what + ": scaleTo");
    CheckNear(a.posXFrom,      b.posXFrom,      1e-4, what + ": posXFrom");
    CheckNear(a.posYFrom,      b.posYFrom,      1e-4, what + ": posYFrom");
    CheckNear(a.posXTo,        b.posXTo,        1e-4, what + ": posXTo");
    CheckNear(a.posYTo,        b.posYTo,        1e-4, what + ": posYTo");
    CheckNear(a.rotFrom,       b.rotFrom,       1e-3, what + ": rotFrom");
    CheckNear(a.rotTo,         b.rotTo,         1e-3, what + ": rotTo");
    Check(a.linkScale == b.linkScale, what + ": linkScale");
    CheckNear(a.scaleYFrom,    b.scaleYFrom,    1e-4, what + ": scaleYFrom");
    CheckNear(a.scaleYTo,      b.scaleYTo,      1e-4, what + ": scaleYTo");
    CheckNear(a.tiltXFrom,     b.tiltXFrom,     1e-3, what + ": tiltXFrom");
    CheckNear(a.tiltXTo,       b.tiltXTo,       1e-3, what + ": tiltXTo");
    CheckNear(a.swivelYFrom,   b.swivelYFrom,   1e-3, what + ": swivelYFrom");
    CheckNear(a.swivelYTo,     b.swivelYTo,     1e-3, what + ": swivelYTo");
    CheckNear(a.opacityFrom,   b.opacityFrom,   1e-4, what + ": opacityFrom");
    CheckNear(a.opacityTo,     b.opacityTo,     1e-4, what + ": opacityTo");
    CheckNear(a.anchorX,       b.anchorX,       1e-4, what + ": anchorX");
    CheckNear(a.anchorY,       b.anchorY,       1e-4, what + ": anchorY");
    CheckNear(a.pathC1X,       b.pathC1X,       1e-4, what + ": pathC1X");
    CheckNear(a.pathC1Y,       b.pathC1Y,       1e-4, what + ": pathC1Y");
    CheckNear(a.pathC2X,       b.pathC2X,       1e-4, what + ": pathC2X");
    CheckNear(a.pathC2Y,       b.pathC2Y,       1e-4, what + ": pathC2Y");
    Check(a.easingPreset == b.easingPreset, what + ": easingPreset");
    CheckNear(a.easeIn,        b.easeIn,        1e-3, what + ": easeIn");
    CheckNear(a.easeOut,       b.easeOut,       1e-3, what + ": easeOut");
    CheckNear(a.anticipation,  b.anticipation,  1e-3, what + ": anticipation");
    CheckNear(a.overshoot,     b.overshoot,     1e-3, what + ": overshoot");
    Check(a.bounceType == b.bounceType, what + ": bounceType");
    CheckNear(a.bounceAmount,  b.bounceAmount,  1e-3, what + ": bounceAmount");
    CheckNear(a.bounceCount,   b.bounceCount,   1e-3, what + ": bounceCount");
    CheckNear(a.bounceDamping, b.bounceDamping, 1e-3, what + ": bounceDamping");
    CheckNear(a.bounceStart,   b.bounceStart,   1e-3, what + ": bounceStart");
}

static void TestRoundTrip()
{
    std::printf("Round trip\n");

    const PresetData in = BusyPreset();

    PresetData out;
    std::string err;
    Check(FromJson(ToJson(in), out, err), "a written preset parses back: " + err);

    Check(out.name == in.name, "name survives");
    Check(out.wholeEffect == in.wholeEffect, "scope survives");
    CheckNear(out.sourceClipLength, in.sourceClipLength, 1e-3, "source clip length survives");
    Check(out.stageCount == in.stageCount, "stage count survives");
    Check(out.filterMode == in.filterMode, "filter survives");
    Check(out.edgeMode   == in.edgeMode,   "edge survives");
    Check(out.blurEnabled  == in.blurEnabled,  "blur enabled survives");
    CheckNear(out.shutterAngle, in.shutterAngle, 1e-3, "shutter angle survives");
    CheckNear(out.shutterPhase, in.shutterPhase, 1e-3, "shutter phase survives");
    Check(out.blurSamples  == in.blurSamples,  "blur samples survives");
    Check(out.blurAdaptive == in.blurAdaptive, "blur adaptive survives");

    CheckNear(out.base.scaleX,  in.base.scaleX,  1e-4, "base scale X survives");
    CheckNear(out.base.scaleY,  in.base.scaleY,  1e-4, "base scale Y survives");
    Check(out.base.linkScale == in.base.linkScale, "base link survives");
    CheckNear(out.base.posX,    in.base.posX,    1e-4, "base position X survives");
    CheckNear(out.base.posY,    in.base.posY,    1e-4, "base position Y survives");
    CheckNear(out.base.rot,     in.base.rot,     1e-3, "base rotation survives");
    CheckNear(out.base.tiltX,   in.base.tiltX,   1e-3, "base tilt survives");
    CheckNear(out.base.swivelY, in.base.swivelY, 1e-3, "base swivel survives");
    CheckNear(out.base.opacity, in.base.opacity, 1e-4, "base opacity survives");
    CheckNear(out.base.anchorX, in.base.anchorX, 1e-4, "base anchor X survives");
    CheckNear(out.base.anchorY, in.base.anchorY, 1e-4, "base anchor Y survives");

    for (int i = 0; i < kMaxStages; ++i)
        CheckStagesEqual(out.stages[i], in.stages[i], "stage " + std::to_string(i + 1));

    // Writing what was read must produce the identical file, or repeated
    // save/load cycles would drift.
    Check(ToJson(out) == ToJson(in), "re-serialising is byte-identical");
}

static void TestSingleStageScope()
{
    std::printf("Single-stage presets\n");

    PresetData in = BusyPreset();
    in.wholeEffect = false;

    const std::string json = ToJson(in);

    // A stage preset must not carry three unused stages around.
    size_t count = 0;
    for (size_t p = json.find("\"enabled\""); p != std::string::npos;
         p = json.find("\"enabled\"", p + 1)) ++count;
    Check(count == 1, "a stage preset writes exactly one stage");

    PresetData out;
    std::string err;
    Check(FromJson(json, out, err), "stage preset parses: " + err);
    Check(!out.wholeEffect, "scope reads back as a single stage");
    CheckStagesEqual(out.stages[0], in.stages[0], "the saved stage");
}

static void TestMalformedInputIsRejected()
{
    std::printf("Malformed input is rejected\n");

    const char* bad[] = {
        "",
        "   ",
        "not json at all",
        "{",
        "{ \"stages\": [",
        "{ \"stages\": [ { \"enabled\": true ",
        "{ \"name\": \"unterminated }",
        "[]",
        "{ \"name\": \"no stages here\" }",
    };

    for (const char* b : bad)
    {
        PresetData out = PresetData::Default();
        out.name = "SENTINEL";
        std::string err;

        const bool ok = FromJson(b, out, err);
        Check(!ok, std::string("rejects: ") + (b[0] ? b : "(empty)"));
        Check(!err.empty(), "a rejection explains itself");

        // The caller's data must be untouched, never half-populated.
        Check(out.name == "SENTINEL", "a failed parse leaves the target alone");
    }
}

static void TestMissingKeysUseDefaults()
{
    std::printf("Missing keys fall back to defaults\n");

    // Deterministic loading: the same file must produce the same result no
    // matter what the target held first, so absent keys resolve to defaults
    // rather than to whatever was already there.
    const std::string minimal =
        "{ \"stages\": [ { \"scaleTo\": 2.5 } ] }";

    PresetData out = BusyPreset();          // deliberately full of other values
    std::string err;
    Check(FromJson(minimal, out, err), "a minimal preset parses: " + err);

    const PresetStage def = PresetStage::Default();
    CheckNear(out.stages[0].scaleTo, 2.5, 1e-4, "the stated key is applied");
    CheckNear(out.stages[0].scaleFrom, def.scaleFrom, 1e-4, "an absent key uses the default");
    CheckNear(out.stages[0].easeIn,    def.easeIn,    1e-4, "absent easing uses the default");
    Check(out.stages[0].anchor == def.anchor, "an absent anchor uses the default");

    // Stages beyond those present likewise reset, rather than retaining the
    // previous setup's values.
    CheckNear(out.stages[3].scaleTo, def.scaleTo, 1e-4, "unlisted stages reset to defaults");
}

static void TestUnknownKeysIgnored()
{
    std::printf("Unknown keys are ignored\n");

    // A file written by a newer build must still load.
    const std::string future =
        "{ \"schemaVersion\": 99, \"somethingNew\": 1.5, "
        "\"nested\": { \"a\": [1,2,3] }, \"listy\": [{\"x\":1}], "
        "\"stages\": [ { \"scaleTo\": 3, \"futureField\": \"hello\" } ] }";

    PresetData out;
    std::string err;
    Check(FromJson(future, out, err), "a newer-schema preset still parses: " + err);
    CheckNear(out.stages[0].scaleTo, 3.0, 1e-4, "known keys still read correctly");
    Check(out.schemaVersion == 99, "the schema version is preserved");
}

static void TestPreBaseTransformPresetsStillLoad()
{
    std::printf("Presets written before the base transform still load\n");

    // A real file as written before split scale, orthographic rotation and the
    // base pose existed: no "base" object, and no scaleY/tilt/swivel keys. It
    // must load unchanged in meaning, which means every new channel lands
    // neutral rather than on whatever the target happened to hold.
    const std::string legacy =
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"name\": \"Punch In\",\n"
        "  \"scope\": \"effect\",\n"
        "  \"sourceClipLength\": 155,\n"
        "  \"stageCount\": 1,\n"
        "  \"stages\": [ { \"enabled\": true, \"scaleFrom\": 1, \"scaleTo\": 1.4,\n"
        "                  \"startFrame\": 0, \"endFrame\": 18 } ]\n"
        "}";

    PresetData out = BusyPreset();          // deliberately non-neutral to begin with
    std::string err;
    Check(FromJson(legacy, out, err), "a pre-change preset parses: " + err);

    CheckNear(out.stages[0].scaleTo, 1.4, 1e-4, "the old keys still mean what they did");

    // Split scale absent means the axes stay linked, so Scale Y is not consulted
    // at all and the move is uniform exactly as it was.
    Check(out.stages[0].linkScale, "an absent linkScale leaves the axes linked");
    CheckNear(out.stages[0].tiltXFrom,   0.0, 1e-4, "absent tilt is neutral");
    CheckNear(out.stages[0].tiltXTo,     0.0, 1e-4, "absent tilt is neutral at the end too");
    CheckNear(out.stages[0].swivelYFrom, 0.0, 1e-4, "absent swivel is neutral");
    CheckNear(out.stages[0].swivelYTo,   0.0, 1e-4, "absent swivel is neutral at the end too");

    Check(out.base.IsNeutral(), "an absent base object leaves the resting pose neutral");
    CheckNear(out.base.anchorX, 0.5, 1e-4, "an absent base anchor centres");
    CheckNear(out.base.anchorY, 0.5, 1e-4, "an absent base anchor centres vertically");
}

static void TestRescaleTiming()
{
    std::printf("Fit-to-clip rescaling\n");

    PresetData d = PresetData::Default();
    d.sourceClipLength = 155.0f;
    d.stageCount = 4;

    d.stages[0].anchor = kAnchorClipStart; d.stages[0].startFrame =   0.0f; d.stages[0].endFrame = 20.0f;
    d.stages[1].anchor = kAnchorClipEnd;   d.stages[1].startFrame = -18.0f; d.stages[1].endFrame =  0.0f;
    d.stages[2].anchor = kAnchorStretch;   d.stages[2].startFrame =   0.0f; d.stages[2].endFrame = 100.0f;
    d.stages[3].anchor = kAnchorTimeline;  d.stages[3].startFrame =  10.0f; d.stages[3].endFrame = 30.0f;

    RescaleTiming(d, 310.0f);               // exactly double

    CheckNear(d.stages[0].startFrame,  0.0, 1e-3, "clip-start scales: start");
    CheckNear(d.stages[0].endFrame,   40.0, 1e-3, "clip-start scales: end");
    CheckNear(d.stages[1].startFrame,-36.0, 1e-3, "clip-end scales: start");
    CheckNear(d.stages[1].endFrame,    0.0, 1e-3, "clip-end scales: end");

    // The whole point: Stretch is already proportional, so scaling it here
    // would apply the ratio twice and break the one anchor built for this.
    CheckNear(d.stages[2].startFrame,   0.0, 1e-3, "stretch is untouched: start");
    CheckNear(d.stages[2].endFrame,   100.0, 1e-3, "stretch is untouched: end");

    CheckNear(d.stages[3].endFrame,   60.0, 1e-3, "frame-based timeline stage scales");
    CheckNear(d.sourceClipLength,    310.0, 1e-3, "the recorded length is updated");
}

static void TestRescaleEdgeCases()
{
    std::printf("Rescale edge cases\n");

    // A short punch must not collapse to a zero-length instant cut when moved
    // to a much shorter clip.
    PresetData tiny = PresetData::Default();
    tiny.sourceClipLength = 300.0f;
    tiny.stages[0].anchor = kAnchorClipStart;
    tiny.stages[0].startFrame = 0.0f;
    tiny.stages[0].endFrame   = 4.0f;
    RescaleTiming(tiny, 10.0f);             // ratio 0.033, so 4 -> 0.13
    Check(std::fabs(tiny.stages[0].endFrame - tiny.stages[0].startFrame) >= 1.0f,
          "a collapsed span is clamped to at least one frame");

    // A backwards span keeps its direction when clamped.
    PresetData rev = PresetData::Default();
    rev.sourceClipLength = 300.0f;
    rev.stages[0].anchor = kAnchorClipEnd;
    rev.stages[0].startFrame = 4.0f;
    rev.stages[0].endFrame   = 0.0f;
    RescaleTiming(rev, 10.0f);
    Check(rev.stages[0].endFrame < rev.stages[0].startFrame, "a reversed span stays reversed");

    // Unknown lengths must leave everything exactly alone rather than scaling
    // by a guess or dividing by zero.
    for (float src : { 0.0f, -5.0f })
    {
        PresetData d = PresetData::Default();
        d.sourceClipLength = src;
        d.stages[0].startFrame = 3.0f;
        d.stages[0].endFrame   = 9.0f;
        RescaleTiming(d, 100.0f);
        CheckNear(d.stages[0].endFrame, 9.0, 1e-4, "unknown source length is a no-op");
    }

    PresetData d = PresetData::Default();
    d.sourceClipLength = 100.0f;
    d.stages[0].startFrame = 3.0f;
    d.stages[0].endFrame   = 9.0f;
    RescaleTiming(d, 0.0f);
    CheckNear(d.stages[0].endFrame, 9.0, 1e-4, "unknown target length is a no-op");
}

static void TestNameEscaping()
{
    std::printf("Preset names with awkward characters\n");

    PresetData in = PresetData::Default();
    in.name = "Zoom \"Hard\" \\ Fast\ttab";

    PresetData out;
    std::string err;
    Check(FromJson(ToJson(in), out, err), "a quoted name parses: " + err);
    Check(out.name == in.name, "quotes, backslashes and tabs survive");
}

////////////////////////////////////////////////////////////////////////////////

static void TestSettings()
{
    std::printf("Preferences round-trip and survive a bad file\n");

    Settings s;
    s.presetFolder = "D:\\Projects\\My Presets";

    Settings out;
    std::string err;
    Check(SettingsFromJson(SettingsToJson(s), out, err), "preferences parse back: " + err);
    Check(out.presetFolder == s.presetFolder, "the folder survives a round trip");

    // Backslashes are the whole point here: a Windows path is nothing but
    // escape characters as far as JSON is concerned, and getting this wrong
    // would corrupt the path a little further on every save.
    Check(SettingsToJson(s).find("\\\\") != std::string::npos,
          "backslashes are escaped on the way out");
    Check(SettingsToJson(out) == SettingsToJson(s), "re-serialising is byte-identical");

    // An empty folder means "use the default" and must not become a stored path.
    Settings empty;
    Check(SettingsFromJson(SettingsToJson(Settings::Default()), empty, err),
          "default preferences parse: " + err);
    Check(empty.presetFolder.empty(), "an unset folder stays unset");

    // A corrupt or truncated file falls back to defaults rather than taking the
    // plugin down. The caller's copy must be left alone, exactly as for presets.
    const char* bad[] = { "", "   ", "not json", "{", "{ \"presetFolder\": ", "[]" };
    for (const char* b : bad)
    {
        Settings target;
        target.presetFolder = "SENTINEL";
        std::string e;
        Check(!SettingsFromJson(b, target, e), std::string("rejects: ") + (b[0] ? b : "(empty)"));
        Check(target.presetFolder == "SENTINEL", "a failed parse leaves the target alone");
    }

    // A file from a newer build with keys this one does not know must still load.
    Settings future;
    Check(SettingsFromJson("{ \"presetFolder\": \"C:\\\\X\", \"somethingNew\": 3 }", future, err),
          "unknown preference keys are ignored: " + err);
    Check(future.presetFolder == "C:\\X", "known keys still read correctly");
}

int main()
{
    std::printf("=== MultiTransform preset tests ===\n");

    TestRoundTrip();
    TestSingleStageScope();
    TestMalformedInputIsRejected();
    TestMissingKeysUseDefaults();
    TestUnknownKeysIgnored();
    TestPreBaseTransformPresetsStillLoad();
    TestRescaleTiming();
    TestRescaleEdgeCases();
    TestNameEscaping();
    TestSettings();

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("ALL TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
