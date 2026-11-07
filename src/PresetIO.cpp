#include "PresetIO.h"

#include "platform/Platform.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

// Shared preset and settings logic. Everything the OS has to do -- folders,
// listings, dialogs -- goes through platform/Platform.h,
// so this file behaves the same on Windows and macOS.

namespace mtx {

std::string CurveFolder()
{
    const std::string base = PresetFolder();
    if (base.empty()) return "";

    const std::string curves = JoinPath(base, "Curves");
    platform::MakeDirectory(curves);
    return curves;
}

std::vector<std::string> ListJsonFiles(const std::string& folder)
{
    std::vector<std::string> out = platform::ListFiles(folder, ".json");

    // The filesystem's order is whatever it hands back, which is not stable.
    // The library is a visual grid, so a curve moving between sessions for no
    // reason would be worse than it sounds.
    std::sort(out.begin(), out.end());
    return out;
}

std::string EffectClipboardPath()
{
    const std::string base = platform::AppDataFolder();
    return base.empty() ? "" : JoinPath(base, "clipboard.json");
}

std::string SettingsFilePath()
{
    const std::string base = platform::AppDataFolder();
    return base.empty() ? "" : JoinPath(base, "settings.json");
}

Settings LoadSettings()
{
    // Read on demand rather than cached, so a change made in one instance is
    // picked up by the others without a restart. That is affordable because the
    // hot caller -- PresetFolder, which the effect constructor uses -- caches
    // the resolved folder itself; everything left here happens when a dialog
    // opens, where a few dozen bytes off disk is beneath notice.
    Settings s = Settings::Default();

    const std::string path = SettingsFilePath();
    if (path.empty()) return s;

    std::string text, error;
    if (!ReadTextFile(path, text, error)) return s;   // absent on first run

    Settings parsed;
    if (SettingsFromJson(text, parsed, error)) s = parsed;
    return s;                                          // corrupt file -> defaults
}

bool SaveSettings(const Settings& s, std::string& error)
{
    const std::string path = SettingsFilePath();
    if (path.empty()) { error = "could not locate the application data folder"; return false; }
    return WriteTextFile(path, SettingsToJson(s), error);
}

bool ChooseFolder(const std::string& current, std::string& outPath)
{
    return platform::RunFolderDialog(current, outPath);
}

namespace {

/// Resolved preset folder, cached for the life of the process.
///
/// It was recomputed on every call, and one of the callers is the effect
/// constructor -- which Resolve runs every time a clip is *selected*. That put
/// a settings-file open, a JSON parse, a directory stat and two directory
/// creations on the path of an ordinary mouse click, to answer a question whose
/// answer changes only when the user changes it. InvalidatePresetFolderCache()
/// is called from the two places that can.
std::mutex      g_folderMutex;
std::string     g_folderCache;
bool            g_folderCached = false;

} // namespace

void InvalidatePresetFolderCache()
{
    std::lock_guard<std::mutex> lock(g_folderMutex);
    g_folderCached = false;
    g_folderCache.clear();
}

std::string PresetFolder()
{
    {
        std::lock_guard<std::mutex> lock(g_folderMutex);
        if (g_folderCached) return g_folderCache;
    }

    const Settings s = LoadSettings();

    std::string resolved;

    // A configured folder is honoured only while it is actually there. Sending
    // the dialog to a deleted folder or an unplugged drive would look exactly
    // like the bug this replaced.
    if (!s.presetFolder.empty() && platform::DirectoryExists(s.presetFolder))
        resolved = s.presetFolder;

    if (resolved.empty()) resolved = DefaultPresetFolder();

    {
        std::lock_guard<std::mutex> lock(g_folderMutex);
        g_folderCache  = resolved;
        g_folderCached = true;
    }
    return resolved;
}

std::string DefaultPresetFolder()
{
    // Documents, not the private data folder. Presets are the user's own files
    // -- they get named, kept, copied to another machine and shared with other
    // people -- and the data folder is hidden and nobody browses to it. The
    // probe log stays there: that one really is internal.
    std::string docs = platform::DocumentsFolder();

    // Somewhere to go rather than nowhere. An empty return would leave the
    // dialog with no folder at all, which is the problem this is fixing.
    const std::string base = docs.empty() ? platform::AppDataFolder()
                                          : JoinPath(docs, "MultiTransform");
    if (base.empty()) return "";
    platform::MakeDirectory(base);

    const std::string presets = JoinPath(base, "Presets");
    platform::MakeDirectory(presets);
    return presets;
}

bool ChoosePresetToOpen(std::string& outPath)
{
    return platform::RunFileDialog(false, PresetFolder(), "", outPath);
}

bool ChoosePresetToSave(const std::string& suggestedName, std::string& outPath)
{
    return platform::RunFileDialog(true, PresetFolder(), suggestedName, outPath);
}

bool ReadTextFile(const std::string& path, std::string& outText, std::string& error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "could not open " + path; return false; }

    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) { error = "could not read " + path; return false; }

    outText = ss.str();
    return true;
}

bool WriteTextFile(const std::string& path, const std::string& text, std::string& error)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { error = "could not create " + path; return false; }

    f << text;
    if (!f) { error = "could not write " + path; return false; }
    return true;
}

std::string StemOf(const std::string& path)
{
    size_t start = path.find_last_of("\\/");
    start = (start == std::string::npos) ? 0 : start + 1;

    const size_t dot = path.find_last_of('.');
    const size_t end = (dot == std::string::npos || dot < start) ? path.size() : dot;

    return path.substr(start, end - start);
}

} // namespace mtx
