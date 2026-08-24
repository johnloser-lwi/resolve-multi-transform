#pragma once

// A trailing-edge debounce for parameter edits.
//
// Why: dragging an Inspector slider fires an instance-changed action per tick,
// and republishing the output link on every tick makes the drag choppy. OFX has
// no release event for Inspector controls (the overlay gizmo has one; sliders
// do not), so "when the user lets go" has to be inferred: each edit arms a
// timer, each new edit resets it, and the callback fires once after a quiet
// gap -- which is the release, for any human drag.
//
// The mechanism is a Win32 thread timer (SetTimer with no window), which posts
// WM_TIMER to the arming thread's message queue and is dispatched by the
// host's own message loop. That pins two properties this plugin needs:
//  * the callback runs on the same thread that armed it -- the UI thread,
//    where instance-changed runs, so the publish code sees the exact world it
//    would in the action it was lifted out of;
//  * nothing fires while that thread is busy in a handler, so the callback
//    cannot interleave with the edit that scheduled it.
//
// The one caveat is honest and deliberate: the callback runs *between* OFX
// actions, not inside one. Parameter writes there are what Fusion's own
// scripting does routinely, but it is outside the letter of the OFX spec --
// which is why the feature this serves also has a manual update button that
// does the same work fully in-spec.

namespace mtx {

using DebounceFn = void (*)(void* ctx);

/** @brief Run @p cb(@p ctx) on this thread once @p ms milliseconds pass with no
 *  re-schedule for the same @p ctx. Re-scheduling resets the clock; the thread
 *  must pump messages (the host UI thread does). */
void ScheduleDebounced(void* ctx, unsigned ms, DebounceFn cb);

/** @brief Drop any pending callback for @p ctx (instance going away). */
void CancelDebounced(void* ctx);

} // namespace mtx
