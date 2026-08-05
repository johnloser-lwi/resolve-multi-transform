#pragma once

#include "Widgets.h"

#include "ofxsInteract.h"

namespace mtx {

/** @brief The plugin's viewer overlay.
 *
 * All custom UI lives here rather than in the Inspector, because Resolve's OFX
 * host supports neither parametric (curve) parameters nor custom parameter
 * interacts -- both measured in probe.log. The viewer overlay is the only
 * surface available, and it turns out to be the better one anyway: the timing
 * lanes and the easing curve belong over the picture, not beside it.
 *
 * Remember that Resolve does not show OFX overlays until the on-screen-control
 * dropdown under the Viewer is set to "Open FX Overlay".
 */
class MultiTransformInteract : public OFX::OverlayInteract
{
public:
    MultiTransformInteract(OfxInteractHandle handle, OFX::ImageEffect* effect);

    bool draw(const OFX::DrawArgs& args) override;
    bool penDown(const OFX::PenArgs& args) override;
    bool penMotion(const OFX::PenArgs& args) override;
    bool penUp(const OFX::PenArgs& args) override;

private:
    /** Snapshot the parameters and viewport for this event.
     *  Never throws: an exception escaping into the host mid-draw is worse than
     *  a frame without an overlay. */
    bool buildContext(OverlayContext& out, double time, const OfxPointD& pixelScale) const;
    bool buildContextUnsafe(OverlayContext& out, double time, const OfxPointD& pixelScale) const;

    /** Stage tabs and the From/To toggle, drawn along the top of the timeline. */
    void     drawToolbar(const OverlayContext& c);
    bool     toolbarHit(const OverlayContext& c, const OfxPointD& p);
    OfxRectD tabRect(const OverlayContext& c, int index) const;
    /** Buttons right-aligned above the timeline, counted from the right edge. */
    OfxRectD rightButtonRect(const OverlayContext& c, int indexFromRight, double widthPx) const;
    OfxRectD fromToRect(const OverlayContext& c, bool toButton) const;
    OfxRectD curveToggleRect(const OverlayContext& c) const;

    OFX::ImageEffect* _effect = nullptr;

    GizmoWidget    _gizmo;
    PathWidget     _path;
    TimelineWidget _timeline;
    CurveWidget    _curve;

    /// Widget that claimed the current drag, so motion/up go to the same place.
    Widget* _captured = nullptr;

    /// Whether a parameter edit block is currently open for that drag. The
    /// block must be closed exactly once, including when a drag is abandoned
    /// because something threw.
    bool _editBlockOpen = false;

    void openEditBlock(const char* name);
    void closeEditBlock();
};

/** @brief Descriptor that registers the overlay with the effect. */
class MultiTransformOverlayDescriptor
    : public OFX::DefaultEffectOverlayDescriptor<MultiTransformOverlayDescriptor,
                                                 MultiTransformInteract>
{
};

} // namespace mtx
