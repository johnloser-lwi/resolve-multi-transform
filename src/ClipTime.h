#pragma once

#include "ClipRange.h"
#include "ofxsImageEffect.h"

namespace mtx {

/** @brief The clip's extent, in timeline frames.
 *
 * Stage timing is authored clip-relative -- frame 0 is the clip's first frame --
 * so that moving or trimming the clip carries the animation with it instead of
 * leaving it stranded at fixed timeline positions. Converting between the two
 * needs exactly one number: where the clip starts.
 *
 * Finding it took measuring, because most of the obvious routes are dead ends
 * in Resolve:
 *
 *   src/dst getFrameRange         [0, 1798200]  a sentinel, exactly 1000 minutes
 *   src/dst getUnmappedFrameRange [0, 0]        not populated
 *   timeLineGetTime               0             not populated
 *   timeLineGetBounds             [107961, 108116]   <- the clip, 155 frames
 *
 * The write direction is a dead end as well, measured the same way:
 *
 *   timeLineGotoTime              silently ignored
 *
 * The suite is present -- timeLineGetBounds above comes from it and works -- but
 * Resolve does not act on gotoTime. An overlay pair of "jump the playhead to
 * this stage's start / end" buttons was built on it and removed again when they
 * turned out to do nothing. OFX 1.4 offers no other route to the host playhead,
 * so a plugin can read where the playhead is (from the time passed to render and
 * changedParam) but cannot move it. That is why timing is captured with "Set
 * Start / End to Playhead" -- park the playhead and click -- rather than the
 * more obvious other way round.
 *
 * @return false if the host reports nothing usable, in which case the caller
 *         should fall back to treating render times as already absolute.
 */
inline bool GetClipRange(OFX::ImageEffect* effect, double& outStart, double& outLength)
{
    if (!effect) return false;

    double t1 = 0.0, t2 = 0.0;
    try
    {
        effect->timeLineGetBounds(t1, t2);
    }
    catch (...)
    {
        return false;
    }

    return ValidateClipRange(t1, t2, outStart, outLength);
}

/** @brief Convert a render time to clip-relative frames, where 0 is the clip's
 *  first frame. Falls through unchanged when the clip extent is unknown. */
inline double ToClipTime(OFX::ImageEffect* effect, double absoluteTime)
{
    double start = 0.0, length = 0.0;
    return GetClipRange(effect, start, length) ? absoluteTime - start : absoluteTime;
}

} // namespace mtx
