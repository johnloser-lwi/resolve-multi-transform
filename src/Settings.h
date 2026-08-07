#pragma once

// Per-user preferences.
//
// Distinct from everything else this plugin stores, and the distinction is the
// whole point: an OFX parameter lives inside the *project*, so it travels with
// the timeline and belongs to one instance of the effect. "Where my presets
// live" is neither of those things -- it belongs to the person and the machine,
// and it has to apply to every instance ever created, including ones in
// projects that do not exist yet. That cannot be a parameter. It has to be a
// file the plugin reads for itself.
//
// Serialisation is kept free of any Win32 or OFX dependency so it can be unit
// tested, exactly as Preset.cpp is. The file paths and the folder picker live
// in PresetIO.cpp with the other platform code.

#include <string>

namespace mtx {

struct Settings
{
    /// Where the preset dialogs open. Empty means "use the built-in default",
    /// which is deliberately not the same as storing the default path: a
    /// recorded path would go stale if the default ever moved, or if the file
    /// were copied to a machine where that folder does not exist.
    std::string presetFolder;

    static Settings Default() { return Settings{ std::string() }; }
};

/** @brief Serialise to JSON. Round-trips exactly through FromJson. */
std::string SettingsToJson(const Settings& s);

/** @brief Parse the preferences file.
 *
 * Unknown keys are ignored so an older build can read a newer file, and missing
 * keys resolve to defaults. On a parse failure this returns false and leaves
 * @p out untouched -- a corrupt preferences file falls back to defaults rather
 * than taking the plugin down with it.
 */
bool SettingsFromJson(const std::string& jsonText, Settings& out, std::string& error);

} // namespace mtx
