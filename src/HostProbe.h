#pragma once

#include <string>

namespace mtx {

/** @brief Append a line to the probe log (%LOCALAPPDATA%\MultiTransform\probe.log).
 *
 * Deliberately cheap and fire-and-forget: opens, appends, closes. The log is a
 * development aid for establishing what the host actually supports, since the
 * published documentation for Resolve's OFX host is thin and partly wrong.
 */
void ProbeLog(const std::string& line);

/** @brief Dump the full host description exactly once per process.
 *
 * Safe to call repeatedly; only the first call writes.
 */
void ProbeHostOnce();

/** @brief Log something only the first time this call site sees it.
 *
 * Used for events that fire per-frame (render, overlay draw) where we want
 * evidence that they happen at all, not a flood.
 */
void ProbeOnce(const std::string& key, const std::string& line);

/** @brief Path of the probe log, for reporting to the user. */
std::string ProbeLogPath();

} // namespace mtx
