#include "Preset.h"

#include "Json.h"

#include <cmath>

namespace mtx {

////////////////////////////////////////////////////////////////////////////////
// Defaults

PresetStage PresetStage::Default()
{
    // Mirrors Stage::Default() so a preset missing a key lands on the same value
    // a fresh stage would have.
    const Stage s = Stage::Default();

    PresetStage p;
    p.enabled      = s.enabled;
    p.anchor       = s.anchor;
    p.startFrame   = s.startFrame;
    p.endFrame     = s.endFrame;
    p.scaleFrom    = s.scaleFrom;    p.scaleTo    = s.scaleTo;
    p.scaleYFrom   = s.scaleYFrom;   p.scaleYTo   = s.scaleYTo;
    p.linkScale    = s.linkScale;
    p.posXFrom     = s.posXFrom;     p.posYFrom   = s.posYFrom;
    p.posXTo       = s.posXTo;       p.posYTo     = s.posYTo;
    p.rotFrom      = s.rotFrom;      p.rotTo      = s.rotTo;
    p.tiltXFrom    = s.tiltXFrom;    p.tiltXTo    = s.tiltXTo;
    p.swivelYFrom  = s.swivelYFrom;  p.swivelYTo  = s.swivelYTo;
    p.opacityFrom  = s.opacityFrom;  p.opacityTo  = s.opacityTo;
    p.anchorX      = s.anchorX;      p.anchorY    = s.anchorY;
    p.pathC1X      = s.pathC1X;      p.pathC1Y    = s.pathC1Y;
    p.pathC2X      = s.pathC2X;      p.pathC2Y    = s.pathC2Y;

    // Easing is stored as the human-facing amounts, not the derived bezier
    // handles, so a preset stays readable and survives changes to the mapping.
    p.easingPreset = 1;      // Smooth
    p.easeIn       = 42.0f;
    p.easeOut      = 42.0f;
    p.anticipation = 0.0f;
    p.overshoot    = 0.0f;
    p.bounceType   = kBounceNone;
    p.bounceAmount = 35.0f;
    p.bounceCount  = 3.0f;
    p.bounceDamping = 45.0f;
    p.bounceStart  = 55.0f;
    return p;
}

PresetData PresetData::Default()
{
    PresetData d;
    d.schemaVersion    = kPresetSchemaVersion;
    d.name             = "";
    d.wholeEffect      = true;
    d.sourceClipLength = 0.0f;
    d.stageCount       = 1;
    d.filterMode       = 1;      // bilinear
    d.edgeMode         = 0;      // transparent
    d.blurEnabled      = false;
    d.shutterAngle     = 180.0f;
    d.shutterPhase     = 0.0f;
    d.blurSamples      = 16;
    d.blurAdaptive     = true;
    d.base             = BasePose::Default();
    for (int i = 0; i < kMaxStages; ++i) d.stages[i] = PresetStage::Default();
    return d;
}

////////////////////////////////////////////////////////////////////////////////
// Writing

namespace {

using json::Num;
using json::Escape;

void WriteStage(std::string& o, const PresetStage& s, const char* indent)
{
    const std::string i2 = std::string(indent) + "  ";
    const auto kv = [&](const char* k, const std::string& v, bool last = false)
    {
        o += i2 + "\"" + k + "\": " + v + (last ? "\n" : ",\n");
    };

    o += std::string(indent) + "{\n";
    kv("enabled",      s.enabled ? "true" : "false");
    kv("anchor",       Num(s.anchor));
    kv("startFrame",   Num(s.startFrame));
    kv("endFrame",     Num(s.endFrame));
    kv("scaleFrom",    Num(s.scaleFrom));
    kv("scaleTo",      Num(s.scaleTo));
    kv("scaleYFrom",   Num(s.scaleYFrom));
    kv("scaleYTo",     Num(s.scaleYTo));
    kv("linkScale",    s.linkScale ? "true" : "false");
    kv("posXFrom",     Num(s.posXFrom));
    kv("posYFrom",     Num(s.posYFrom));
    kv("posXTo",       Num(s.posXTo));
    kv("posYTo",       Num(s.posYTo));
    kv("rotFrom",      Num(s.rotFrom));
    kv("rotTo",        Num(s.rotTo));
    kv("tiltXFrom",    Num(s.tiltXFrom));
    kv("tiltXTo",      Num(s.tiltXTo));
    kv("swivelYFrom",  Num(s.swivelYFrom));
    kv("swivelYTo",    Num(s.swivelYTo));
    kv("opacityFrom",  Num(s.opacityFrom));
    kv("opacityTo",    Num(s.opacityTo));
    kv("anchorX",      Num(s.anchorX));
    kv("anchorY",      Num(s.anchorY));
    kv("pathC1X",      Num(s.pathC1X));
    kv("pathC1Y",      Num(s.pathC1Y));
    kv("pathC2X",      Num(s.pathC2X));
    kv("pathC2Y",      Num(s.pathC2Y));
    kv("easingPreset", Num(s.easingPreset));
    kv("easeIn",       Num(s.easeIn));
    kv("easeOut",      Num(s.easeOut));
    kv("anticipation", Num(s.anticipation));
    kv("overshoot",    Num(s.overshoot));
    kv("bounceType",   Num(s.bounceType));
    kv("bounceAmount", Num(s.bounceAmount));
    kv("bounceCount",  Num(s.bounceCount));
    kv("bounceDamping",Num(s.bounceDamping));
    kv("bounceStart",  Num(s.bounceStart), true);
    o += std::string(indent) + "}";
}

} // namespace

std::string ToJson(const PresetData& d)
{
    std::string o;
    o += "{\n";
    o += "  \"schemaVersion\": "    + Num(d.schemaVersion) + ",\n";
    o += "  \"name\": \""           + Escape(d.name) + "\",\n";
    o += "  \"scope\": \""          + std::string(d.wholeEffect ? "effect" : "stage") + "\",\n";
    o += "  \"sourceClipLength\": " + Num(d.sourceClipLength) + ",\n";
    o += "  \"stageCount\": "       + Num(d.stageCount) + ",\n";
    o += "  \"filterMode\": "       + Num(d.filterMode) + ",\n";
    o += "  \"edgeMode\": "         + Num(d.edgeMode) + ",\n";
    o += "  \"blurEnabled\": "      + std::string(d.blurEnabled ? "true" : "false") + ",\n";
    o += "  \"shutterAngle\": "     + Num(d.shutterAngle) + ",\n";
    o += "  \"shutterPhase\": "     + Num(d.shutterPhase) + ",\n";
    o += "  \"blurSamples\": "      + Num(d.blurSamples) + ",\n";
    o += "  \"blurAdaptive\": "     + std::string(d.blurAdaptive ? "true" : "false") + ",\n";

    // The resting pose, nested rather than flattened, so its Scale and Rotation
    // are never confused with a stage's when reading the file by eye.
    o += "  \"base\": {\n";
    o += "    \"scaleX\": "    + Num(d.base.scaleX) + ",\n";
    o += "    \"scaleY\": "    + Num(d.base.scaleY) + ",\n";
    o += "    \"linkScale\": " + std::string(d.base.linkScale ? "true" : "false") + ",\n";
    o += "    \"posX\": "      + Num(d.base.posX) + ",\n";
    o += "    \"posY\": "      + Num(d.base.posY) + ",\n";
    o += "    \"rot\": "       + Num(d.base.rot) + ",\n";
    o += "    \"tiltX\": "     + Num(d.base.tiltX) + ",\n";
    o += "    \"swivelY\": "   + Num(d.base.swivelY) + ",\n";
    o += "    \"opacity\": "   + Num(d.base.opacity) + ",\n";
    o += "    \"anchorX\": "   + Num(d.base.anchorX) + ",\n";
    o += "    \"anchorY\": "   + Num(d.base.anchorY) + "\n";
    o += "  },\n";

    // A single-stage preset writes one entry, so the file says plainly what it
    // contains rather than carrying three unused stages.
    const int count = d.wholeEffect ? kMaxStages : 1;
    o += "  \"stages\": [\n";
    for (int i = 0; i < count; ++i)
    {
        WriteStage(o, d.stages[i], "    ");
        o += (i + 1 < count) ? ",\n" : "\n";
    }
    o += "  ]\n";
    o += "}\n";
    return o;
}

////////////////////////////////////////////////////////////////////////////////
// Reading
//
// The reader itself lives in Json.h, shared with the preferences file. What is
// here is only the mapping from keys to this schema.

namespace {

using json::Fields;
using json::Reader;
using json::ReadFlatObject;
using json::FieldNum;
using json::FieldBool;
using json::FieldStr;

PresetStage StageFromFields(const Fields& f)
{
    PresetStage s = PresetStage::Default();
    s.enabled       = FieldBool(f, "enabled", s.enabled);
    s.anchor        = static_cast<int>(FieldNum(f, "anchor", s.anchor));
    s.startFrame    = static_cast<float>(FieldNum(f, "startFrame", s.startFrame));
    s.endFrame      = static_cast<float>(FieldNum(f, "endFrame", s.endFrame));
    s.scaleFrom     = static_cast<float>(FieldNum(f, "scaleFrom", s.scaleFrom));
    s.scaleTo       = static_cast<float>(FieldNum(f, "scaleTo", s.scaleTo));
    s.scaleYFrom    = static_cast<float>(FieldNum(f, "scaleYFrom", s.scaleYFrom));
    s.scaleYTo      = static_cast<float>(FieldNum(f, "scaleYTo", s.scaleYTo));
    s.linkScale     = FieldBool(f, "linkScale", s.linkScale);
    s.posXFrom      = static_cast<float>(FieldNum(f, "posXFrom", s.posXFrom));
    s.posYFrom      = static_cast<float>(FieldNum(f, "posYFrom", s.posYFrom));
    s.posXTo        = static_cast<float>(FieldNum(f, "posXTo", s.posXTo));
    s.posYTo        = static_cast<float>(FieldNum(f, "posYTo", s.posYTo));
    s.rotFrom       = static_cast<float>(FieldNum(f, "rotFrom", s.rotFrom));
    s.rotTo         = static_cast<float>(FieldNum(f, "rotTo", s.rotTo));
    s.tiltXFrom     = static_cast<float>(FieldNum(f, "tiltXFrom", s.tiltXFrom));
    s.tiltXTo       = static_cast<float>(FieldNum(f, "tiltXTo", s.tiltXTo));
    s.swivelYFrom   = static_cast<float>(FieldNum(f, "swivelYFrom", s.swivelYFrom));
    s.swivelYTo     = static_cast<float>(FieldNum(f, "swivelYTo", s.swivelYTo));
    s.opacityFrom   = static_cast<float>(FieldNum(f, "opacityFrom", s.opacityFrom));
    s.opacityTo     = static_cast<float>(FieldNum(f, "opacityTo", s.opacityTo));
    s.anchorX       = static_cast<float>(FieldNum(f, "anchorX", s.anchorX));
    s.anchorY       = static_cast<float>(FieldNum(f, "anchorY", s.anchorY));
    s.pathC1X       = static_cast<float>(FieldNum(f, "pathC1X", s.pathC1X));
    s.pathC1Y       = static_cast<float>(FieldNum(f, "pathC1Y", s.pathC1Y));
    s.pathC2X       = static_cast<float>(FieldNum(f, "pathC2X", s.pathC2X));
    s.pathC2Y       = static_cast<float>(FieldNum(f, "pathC2Y", s.pathC2Y));
    s.easingPreset  = static_cast<int>(FieldNum(f, "easingPreset", s.easingPreset));
    s.easeIn        = static_cast<float>(FieldNum(f, "easeIn", s.easeIn));
    s.easeOut       = static_cast<float>(FieldNum(f, "easeOut", s.easeOut));
    s.anticipation  = static_cast<float>(FieldNum(f, "anticipation", s.anticipation));
    s.overshoot     = static_cast<float>(FieldNum(f, "overshoot", s.overshoot));
    s.bounceType    = static_cast<int>(FieldNum(f, "bounceType", s.bounceType));
    s.bounceAmount  = static_cast<float>(FieldNum(f, "bounceAmount", s.bounceAmount));
    s.bounceCount   = static_cast<float>(FieldNum(f, "bounceCount", s.bounceCount));
    s.bounceDamping = static_cast<float>(FieldNum(f, "bounceDamping", s.bounceDamping));
    s.bounceStart   = static_cast<float>(FieldNum(f, "bounceStart", s.bounceStart));
    return s;
}

} // namespace

bool FromJson(const std::string& json, PresetData& out, std::string& error)
{
    // Built into a local and only copied out on success, so a failed parse
    // cannot leave the caller holding a half-populated preset.
    PresetData d = PresetData::Default();

    Reader r(json);
    if (r.eof()) { error = "the file is empty"; return false; }
    if (!r.expect('{')) { error = r.err; return false; }

    bool sawStages = false;

    for (;;)
    {
        if (r.peek() == '}') { ++r.i; break; }

        std::string key;
        if (!r.readString(key)) { error = r.err; return false; }
        if (!r.expect(':'))     { error = r.err; return false; }

        if (key == "stages")
        {
            if (!r.expect('[')) { error = r.err; return false; }
            int n = 0;
            if (r.peek() == ']') ++r.i;
            else for (;;)
            {
                Fields f;
                if (!ReadFlatObject(r, f)) { error = r.err; return false; }
                if (n < kMaxStages) d.stages[n] = StageFromFields(f);
                ++n;

                r.skipWs();
                if (r.peek() == ',') { ++r.i; continue; }
                if (r.peek() == ']') { ++r.i; break; }
                error = "expected ',' or ']' in stages at offset " + std::to_string(r.i);
                return false;
            }
            sawStages = true;
        }
        else if (key == "base")
        {
            Fields f;
            if (!ReadFlatObject(r, f)) { error = r.err; return false; }

            BasePose& b = d.base;
            b.scaleX    = static_cast<float>(FieldNum(f, "scaleX", b.scaleX));
            b.scaleY    = static_cast<float>(FieldNum(f, "scaleY", b.scaleY));
            b.linkScale = FieldBool(f, "linkScale", b.linkScale);
            b.posX      = static_cast<float>(FieldNum(f, "posX", b.posX));
            b.posY      = static_cast<float>(FieldNum(f, "posY", b.posY));
            b.rot       = static_cast<float>(FieldNum(f, "rot", b.rot));
            b.tiltX     = static_cast<float>(FieldNum(f, "tiltX", b.tiltX));
            b.swivelY   = static_cast<float>(FieldNum(f, "swivelY", b.swivelY));
            b.opacity   = static_cast<float>(FieldNum(f, "opacity", b.opacity));
            b.anchorX   = static_cast<float>(FieldNum(f, "anchorX", b.anchorX));
            b.anchorY   = static_cast<float>(FieldNum(f, "anchorY", b.anchorY));
        }
        else
        {
            r.skipWs();
            const size_t valueStart = r.i;
            if (!r.skipValue()) { error = r.err; return false; }
            const std::string raw = json.substr(valueStart, r.i - valueStart);

            Fields one; one[key] = raw;
            if      (key == "schemaVersion")    d.schemaVersion    = static_cast<int>(FieldNum(one, key.c_str(), d.schemaVersion));
            else if (key == "name")             d.name             = FieldStr(one, key.c_str(), d.name);
            else if (key == "scope")            d.wholeEffect      = (FieldStr(one, key.c_str(), "effect") != "stage");
            else if (key == "sourceClipLength") d.sourceClipLength = static_cast<float>(FieldNum(one, key.c_str(), d.sourceClipLength));
            else if (key == "stageCount")       d.stageCount       = static_cast<int>(FieldNum(one, key.c_str(), d.stageCount));
            else if (key == "filterMode")       d.filterMode       = static_cast<int>(FieldNum(one, key.c_str(), d.filterMode));
            else if (key == "edgeMode")         d.edgeMode         = static_cast<int>(FieldNum(one, key.c_str(), d.edgeMode));
            else if (key == "blurEnabled")      d.blurEnabled      = FieldBool(one, key.c_str(), d.blurEnabled);
            else if (key == "shutterAngle")     d.shutterAngle     = static_cast<float>(FieldNum(one, key.c_str(), d.shutterAngle));
            else if (key == "shutterPhase")     d.shutterPhase     = static_cast<float>(FieldNum(one, key.c_str(), d.shutterPhase));
            else if (key == "blurSamples")      d.blurSamples      = static_cast<int>(FieldNum(one, key.c_str(), d.blurSamples));
            else if (key == "blurAdaptive")     d.blurAdaptive     = FieldBool(one, key.c_str(), d.blurAdaptive);
            // anything else: an unknown key from a newer build, ignored.
        }

        r.skipWs();
        if (r.peek() == ',') { ++r.i; continue; }
        if (r.peek() == '}') { ++r.i; break; }
        error = "expected ',' or '}' at offset " + std::to_string(r.i);
        return false;
    }

    if (!sawStages) { error = "no \"stages\" array -- this is not a Multi Transform preset"; return false; }

    // Guard against a file that would otherwise silently disable everything.
    if (d.stageCount < 1)          d.stageCount = 1;
    if (d.stageCount > kMaxStages) d.stageCount = kMaxStages;

    out = d;
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// Rescaling

void RescaleTiming(PresetData& d, float targetClipLength)
{
    // Scaling by a guessed factor would quietly mis-time everything, so an
    // unknown length on either side means leave the values exactly alone.
    if (!(d.sourceClipLength > 0.0f) || !(targetClipLength > 0.0f)) return;

    const float k = targetClipLength / d.sourceClipLength;

    for (int i = 0; i < kMaxStages; ++i)
    {
        PresetStage& s = d.stages[i];

        // A Stretch stage stores percentages and is already proportional to
        // whatever clip it lands on. Scaling it here would apply the ratio a
        // second time and break the one anchor built to be length-independent.
        if (s.anchor == kAnchorStretch) continue;

        const float start = s.startFrame * k;
        float       end   = s.endFrame   * k;

        // Preserve the direction of the span while enforcing a floor of one
        // frame, so a short punch does not collapse into an instant cut.
        const float span = end - start;
        if (std::fabs(span) < 1.0f) end = start + (span < 0.0f ? -1.0f : 1.0f);

        s.startFrame = start;
        s.endFrame   = end;
    }

    d.sourceClipLength = targetClipLength;
}

} // namespace mtx
