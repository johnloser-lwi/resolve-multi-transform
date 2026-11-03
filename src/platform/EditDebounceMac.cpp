// macOS implementation of EditDebounce.h: a one-shot CFRunLoopTimer on the
// main run loop. That gives the same two properties the Windows thread timer
// relies on (see EditDebounce.h): the callback runs on the UI thread -- where
// the host runs instance-changed -- and only from the run loop, so never in the
// middle of a handler. Re-scheduling invalidates the pending timer and starts a
// fresh one, which is the trailing-edge debounce.
//
// UNTESTED ON HARDWARE: written on Windows ahead of a Mac being available.

#include "EditDebounce.h"

#include <CoreFoundation/CoreFoundation.h>

#include <map>
#include <mutex>

namespace mtx {

namespace {

struct Pending
{
    void*             ctx;
    DebounceFn        cb;
    CFRunLoopTimerRef timer;
};

std::mutex                g_lock;
std::map<void*, Pending*> g_byCtx;   ///< the live timer, if any, per context

void Retire(Pending* p)
{
    CFRunLoopTimerInvalidate(p->timer);
    CFRelease(p->timer);
    delete p;
}

void TimerFired(CFRunLoopTimerRef /*timer*/, void* info)
{
    // Found by pointer value, never dereferenced first: if a cancel or a
    // re-schedule took it out of the map (and freed it) between the run loop
    // queuing this callout and it running, `info` points at freed memory and
    // the only safe thing to do with it is compare.
    Pending* p = nullptr;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        for (auto it = g_byCtx.begin(); it != g_byCtx.end(); ++it)
            if (it->second == info) { p = it->second; g_byCtx.erase(it); break; }
    }
    if (!p) return;   // superseded: whoever removed it owns retiring it
    // Outside the lock: the callback may take as long as it likes and may
    // re-schedule itself without deadlocking.
    const DebounceFn cb  = p->cb;
    void* const      ctx = p->ctx;
    Retire(p);
    if (cb) cb(ctx);
}

} // namespace

void ScheduleDebounced(void* ctx, unsigned ms, DebounceFn cb)
{
    Pending* p = new Pending{ ctx, cb, nullptr };

    CFRunLoopTimerContext context = {};
    context.info = p;
    p->timer = CFRunLoopTimerCreate(kCFAllocatorDefault,
                                    CFAbsoluteTimeGetCurrent() + ms / 1000.0,
                                    0,      // interval 0: one-shot
                                    0, 0, &TimerFired, &context);
    if (!p->timer) { delete p; cb(ctx); return; }   // no timer to be had: run now

    Pending* old = nullptr;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        auto it = g_byCtx.find(ctx);
        if (it != g_byCtx.end()) old = it->second;   // reset, not stack
        g_byCtx[ctx] = p;
    }
    if (old) Retire(old);

    CFRunLoopAddTimer(CFRunLoopGetMain(), p->timer, kCFRunLoopCommonModes);
}

void CancelDebounced(void* ctx)
{
    Pending* old = nullptr;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        auto it = g_byCtx.find(ctx);
        if (it == g_byCtx.end()) return;
        old = it->second;
        g_byCtx.erase(it);
    }
    Retire(old);
}

} // namespace mtx
