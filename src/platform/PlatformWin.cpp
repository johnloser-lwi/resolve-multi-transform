// Windows implementation of Platform.h. The Win32/COM code that used to live
// in PresetIO.cpp, moved here unchanged in behaviour so the preset logic above
// it can be shared with macOS.

#include "platform/Platform.h"

#include "HostProbe.h"

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>
#include <cstring>

namespace mtx {
namespace platform {

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

std::string KnownFolder(int csidl)
{
    char path[MAX_PATH] = { 0 };
    if (FAILED(SHGetFolderPathA(nullptr, csidl, nullptr, 0, path))) return "";
    return path;
}

} // namespace

std::string AppDataFolder()
{
    const std::string base = KnownFolder(CSIDL_LOCAL_APPDATA);
    if (base.empty()) return "";
    const std::string dir = base + "\\MultiTransform";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::string DocumentsFolder()
{
    // CSIDL_PERSONAL follows a redirected or OneDrive-backed Documents folder,
    // which a hardcoded %USERPROFILE%\Documents would not.
    return KnownFolder(CSIDL_PERSONAL);
}

char PathSeparator() { return '\\'; }

bool MakeDirectory(const std::string& path)
{
    CreateDirectoryA(path.c_str(), nullptr);
    return DirectoryExists(path);
}

bool DirectoryExists(const std::string& path)
{
    const DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::vector<std::string> ListFiles(const std::string& folder, const std::string& extension)
{
    std::vector<std::string> out;
    if (folder.empty()) return out;

    WIN32_FIND_DATAA fd = {};
    const HANDLE h = FindFirstFileA((folder + "\\*" + extension).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;      // no folder, or nothing in it

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(folder + "\\" + fd.cFileName);
    }
    while (FindNextFileA(h, &fd));

    FindClose(h);
    return out;
}

/** @brief Shared open/save dialog, built on IFileDialog.
 *
 * Deliberately not GetOpenFileName/GetSaveFileName any more.
 *
 * The old code had to smuggle the starting folder in through `lpstrFile`,
 * because `lpstrInitialDir` has only been advisory since Vista -- Windows
 * prefers the calling executable's last-visited-folder MRU, and that executable
 * is Resolve, whose MRU is shared with every other dialog it opens. Putting a
 * bare directory in `lpstrFile` did beat the MRU, and then broke Open outright:
 * with OFN_FILEMUSTEXIST the dialog validates that string as a *file name*,
 * finds a path ending in a backslash, and returns FALSE before it ever appears.
 * The button did nothing at all.
 *
 * IFileDialog has SetFolder, so the folder can simply be stated. No filename
 * hack, nothing for the MRU to override, and failures come back as an HRESULT
 * that can be logged instead of vanishing.
 */
bool RunFileDialog(bool saving, const std::string& folder, const std::string& suggestedName,
                   std::string& outPath)
{
    // The host's UI thread is already COM-initialised, but this must not assume
    // it: initialise, tolerate a different existing mode, and only uninitialise
    // if this call is what initialised it.
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool weInitialised = SUCCEEDED(coInit);

    bool ok = false;
    IFileDialog* dialog = nullptr;

    HRESULT hr = CoCreateInstance(saving ? CLSID_FileSaveDialog : CLSID_FileOpenDialog,
                                  nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (SUCCEEDED(hr))
    {
        const COMDLG_FILTERSPEC kFilter[] = {
            { L"Multi Transform preset (*.json)", L"*.json" },
            { L"All files (*.*)",                 L"*.*"    }
        };
        dialog->SetFileTypes(2, kFilter);
        dialog->SetDefaultExtension(L"json");
        dialog->SetTitle(saving ? L"Save Multi Transform preset" : L"Load Multi Transform preset");

        DWORD flags = 0;
        if (SUCCEEDED(dialog->GetOptions(&flags)))
        {
            flags |= FOS_FORCEFILESYSTEM;
            flags |= saving ? FOS_OVERWRITEPROMPT : (FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
            dialog->SetOptions(flags);
        }

        if (!folder.empty())
        {
            IShellItem* start = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(Widen(folder).c_str(), nullptr,
                                                      IID_PPV_ARGS(&start))))
            {
                // SetFolder, not SetDefaultFolder: the latter is a suggestion the
                // last-visited folder overrides, which is the trap above.
                dialog->SetFolder(start);
                start->Release();
            }
        }

        if (saving && !suggestedName.empty())
            dialog->SetFileName(Widen(suggestedName).c_str());

        hr = dialog->Show(GetActiveWindow());
        if (SUCCEEDED(hr))
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

    // Cancelling is not a failure and must stay silent. Anything else is a
    // dialog that refused to appear, which is precisely the bug that went
    // unnoticed before -- so it gets recorded rather than swallowed.
    if (!ok && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
        ProbeLog(std::string("preset dialog failed: hr=") + buf);
    }
    return ok;
}

bool RunFolderDialog(const std::string& current, std::string& outPath)
{
    // IFileDialog in folder-pick mode rather than SHBrowseForFolder: the old
    // one is a cramped tree with no address bar and no way to paste a path.
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

bool SetClipboardText(const std::string& utf8, std::string& error)
{
    const std::wstring w = Widen(utf8);

    if (!OpenClipboard(GetActiveWindow()))
    {
        error = "could not open the clipboard (another app may be holding it)";
        return false;
    }

    bool ok = false;
    if (EmptyClipboard())
    {
        const size_t bytes = (w.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h)
        {
            if (void* dst = GlobalLock(h))
            {
                std::memcpy(dst, w.c_str(), bytes);
                GlobalUnlock(h);
                // On success the clipboard owns h; do not free it.
                if (SetClipboardData(CF_UNICODETEXT, h)) ok = true;
                else                                     GlobalFree(h);
            }
            else { GlobalFree(h); }
        }
    }
    CloseClipboard();

    if (!ok) error = "could not write to the clipboard";
    return ok;
}

std::string LogTimestamp()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d ",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return stamp;
}

} // namespace platform
} // namespace mtx
