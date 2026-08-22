#pragma once

// The frame's bounds, published by the render path for the viewer overlay.
//
// The overlay used to ask the host for the source clip's region of definition
// on every draw. On the Fusion page that call goes through Fusion's graph
// evaluation, and at a frame where the node's input cannot be resolved --
// "cannot get Parameter for Source" in Fusion's own log, routine while
// scrubbing -- the host's implementation sometimes returns an error and
// sometimes faults with a null write inside ntdll. A crash dump put the fault
// exactly there: overlay draw -> Clip::getRegionOfDefinition -> host frames ->
// ntdll. A status-checked call would still make the call.
//
// So the overlay does not ask. render() sees the destination's bounds on every
// frame and records them here; the overlay reads the last recorded rectangle.
// Before the first render of a session there is nothing to show and the
// overlay draws nothing, which is the right answer for a frame that has not
// been rendered either.

#include "ofxCore.h"

namespace OFX { class ImageEffect; }

namespace mtx {

class FrameBoundsProvider
{
public:
    virtual ~FrameBoundsProvider() = default;

    /** @brief The canonical bounds of the most recently rendered frame.
     *  @return false until a frame has been rendered in this session. */
    virtual bool lastFrameBounds(OfxRectD& out) const = 0;
};

/** @brief The provider behind an effect, or null if the effect is not ours.
 *  A dynamic_cast, because the interact is handed an OFX::ImageEffect by the
 *  host with no guarantee whose it is. */
FrameBoundsProvider* FrameBoundsOf(OFX::ImageEffect* effect);

} // namespace mtx
