#include "Preset.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

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
    p.posXFrom     = s.posXFrom;     p.posYFrom   = s.posYFrom;
    p.posXTo       = s.posXTo;       p.posYTo     = s.posYTo;
    p.rotFrom      = s.rotFrom;      p.rotTo      = s.rotTo;
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
    for (int i = 0; i < kMaxStages; ++i) d.stages[i] = PresetStage::Default();
    return d;
}

////////////////////////////////////////////////////////////////////////////////
// Writing

namespace {

std::string Num(double v)
{
    // %.6g keeps the files readable and round-trips a float exactly, without
    // emitting the long decimal tails that make a diff unreadable.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

std::string EscapeString(const std::string& s)
{
    std::string out;
    for (char ch : s)
    {
        switch (ch)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                // Control characters would produce invalid JSON; drop them
                // rather than emitting a file we cannot read back.
                if (static_cast<unsigned char>(ch) >= 0x20) out += ch;
                break;
        }
    }
    return out;
}

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
    kv("posXFrom",     Num(s.posXFrom));
    kv("posYFrom",     Num(s.posYFrom));
    kv("posXTo",       Num(s.posXTo));
    kv("posYTo",       Num(s.posYTo));
    kv("rotFrom",      Num(s.rotFrom));
    kv("rotTo",        Num(s.rotTo));
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
    o += "  \"name\": \""           + EscapeString(d.name) + "\",\n";
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
// A deliberately small recursive-descent reader. It accepts the subset this
// plugin emits and rejects everything else with a message: a preset that half
// loads would leave a setup in a state that never existed, which is harder to
// notice and harder to undo than an outright refusal.

namespace {

struct Reader
{
    const std::string& s;
    size_t i = 0;
    std::string err;

    explicit Reader(const std::string& src) : s(src) {}

    void skipWs() { while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) ++i; }
    bool eof()    { skipWs(); return i >= s.size(); }
    char peek()   { skipWs(); return i < s.size() ? s[i] : '\0'; }

    bool expect(char c)
    {
        skipWs();
        if (i < s.size() && s[i] == c) { ++i; return true; }
        err = std::string("expected '") + c + "' at offset " + std::to_string(i);
        return false;
    }

    bool readString(std::string& out)
    {
        skipWs();
        if (i >= s.size() || s[i] != '"') { err = "expected a string at offset " + std::to_string(i); return false; }
        ++i;
        out.clear();
        while (i < s.size() && s[i] != '"')
        {
            if (s[i] == '\\')
            {
                if (++i >= s.size()) { err = "truncated escape"; return false; }
                switch (s[i])
                {
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case '"': out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/';  break;
                    default: err = "unsupported escape"; return false;
                }
                ++i;
            }
            else out += s[i++];
        }
        if (i >= s.size()) { err = "unterminated string"; return false; }
        ++i;
        return true;
    }

    bool readNumber(double& out)
    {
        skipWs();
        const size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        bool digits = false;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i]=='.' || s[i]=='e' || s[i]=='E'
                                || ((s[i]=='-'||s[i]=='+') && (s[i-1]=='e'||s[i-1]=='E'))))
        {
            if (s[i] >= '0' && s[i] <= '9') digits = true;
            ++i;
        }
        if (!digits) { err = "expected a number at offset " + std::to_string(start); return false; }

        out = std::strtod(s.substr(start, i - start).c_str(), nullptr);
        return true;
    }

    /** Skip one value of any type, so unknown keys do not break the parse. */
    bool skipValue()
    {
        skipWs();
        if (i >= s.size()) { err = "unexpected end of input"; return false; }

        const char c = s[i];
        if (c == '"') { std::string t; return readString(t); }
        if (c == '{' || c == '[')
        {
            const char close = (c == '{') ? '}' : ']';
            int depth = 0;
            while (i < s.size())
            {
                if (s[i] == '"') { std::string t; if (!readString(t)) return false; continue; }
                if (s[i] == c)     ++depth;
                if (s[i] == close) { if (--depth == 0) { ++i; return true; } }
                ++i;
            }
            err = "unterminated object or array";
            return false;
        }
        if (s.compare(i, 4, "true") == 0)  { i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; return true; }
        if (s.compare(i, 4, "null") == 0)  { i += 4; return true; }

        double d = 0.0;
        return readNumber(d);
    }
};

