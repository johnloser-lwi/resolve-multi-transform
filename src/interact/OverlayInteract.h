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

    // Shift-to-constrain. OFX gives pen actions no modifier state, so the only
    // way to know Shift is down is to watch the key events and remember.
    bool keyDown(const OFX::KeyArgs& args) override;
    bool keyUp(const OFX::KeyArgs& args) override;
    void gainFocus(const OFX::FocusArgs& args) override;
    void loseFocus(const OFX::FocusArgs& args) override;

private:
    static bool isShift(int keySymbol);
    static bool isCtrl(int keySymbol);
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
    /** Button width that keeps the right-hand run clear of the stage tabs. */
    double   toggleWidth(const OverlayContext& c) const;
    OfxRectD rightButtonRect(const OverlayContext& c, int indexFromRight, double widthPx,
                             int row = 0) const;
    OfxRectD fromToRect(const OverlayContext& c, bool toButton) const;
    OfxRectD baseTargetRect(const OverlayContext& c) const;
    OfxRectD dragPreviewRect(const OverlayContext& c) const;
    OfxRectD curveToggleRect(const OverlayContext& c) const;
    OfxRectD libraryToggleRect(const OverlayContext& c) const;
    OfxRectD loadPresetRect(const OverlayContext& c) const;
    OfxRectD timelineToggleRect(const OverlayContext& c) const;
    OfxRectD pathToggleRect(const OverlayContext& c) const;
    OfxRectD opacityToggleRect(const OverlayContext& c) const;
    OfxRectD removeStageRect(const OverlayContext& c) const;
    OfxRectD enableStageRect(const OverlayContext& c) const;
    OfxRectD quickToggleRect(const OverlayContext& c) const;

    /** Flip a hidden trigger parameter, which changedParam reads as a press. */
    void     fireTrigger(const char* paramName);

    OFX::ImageEffect* _effect = nullptr;

    GizmoWidget    _gizmo;
    PathWidget     _path;
    TimelineWidget _timeline;
    CurveWidget    _curve;
    LibraryWidget  _library;
    QuickWidget    _quick;
    OpacityWidget  _opacity;

    /// Widget that claimed the current drag, so motion/up go to the same place.
    Widget* _captured = nullptr;

    /// Shift state, maintained across events. Cleared on focus loss: a host that
    /// delivers the press but takes focus away before the release would
    /// otherwise leave it stuck on for good.
    bool _shiftHeld = false;
    bool _ctrlHeld  = false;

    /// Whether a parameter edit block is currently open for that drag. The
    /// block must be closed exactly once, including when a drag is abandoned
    /// because something threw.
    bool _editBlockOpen = false;

    void openEditBlock(const char* name);
    void closeEditBlock();

    /** Recover from a drag whose mouse-up never arrived: drop the capture and
     *  force every open edit block shut. Safe to call when nothing is open. */
    void abandonDrag(const char* why);

    /** Ask the renderer to composite a faded copy at the dragged pose. */
    void setPreviewGhost(bool on);
};

/** @brief Descriptor that registers the overlay with the effect. */
class MultiTransformOverlayDescriptor
    : public OFX::DefaultEffectOverlayDescriptor<MultiTransformOverlayDescriptor,
                                                 MultiTransformInteract>
{
};

} // namespace mtx
