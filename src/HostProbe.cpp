#include "HostProbe.h"

#include "platform/Platform.h"

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
    const std::string dir = platform::AppDataFolder();
    return dir.empty() ? "." : dir;
}

const char* YesNo(bool b) { return b ? "yes" : "NO"; }

} // namespace

std::string ProbeLogPath()
{
    static const std::string p = JoinPath(LogDir(), "probe.log");
    return p;
}

void ProbeLog(const std::string& line)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::ofstream f(ProbeLogPath(), std::ios::app);
    if (!f) return;

    f << platform::LogTimestamp() << line << "\n";
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

    // Every pixel depth the host is prepared to hand a plugin. This is the
    // host's list, not ours: a plugin can only choose from it, so if it says
    // "float" alone then no declaration on our side can make an image arrive
    // as half or 8-bit.
    std::string depths;
    for (OFX::BitDepthEnum d : h->_supportedPixelDepths)
        depths += std::string(OFX::mapBitDepthEnumToStr(d)) + " ";
    if (depths.empty()) depths = "(none reported)";

    std::ostringstream o;
    o << "\n"
      << "================ MultiTransform host probe ================\n"
      << "  supportedPixelDepths ....... " << depths << "\n"
      << "  defaultPixelDepth .......... "
      << OFX::mapBitDepthEnumToStr(h->getDefaultPixelDepth()) << "\n"
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
