#pragma once

// The operating-system layer: everything the plugin needs from the OS that is
// not image processing -- folders, file listing and the native file dialogs.
// One implementation per platform:
//
//   PlatformWin.cpp   Win32 / COM (IFileDialog, SHGetFolderPath)
//   PlatformMac.mm    Cocoa (NSOpenPanel, NSSavePanel)
//
// Kept deliberately small and string-based, so the preset and settings logic
// above it (PresetIO.cpp) is shared code with one behaviour on both systems.
// Paths are UTF-8 throughout.

#include <string>
#include <vector>

namespace mtx {
namespace platform {

/// The plugin's private data folder, created if missing; "" if the OS will not
/// say. Holds settings.json, the effect clipboard and the probe log.
///   Windows: %LOCALAPPDATA%\MultiTransform
///   macOS:   ~/Library/Application Support/MultiTransform
std::string AppDataFolder();

/// The user's Documents folder (following any redirection), or "".
std::string DocumentsFolder();

/// '\\' on Windows, '/' on macOS.
char PathSeparator();

/// Create one directory level. True if it exists afterwards.
bool MakeDirectory(const std::string& path);

bool DirectoryExists(const std::string& path);

/// Full paths of the regular files in @p folder whose names end in
/// @p extension (e.g. ".json"), in no particular order.
std::vector<std::string> ListFiles(const std::string& folder, const std::string& extension);

/// The native open or save dialog for a .json preset, starting in @p folder.
/// False when cancelled or when the dialog could not be shown.
bool RunFileDialog(bool saving, const std::string& folder, const std::string& suggestedName,
                   std::string& outPath);

/// The native folder picker, starting in @p current.
bool RunFolderDialog(const std::string& current, std::string& outPath);

/// Local wall-clock time as "HH:MM:SS.mmm ", for log lines.
std::string LogTimestamp();

} // namespace platform

/// @p dir and @p name joined with the platform's separator.
inline std::string JoinPath(const std::string& dir, const std::string& name)
{
    if (dir.empty()) return name;
    const char sep = platform::PathSeparator();
    const char last = dir[dir.size() - 1];
    if (last == '/' || last == sep) return dir + name;
    return dir + sep + name;
}

} // namespace mtx
