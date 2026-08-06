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
