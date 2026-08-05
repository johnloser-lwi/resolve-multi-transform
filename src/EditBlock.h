#pragma once

#include "ofxsImageEffect.h"

namespace mtx {

/** @brief RAII wrapper around OFX's parameter edit block.
 *
 * The OFX spec is explicit that when a plugin calls paramSetValue itself --
 * "either from custom GUI interaction or some analysis of imagery" -- the
 * writes must be bracketed by paramEditBegin/paramEditEnd, from inside an
 * interact action or kOfxActionInstanceChanged.
 *
 * Every parameter this plugin writes is exactly that case: the viewer overlay's
 * gizmo, timeline and curve handles, the Set Start/End buttons, the easing
 * presets, and the derived Duration read-out. Without the brackets a host has
 * been told nothing about the edit. Resolve tolerates it. Fusion does not: it
 * kept serving cached frames after any overlay drag or button press, while
 * edits made through its own Inspector -- which the host performs itself, and
 * therefore already knows about -- updated correctly. That asymmetry is the
 * signature of this bug.
 *
 * Bracketing also groups a drag into a single undo step instead of one per
 * mouse-move event, which is the behaviour the block was designed for.
 */
class EditBlock
{
public:
    EditBlock(OFX::ImageEffect* effect, const char* name)
        : _effect(effect)
    {
        if (_effect) _effect->beginEditBlock(name);
    }

    ~EditBlock()
    {
        if (_effect) _effect->endEditBlock();
    }

    EditBlock(const EditBlock&)            = delete;
    EditBlock& operator=(const EditBlock&) = delete;

private:
    OFX::ImageEffect* _effect;
};

} // namespace mtx