/// One flat object of key -> raw scalar, which is all the schema needs.
using Fields = std::map<std::string, std::string>;

bool ReadFlatObject(Reader& r, Fields& out)
{
    if (!r.expect('{')) return false;
    if (r.peek() == '}') { ++r.i; return true; }

    for (;;)
    {
        std::string key;
        if (!r.readString(key)) return false;
        if (!r.expect(':')) return false;

        r.skipWs();
        const size_t valueStart = r.i;
        if (!r.skipValue()) return false;

        // Stored raw; conversion happens at the point of use so a wrong type is
        // caught with the key name rather than as a bare offset.
        out[key] = r.s.substr(valueStart, r.i - valueStart);

        r.skipWs();
        if (r.peek() == ',') { ++r.i; continue; }
        if (r.peek() == '}') { ++r.i; return true; }
        r.err = "expected ',' or '}' at offset " + std::to_string(r.i);
        return false;
    }
}

double FieldNum(const Fields& f, const char* key, double fallback)
{
    const auto it = f.find(key);
    if (it == f.end() || it->second.empty()) return fallback;
    if (it->second[0] == '"') return fallback;      // a string where a number belongs
    return std::strtod(it->second.c_str(), nullptr);
}

bool FieldBool(const Fields& f, const char* key, bool fallback)
{
    const auto it = f.find(key);
    if (it == f.end()) return fallback;
    if (it->second == "true")  return true;
    if (it->second == "false") return false;
    return fallback;
}

/** @brief Decode a raw JSON string token, quotes included, back to its text.
 *
 * Stripping the quotes is not enough: the stored token is the raw source, so a
 * name containing a quote or a tab would otherwise come back with its backslash
 * escapes intact and drift a little further every save/load cycle.
 */
std::string Unescape(const std::string& quoted)
{
    std::string out;
    for (size_t i = 1; i + 1 < quoted.size(); ++i)
    {
        if (quoted[i] != '\\') { out += quoted[i]; continue; }
        if (i + 2 >= quoted.size() + 1) break;

        switch (quoted[++i])
        {
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            default:   out += quoted[i]; break;
        }
    }
    return out;
}

std::string FieldStr(const Fields& f, const char* key, const std::string& fallback)
{
    const auto it = f.find(key);
    if (it == f.end() || it->second.size() < 2 || it->second[0] != '"') return fallback;
    return Unescape(it->second);
}

PresetStage StageFromFields(const Fields& f)
{
    PresetStage s = PresetStage::Default();
    s.enabled       = FieldBool(f, "enabled", s.enabled);
    s.anchor        = static_cast<int>(FieldNum(f, "anchor", s.anchor));
    s.startFrame    = static_cast<float>(FieldNum(f, "startFrame", s.startFrame));
    s.endFrame      = static_cast<float>(FieldNum(f, "endFrame", s.endFrame));
    s.scaleFrom     = static_cast<float>(FieldNum(f, "scaleFrom", s.scaleFrom));
    s.scaleTo       = static_cast<float>(FieldNum(f, "scaleTo", s.scaleTo));
    s.posXFrom      = static_cast<float>(FieldNum(f, "posXFrom", s.posXFrom));
    s.posYFrom      = static_cast<float>(FieldNum(f, "posYFrom", s.posYFrom));
    s.posXTo        = static_cast<float>(FieldNum(f, "posXTo", s.posXTo));
    s.posYTo        = static_cast<float>(FieldNum(f, "posYTo", s.posYTo));
    s.rotFrom       = static_cast<float>(FieldNum(f, "rotFrom", s.rotFrom));
    s.rotTo         = static_cast<float>(FieldNum(f, "rotTo", s.rotTo));
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
