#pragma once

// Deliberately free of any OFX dependency, so the rules below can be unit
// tested without a host. They are the part most likely to be wrong: a bad
// answer here silently mis-times every animation rather than failing loudly.

namespace mtx {

/** @brief Decide whether a reported [t1, t2] is a real clip extent.
 *
 * Measured behaviour of the candidate sources in Resolve:
 *
 *   src/dst getFrameRange         [0, 1798200]      a sentinel, exactly 1000 minutes
 *   src/dst getUnmappedFrameRange [0, 0]            not populated
 *   timeLineGetTime               0                 not populated
 *   timeLineGetBounds             [107961, 108116]  the clip: 155 frames
 *
 * Only the last is usable, so this exists to tell a genuine answer from a host
 * that is really saying "I do not know".
 */
inline bool ValidateClipRange(double t1, double t2, double& outStart, double& outLength)
{
    // NaN fails every comparison, so reject it explicitly rather than relying on
    // the range checks below to catch it.
    if (!(t1 == t1) || !(t2 == t2)) return false;

    const double span = t2 - t1;

    // A real clip is at least one frame long.
    if (!(span > 0.0)) return false;

    // ...and nowhere near a thousand minutes. Anything of that order is the
    // unbounded sentinel, not a clip.
    if (span > 1.0e6) return false;

    outStart  = t1;
    outLength = span;
    return true;
}

/** @brief Whether a stored frame value is a legacy timeline-absolute one.
 *
 * Stage timing used to be stored as absolute timeline frames and is now stored
 * clip-relative, so old projects need converting once. The two are told apart
 * by magnitude, which is unambiguous in practice:
 *
 *  - Resolve timelines start at 01:00:00:00, so a clip's start is around
 *    107892 frames and any absolute value sits near it.
 *  - A clip-relative value is bounded by the clip's length -- tens or hundreds
 *    of frames, occasionally a few thousand for a very long clip.
 *
 * Requiring the value to exceed *half* the clip start leaves an enormous margin
 * between the two populations: a relative value would have to be some 54000
 * frames (half an hour) into the clip to be mistaken for an absolute one.
 *
 * When the clip start is small -- a host whose timeline begins at zero, or a
 * clip sitting at the very start -- absolute and relative coincide anyway, so
 * there is nothing to convert and this correctly declines to.
 */
inline bool LooksTimelineAbsolute(double value, double clipStart)
{
    return clipStart > 1000.0 && value > clipStart * 0.5;
}

/** @brief Interpret a stored stage frame as clip-relative, converting a legacy
 *  timeline-absolute value on the fly.
 *
 * Applied on every read rather than relying on a one-time conversion having
 * already happened. That matters because the conversion needs the clip's start,
 * which is not reliably available at every point in an effect's life -- asked
 * too early, the host returns a placeholder. A one-shot flag combined with a
 * bad reading is the worst of both worlds: it converts nothing and then records
 * the job as done.
 *
 * Because the test is purely one of magnitude, this is idempotent: an already
 * relative value is bounded by the clip's length and can never look absolute,
 * so re-applying it is harmless. Correctness therefore does not depend on when
 * or whether the stored values are rewritten; that becomes a tidy-up of the
 * displayed numbers, not a prerequisite.
 */
inline double NormaliseStageFrame(double stored, double clipStart)
{
    return LooksTimelineAbsolute(stored, clipStart) ? stored - clipStart : stored;
}

} // namespace mtx
