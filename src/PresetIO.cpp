#include "PresetIO.h"

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

namespace mtx {

namespace {

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

/** Shared dialog setup. The filter is a double-null-terminated pair list. */
bool RunFileDialog(bool saving, const std::string& suggestedName, std::string& outPath)
{
    const std::wstring folder = Widen(PresetFolder());

    // The folder is seeded into lpstrFile, not just lpstrInitialDir.
    //
    // Since Vista, lpstrInitialDir is only advisory: Windows prefers the calling
    // *executable's* last-visited-folder MRU, and that executable is Resolve --
    // an MRU shared with every other file dialog Resolve opens. So after using
    // any unrelated dialog in the host, this one would open there instead of in
    // the preset folder, which looked like the setting being ignored at random.
    // A path in lpstrFile takes priority over that MRU, so it is the one place
    // the folder can be stated and actually respected.
    wchar_t file[MAX_PATH] = { 0 };
    if (!folder.empty())
    {
        std::wstring seed = folder;
        if (seed.back() != L'\\') seed += L'\\';
        if (saving && !suggestedName.empty()) seed += Widen(suggestedName);
        wcsncpy_s(file, seed.c_str(), _TRUNCATE);
    }
    else if (saving && !suggestedName.empty())
    {
        wcsncpy_s(file, Widen(suggestedName).c_str(), _TRUNCATE);
    }

    static const wchar_t kFilter[] = L"Multi Transform preset (*.json)\0*.json\0All files (*.*)\0*.*\0\0";

    OPENFILENAMEW ofn = {};
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = GetActiveWindow();
    ofn.lpstrFilter     = kFilter;
    ofn.lpstrFile       = file;
    ofn.nMaxFile        = MAX_PATH;
    ofn.lpstrInitialDir = folder.empty() ? nullptr : folder.c_str();
    ofn.lpstrDefExt     = L"json";
    ofn.lpstrTitle      = saving ? L"Save Multi Transform preset"
                                 : L"Load Multi Transform preset";
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR
              | (saving ? OFN_OVERWRITEPROMPT : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));

    const BOOL ok = saving ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return false;   // cancelled, or the dialog failed

    outPath = Narrow(file);
    return !outPath.empty();
}

} // namespace

std::string CurveFolder()
{
    const std::string base = PresetFolder();
    if (base.empty()) return "";

    const std::string curves = base + "\\Curves";
    CreateDirectoryA(curves.c_str(), nullptr);
    return curves;
}

std::vector<std::string> ListJsonFiles(const std::string& folder)
{
    std::vector<std::string> out;
    if (folder.empty()) return out;

    WIN32_FIND_DATAA fd = {};
    const HANDLE h = FindFirstFileA((folder + "\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;      // no folder, or nothing in it

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(folder + "\\" + fd.cFileName);
    }
    while (FindNextFileA(h, &fd));

    FindClose(h);

    // FindFirstFile's order is whatever the filesystem hands back, which is not
    // stable. The library is a visual grid, so a curve moving between sessions
    // for no reason would be worse than it sounds.
    std::sort(out.begin(), out.end());
    return out;
}

std::string SettingsFilePath()
{
    char path[MAX_PATH] = { 0 };
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) return "";

    const std::string base = std::string(path) + "\\MultiTransform";
    CreateDirectoryA(base.c_str(), nullptr);
    return base + "\\settings.json";
}

Settings LoadSettings()
{
    // Read on demand rather than cached. The file is a few dozen bytes, this is
    // only touched when a dialog opens, and reading each time means a change
    // made in one instance is picked up by the others without a restart.
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
    // IFileDialog in folder-pick mode rather than SHBrowseForFolder: the old
    // one is a cramped tree with no address bar and no way to paste a path,
    // which is the same friction this whole change exists to remove.
    //
    // The host's UI thread is already COM-initialised, but this must not assume
    // it: initialise, tolerate a different existing mode, and only uninitialise
    // if this call is what initialised it.
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool weInitialised = SUCCEEDED(coInit);

    bool ok = false;
    IFileDialog* dialog = nullptr;

    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog))))
    {
        DWORD flags = 0;
        if (SUCCEEDED(dialog->GetOptions(&flags)))
            dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

        dialog->SetTitle(L"Choose the Multi Transform preset folder");

        if (!current.empty())
        {
            IShellItem* start = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(Widen(current).c_str(), nullptr,
                                                      IID_PPV_ARGS(&start))))
            {
                // SetFolder, not SetDefaultFolder: the latter is only a
                // suggestion that the last-visited folder overrides, which is
                // the exact trap the file dialogs fell into.
                dialog->SetFolder(start);
                start->Release();
            }
        }

        if (SUCCEEDED(dialog->Show(GetActiveWindow())))
        {
            IShellItem* picked = nullptr;
            if (SUCCEEDED(dialog->GetResult(&picked)))
            {
                PWSTR wide = nullptr;
                if (SUCCEEDED(picked->GetDisplayName(SIGDN_FILESYSPATH, &wide)) && wide)
                {
                    outPath = Narrow(wide);
                    ok = !outPath.empty();
                    CoTaskMemFree(wide);
                }
                picked->Release();
            }
        }
        dialog->Release();
    }

    if (weInitialised) CoUninitialize();
    return ok;
}

std::string PresetFolder()
{
    const Settings s = LoadSettings();

    // A configured folder is honoured only while it is actually there. Sending
    // the dialog to a deleted folder or an unplugged drive would look exactly
    // like the bug this replaced.
    if (!s.presetFolder.empty())
    {
        const DWORD attr = GetFileAttributesA(s.presetFolder.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            return s.presetFolder;
    }

    return DefaultPresetFolder();
}

std::string DefaultPresetFolder()
{
    // Documents, not LocalAppData. Presets are the user's own files -- they get
    // named, kept, copied to another machine and shared with other people -- and
    // LocalAppData is a hidden folder nobody browses to. The probe log stays
    // where it was: that one really is internal.
    char path[MAX_PATH] = { 0 };

    // CSIDL_PERSONAL follows a redirected or OneDrive-backed Documents folder,
    // which a hardcoded %USERPROFILE%\Documents would not.
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, path)))
    {
        // Somewhere to go rather than nowhere. An empty return would leave the
        // dialog with no folder at all, which is the problem this is fixing.
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) return "";
    }

    const std::string base = std::string(path) + "\\MultiTransform";
    CreateDirectoryA(base.c_str(), nullptr);

    const std::string presets = base + "\\Presets";
    CreateDirectoryA(presets.c_str(), nullptr);
    return presets;
}

bool ChoosePresetToOpen(std::string& outPath)
{
    return RunFileDialog(false, "", outPath);
}

bool ChoosePresetToSave(const std::string& suggestedName, std::string& outPath)
{
    return RunFileDialog(true, suggestedName, outPath);
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
