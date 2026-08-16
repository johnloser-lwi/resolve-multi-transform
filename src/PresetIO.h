#pragma once

#include <string>
#include <vector>

#include "CurvePreset.h"
#include "Settings.h"

namespace mtx {

/** @brief Where saved curves live: a `Curves` subfolder of the preset folder.
 *
 *  Separate from the effect and stage presets deliberately. Every file this
 *  plugin writes is a `.json`, so a flat folder gives no clue which of them is
 *  a whole setup and which is nine numbers describing an ease -- and the curve
 *  library has to be able to list one without the other. Created on demand,
 *  and it follows the preset folder if that is moved. */
std::string CurveFolder();

/** @brief Every `.json` in @p folder, as full paths, sorted by name.
 *  Empty if the folder does not exist. */
std::vector<std::string> ListJsonFiles(const std::string& folder);


/** @brief Where the preset dialogs open.
 *
 *  The folder set in preferences if there is one and it still exists, otherwise
 *  DefaultPresetFolder(). A configured folder that has since been deleted or
 *  unplugged falls back rather than sending the dialog somewhere that is not
 *  there. */
std::string PresetFolder();

/** @brief Built-in default, created on demand.
 *
 *  `Documents\MultiTransform\Presets` -- somewhere the user can actually browse
 *  to, copy from and back up. Falls back to LocalAppData only if Documents
 *  cannot be resolved at all. */
std::string DefaultPresetFolder();

/** @brief Ask the user to pick a folder. False if cancelled.
 *  @param current folder the picker starts in; may be empty. */
bool ChooseFolder(const std::string& current, std::string& outPath);

/** @brief Where Copy All Settings puts the effect, and Paste reads it from.
 *
 *  `%LOCALAPPDATA%\MultiTransform\clipboard.json` -- beside the preferences
 *  rather than with the presets, because it is scratch state and not something
 *  to browse or keep.
 *
 *  A file rather than memory so it survives a restart, and so a copy taken
 *  before Resolve was closed is still there afterwards. It also means the copy
 *  outlives the plugin being unloaded and reloaded, which an in-process
 *  clipboard would not. */
std::string EffectClipboardPath();

/** @brief Preferences file, `%LOCALAPPDATA%\MultiTransform\settings.json`.
 *
 *  Stays in LocalAppData even though presets moved to Documents: this one
 *  really is internal bookkeeping, and it is per-machine by definition. */
std::string SettingsFilePath();

/** @brief Read preferences, or defaults if the file is absent or unreadable. */
Settings LoadSettings();

/** @brief Write preferences. False with a message if it cannot be written. */
bool SaveSettings(const Settings& s, std::string& error);

/** @brief Ask the user for a preset file to read.
 *  @return false if the dialog was cancelled. */
bool ChoosePresetToOpen(std::string& outPath);

/** @brief Ask the user where to write a preset.
 *  @param suggestedName file name offered in the dialog, without extension.
 *  @return false if the dialog was cancelled. */
bool ChoosePresetToSave(const std::string& suggestedName, std::string& outPath);

/** @brief Whole-file read. False with a message if the file cannot be read. */
bool ReadTextFile(const std::string& path, std::string& outText, std::string& error);

/** @brief Whole-file write. False with a message if it cannot be written. */
bool WriteTextFile(const std::string& path, const std::string& text, std::string& error);

/** @brief The file name without directory or extension, used to name a preset
 *  after the file it came from. */
std::string StemOf(const std::string& path);

} // namespace mtx
