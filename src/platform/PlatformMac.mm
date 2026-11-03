// macOS implementation of Platform.h: Cocoa dialogs, NSPasteboard, and the
// standard user folders. Compiled as Objective-C++ with ARC (see CMakeLists).
//
// UNTESTED ON HARDWARE: written on Windows ahead of a Mac being available.
// The first things to check when it is: that the dialogs appear in front of
// Resolve, and that the folders below are created.

#include "platform/Platform.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <cstdio>
#include <ctime>

namespace mtx {
namespace platform {

namespace {

NSString* ToNS(const std::string& s)
{
    NSString* n = [NSString stringWithUTF8String:s.c_str()];
    return n ? n : @"";
}

std::string FromNS(NSString* s)
{
    if (!s) return "";
    const char* c = [s fileSystemRepresentation];
    return c ? std::string(c) : std::string();
}

std::string UserFolder(NSSearchPathDirectory which)
{
    NSArray<NSString*>* paths = NSSearchPathForDirectoriesInDomains(which, NSUserDomainMask, YES);
    return paths.count > 0 ? FromNS(paths[0]) : std::string();
}

/// AppKit panels and the pasteboard belong to the main thread. The host calls
/// instance-changed there in practice; this keeps the call safe if it ever
/// does not, at the cost of blocking the caller until the user answers.
void OnMain(void (^block)(void))
{
    if ([NSThread isMainThread]) block();
    else dispatch_sync(dispatch_get_main_queue(), block);
}

} // namespace

std::string AppDataFolder()
{
    const std::string base = UserFolder(NSApplicationSupportDirectory);
    if (base.empty()) return "";
    const std::string dir = base + "/MultiTransform";
    mkdir(dir.c_str(), 0755);
    return dir;
}

std::string DocumentsFolder()
{
    return UserFolder(NSDocumentDirectory);
}

char PathSeparator() { return '/'; }

bool MakeDirectory(const std::string& path)
{
    mkdir(path.c_str(), 0755);
    return DirectoryExists(path);
}

bool DirectoryExists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<std::string> ListFiles(const std::string& folder, const std::string& extension)
{
    std::vector<std::string> out;
    if (folder.empty()) return out;

    DIR* d = opendir(folder.c_str());
    if (!d) return out;

    while (struct dirent* e = readdir(d))
    {
        const std::string name = e->d_name;
        if (name.empty() || name[0] == '.') continue;
        if (name.size() < extension.size()) continue;
        if (name.compare(name.size() - extension.size(), extension.size(), extension) != 0)
            continue;

        const std::string full = folder + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) out.push_back(full);
    }
    closedir(d);
    return out;
}

bool RunFileDialog(bool saving, const std::string& folder, const std::string& suggestedName,
                   std::string& outPath)
{
    __block bool ok = false;
    __block std::string picked;
    NSString* start  = folder.empty() ? nil : ToNS(folder);
    NSString* name   = suggestedName.empty() ? nil : ToNS(suggestedName);

    OnMain(^{
        @autoreleasepool {
            NSSavePanel* panel = saving ? [NSSavePanel savePanel] : [NSOpenPanel openPanel];
            panel.title = saving ? @"Save Multi Transform preset" : @"Load Multi Transform preset";

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            // allowedFileTypes rather than allowedContentTypes: the latter
            // needs macOS 11's UniformTypeIdentifiers, and the extension is all
            // this needs to say.
            panel.allowedFileTypes = @[ @"json" ];
#pragma clang diagnostic pop

            if (start) panel.directoryURL = [NSURL fileURLWithPath:start isDirectory:YES];

            if (saving)
            {
                if (name) panel.nameFieldStringValue = [name stringByAppendingPathExtension:@"json"];
            }
            else
            {
                NSOpenPanel* open = (NSOpenPanel*)panel;
                open.canChooseFiles          = YES;
                open.canChooseDirectories    = NO;
                open.allowsMultipleSelection = NO;
            }

            if ([panel runModal] == NSModalResponseOK && panel.URL)
            {
                picked = FromNS(panel.URL.path);
                ok = !picked.empty();
            }
        }
    });

    if (ok) outPath = picked;
    return ok;
}

bool RunFolderDialog(const std::string& current, std::string& outPath)
{
    __block bool ok = false;
    __block std::string picked;
    NSString* start = current.empty() ? nil : ToNS(current);

    OnMain(^{
        @autoreleasepool {
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            panel.title                   = @"Choose the Multi Transform preset folder";
            panel.canChooseFiles          = NO;
            panel.canChooseDirectories    = YES;
            panel.canCreateDirectories    = YES;
            panel.allowsMultipleSelection = NO;
            if (start) panel.directoryURL = [NSURL fileURLWithPath:start isDirectory:YES];

            if ([panel runModal] == NSModalResponseOK && panel.URL)
            {
                picked = FromNS(panel.URL.path);
                ok = !picked.empty();
            }
        }
    });

    if (ok) outPath = picked;
    return ok;
}

bool SetClipboardText(const std::string& utf8, std::string& error)
{
    __block bool ok = false;
    NSString* text = ToNS(utf8);

    OnMain(^{
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        ok = [pb setString:text forType:NSPasteboardTypeString];
    });

    if (!ok) error = "could not write to the clipboard";
    return ok;
}

std::string LogTimestamp()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm local;
    const time_t secs = tv.tv_sec;
    localtime_r(&secs, &local);

    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d ",
                  local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(tv.tv_usec / 1000));
    return stamp;
}

} // namespace platform
} // namespace mtx
