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

/** @brief Time a lifecycle step and write one summary line to the probe log.
 *
 * Exists because the cost of selecting a clip is spread across a dozen small
 * things -- parameter fetches, secrecy updates, edit blocks, a settings file --
 * and guessing which of them dominates is exactly the mistake this project has
 * made before. Every split is named, so the log says where the time went rather
 * than only that there was some.
 *
 * One file write per timer, not per split: the measurement must not be a
 * meaningful part of what is being measured.
 */
class ProbeTimer
{
public:
    explicit ProbeTimer(std::string label);
    ~ProbeTimer();

    /** @brief Record the time since the previous split (or since construction). */
    void split(const char* name);

    /** @brief Add a bare fact to the line, such as a count. */
    void note(const std::string& text);

private:
    std::string _label;
    std::string _parts;
    double      _startMs;
    double      _lastMs;
};

} // namespace mtx
