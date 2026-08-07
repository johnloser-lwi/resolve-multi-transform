#include "Settings.h"

#include "Json.h"

namespace mtx {

std::string SettingsToJson(const Settings& s)
{
    std::string o;
    o += "{\n";
    o += "  \"presetFolder\": \"" + json::Escape(s.presetFolder) + "\"\n";
    o += "}\n";
    return o;
}

bool SettingsFromJson(const std::string& jsonText, Settings& out, std::string& error)
{
    // Built into a local and only copied out on success, so a failed parse
    // cannot leave the caller holding half a preferences file.
    Settings s = Settings::Default();

    json::Reader r(jsonText);
    if (r.eof()) { error = "the file is empty"; return false; }

    json::Fields f;
    if (!json::ReadFlatObject(r, f)) { error = r.err; return false; }

    s.presetFolder = json::FieldStr(f, "presetFolder", s.presetFolder);

    out = s;
    return true;
}

} // namespace mtx
