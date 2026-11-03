#include "EditDebounce.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <map>
#include <mutex>

namespace mtx {

namespace {

struct Pending
{
    void*      ctx;
    DebounceFn cb;
};

// Both maps under one lock: timer id -> what to run, and ctx -> its live timer
// so a re-schedule can reset rather than stack. Thread timers are id-keyed
// globally per thread, so a plain KillTimer(nullptr, id) retires them.
std::mutex                      g_lock;
std::map<UINT_PTR, Pending>     g_byTimer;
std::map<void*, UINT_PTR>       g_byCtx;

void CALLBACK TimerProc(HWND, UINT, UINT_PTR id, DWORD)
{
    Pending run = { nullptr, nullptr };
    {
        std::lock_guard<std::mutex> hold(g_lock);
        auto it = g_byTimer.find(id);
        if (it == g_byTimer.end()) return;   // cancelled after the post
        run = it->second;
        g_byTimer.erase(it);
        g_byCtx.erase(run.ctx);
    }
    ::KillTimer(nullptr, id);
    // Outside the lock: the callback may take as long as it likes and may
    // re-schedule itself without deadlocking.
    if (run.cb) run.cb(run.ctx);
}

} // namespace

void ScheduleDebounced(void* ctx, unsigned ms, DebounceFn cb)
{
    std::lock_guard<std::mutex> hold(g_lock);

    // Reset, not stack: a drag re-arms per tick and must end as one callback.
    auto it = g_byCtx.find(ctx);
    if (it != g_byCtx.end())
    {
        ::KillTimer(nullptr, it->second);
        g_byTimer.erase(it->second);
        g_byCtx.erase(it);
    }

    // hwnd null: a thread timer, fired by this thread's own message loop --
    // the property the header relies on.
    const UINT_PTR id = ::SetTimer(nullptr, 0, ms, &TimerProc);
    if (id == 0) { cb(ctx); return; }        // no timer to be had: run now

    g_byTimer[id] = { ctx, cb };
    g_byCtx[ctx]  = id;
}

void CancelDebounced(void* ctx)
{
    std::lock_guard<std::mutex> hold(g_lock);
    auto it = g_byCtx.find(ctx);
    if (it == g_byCtx.end()) return;
    ::KillTimer(nullptr, it->second);
    g_byTimer.erase(it->second);
    g_byCtx.erase(it);
}

} // namespace mtx
