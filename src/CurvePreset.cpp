#include "CurvePreset.h"

#include "Json.h"
#include "ParamNames.h"

namespace mtx {

CurvePreset CurvePreset::Default()
{
    // Matches the Smooth easing preset, so a curve file missing every key reads
    // as the same curve a fresh stage would have rather than as something new.
    const EasingPresetValues v = PresetValues(kEasingSmooth);

    CurvePreset c;
    c.name          = "";
    c.easeIn        = v.easeIn;
    c.easeOut       = v.easeOut;
    c.anticipation  = v.anticipation;
    c.overshoot     = v.overshoot;
    c.bounceType    = v.bounceType;
    c.bounceAmount  = v.bounceAmount;
    c.bounceCount   = v.bounceCount;
    c.bounceDamping = v.bounceDamping;
    c.bounceStart   = v.bounceStart;
    return c;
}

Easing CurvePreset::ToEasing() const
{
    // The same construction the renderer uses, so a library thumbnail is drawn
    // by exactly the code that will animate the frame.
    return MakeEasing(easeIn, easeOut, anticipation, overshoot,
                      bounceType, bounceAmount, bounceCount, bounceDamping, bounceStart);
}

std::string SanitiseFileName(const std::string& name)
{
    static const std::string kIllegal = "\\/:*?\"<>|";

    std::string out;
    for (char ch : name)
    {
        if (static_cast<unsigned char>(ch) < 0x20) continue;
        if (kIllegal.find(ch) != std::string::npos) continue;
        out += ch;
    }

    // Leading and trailing spaces and dots are legal to type and illegal to
    // store, so a name of "..." must come back empty rather than unwritable.
    const size_t first = out.find_first_not_of(" .");
    if (first == std::string::npos) return "";
    const size_t last = out.find_last_not_of(" .");
    return out.substr(first, last - first + 1);
}

std::string CurveToJson(const CurvePreset& c)
{
    std::string o;
    o += "{\n";
    o += "  \"type\": \""          + std::string(kCurveTypeTag) + "\",\n";
    o += "  \"name\": \""          + json::Escape(c.name) + "\",\n";
    o += "  \"easeIn\": "          + json::Num(c.easeIn) + ",\n";
    o += "  \"easeOut\": "         + json::Num(c.easeOut) + ",\n";
    o += "  \"anticipation\": "    + json::Num(c.anticipation) + ",\n";
    o += "  \"overshoot\": "       + json::Num(c.overshoot) + ",\n";
    o += "  \"bounceType\": "      + json::Num(c.bounceType) + ",\n";
    o += "  \"bounceAmount\": "    + json::Num(c.bounceAmount) + ",\n";
    o += "  \"bounceCount\": "     + json::Num(c.bounceCount) + ",\n";
    o += "  \"bounceDamping\": "   + json::Num(c.bounceDamping) + ",\n";
    o += "  \"bounceStart\": "     + json::Num(c.bounceStart) + "\n";
    o += "}\n";
    return o;
}

bool CurveFromJson(const std::string& jsonText, CurvePreset& out, std::string& error)
{
    CurvePreset c = CurvePreset::Default();

    json::Reader r(jsonText);
    if (r.eof()) { error = "the file is empty"; return false; }

    json::Fields f;
    if (!json::ReadFlatObject(r, f)) { error = r.err; return false; }

    // The type tag is checked before anything is read out. An effect preset has
    // no easing keys at its top level, so without this it would parse happily
    // and produce a default curve -- an empty success, which is the worst
    // possible answer.
    if (json::FieldStr(f, "type", "") != kCurveTypeTag)
    {
        error = "this is not a curve file";
        return false;
    }

    c.name          = json::FieldStr(f, "name", c.name);
    c.easeIn        = static_cast<float>(json::FieldNum(f, "easeIn", c.easeIn));
    c.easeOut       = static_cast<float>(json::FieldNum(f, "easeOut", c.easeOut));
    c.anticipation  = static_cast<float>(json::FieldNum(f, "anticipation", c.anticipation));
    c.overshoot     = static_cast<float>(json::FieldNum(f, "overshoot", c.overshoot));
    c.bounceType    = static_cast<int>(json::FieldNum(f, "bounceType", c.bounceType));
    c.bounceAmount  = static_cast<float>(json::FieldNum(f, "bounceAmount", c.bounceAmount));
    c.bounceCount   = static_cast<float>(json::FieldNum(f, "bounceCount", c.bounceCount));
    c.bounceDamping = static_cast<float>(json::FieldNum(f, "bounceDamping", c.bounceDamping));
    c.bounceStart   = static_cast<float>(json::FieldNum(f, "bounceStart", c.bounceStart));

    out = c;
    return true;
}

} // namespace mtx
