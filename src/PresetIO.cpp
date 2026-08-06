#include "PresetIO.h"

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

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

    wchar_t file[MAX_PATH] = { 0 };
    if (saving && !suggestedName.empty())
    {
        const std::wstring stem = Widen(suggestedName);
        wcsncpy_s(file, stem.c_str(), _TRUNCATE);
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

std::string PresetFolder()
{
    // Same root as the probe log, so everything this plugin writes lives in one
    // predictable place.
    char path[MAX_PATH] = { 0 };
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) return "";

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
