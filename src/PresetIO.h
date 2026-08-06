#pragma once

#include <string>

namespace mtx {

/** @brief Default preset folder, created on demand.
 *  `%LOCALAPPDATA%\MultiTransform\Presets`, alongside the probe log. */
std::string PresetFolder();

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
