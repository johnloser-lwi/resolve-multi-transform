#include "HostProbe.h"

#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#include "ofxsImageEffect.h"
#include "ofxsSupportPrivate.h"

namespace mtx {

namespace {

std::mutex g_mutex;
std::set<std::string> g_seen;
bool g_hostDumped = false;

std::string LogDir()
{
    char path[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path)))
    {
        std::string dir = std::string(path) + "\\MultiTransform";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir;
    }
    return ".";
}

const char* YesNo(bool b) { return b ? "yes" : "NO"; }

} // namespace

std::string ProbeLogPath()
{
    static const std::string p = LogDir() + "\\probe.log";
    return p;
}

void ProbeLog(const std::string& line)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::ofstream f(ProbeLogPath(), std::ios::app);
    if (!f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[32];
    sprintf_s(stamp, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    f << stamp << line << "\n";
}

void ProbeOnce(const std::string& key, const std::string& line)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_seen.count(key)) return;
        g_seen.insert(key);
    }
    ProbeLog(line);
}

namespace {

/// Wall clock in milliseconds. steady_clock, so it cannot go backwards when the
/// system clock is adjusted mid-measurement.
double NowMs()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

/// Formatted to hundredths: below that is noise next to a host round trip, and
/// full precision would make the line unreadable.
std::string Ms(double v)
{
    char buf[32];
    sprintf_s(buf, "%.2f", v);
    return buf;
}

} // namespace

ProbeTimer::ProbeTimer(std::string label)
    : _label(std::move(label))
    , _startMs(NowMs())
    , _lastMs(_startMs)
{
}

void ProbeTimer::split(const char* name)
{
    const double now = NowMs();
    if (!_parts.empty()) _parts += "  ";
    _parts += std::string(name) + "=" + Ms(now - _lastMs);
    _lastMs = now;
}

void ProbeTimer::note(const std::string& text)
{
    if (!_parts.empty()) _parts += "  ";
    _parts += text;
}

ProbeTimer::~ProbeTimer()
{
    ProbeLog("timing " + _label + " total=" + Ms(NowMs() - _startMs) + "ms"
             + (_parts.empty() ? std::string() : "  [" + _parts + "]"));
}

void ProbeHostOnce()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_hostDumped) return;
        g_hostDumped = true;
    }

    const OFX::ImageEffectHostDescription* h = OFX::getImageEffectHostDescription();
    if (!h)
    {
        ProbeLog("=== HOST PROBE === getImageEffectHostDescription() returned null");
        return;
    }

    std::ostringstream o;
    o << "\n"
      << "================ MultiTransform host probe ================\n"
      << "  host name .................. " << h->hostName  << "\n"
      << "  host label ................. " << h->hostLabel << "\n"
      << "  host version ............... " << h->versionMajor << "." << h->versionMinor
                                           << "." << h->versionMicro
                                           << " (" << h->versionLabel << ")\n"
      << "  OFX API version ............ " << h->APIVersionMajor << "." << h->APIVersionMinor << "\n"
      << "  -- things this project depends on --\n"
      << "  supportsOverlays ........... " << YesNo(h->supportsOverlays) << "\n"
      << "  DrawSuite fetched .......... " << YesNo(OFX::Private::gDrawSuite != nullptr) << "\n"
      << "  supportsCustomInteract ..... " << YesNo(h->supportsCustomInteract) << "\n"
      << "  supportsParametricParameter  " << YesNo(h->supportsParametricParameter) << "\n"
      << "  supportsCudaRender ......... " << YesNo(h->supportsCudaRender) << "\n"
      << "  supportsCudaStream ......... " << YesNo(h->supportsCudaStream) << "\n"
      << "  supportsOpenCLRender ....... " << YesNo(h->supportsOpenCLRender) << "\n"
      << "  -- secondary --\n"
      << "  supportsMultiResolution .... " << YesNo(h->supportsMultiResolution) << "\n"
      << "  supportsTiles .............. " << YesNo(h->supportsTiles) << "\n"
      << "  temporalClipAccess ......... " << YesNo(h->temporalClipAccess) << "\n"
      << "  supportsBooleanAnimation ... " << YesNo(h->supportsBooleanAnimation) << "\n"
      << "  supportsChoiceAnimation .... " << YesNo(h->supportsChoiceAnimation) << "\n"
      << "  maxParameters .............. " << h->maxParameters << "\n"
      << "  maxPages ................... " << h->maxPages << "\n"
      << "  nativeOrigin ............... " << static_cast<int>(h->nativeOrigin) << "\n"
      << "==========================================================";

    ProbeLog(o.str());
}

} // namespace mtx
