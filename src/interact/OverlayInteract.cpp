#include "OverlayInteract.h"

#include "AnimEngine.h"
#include "ClipTime.h"
#include "EditBlock.h"
#include "HostProbe.h"
#include "ParamNames.h"

#include <cmath>

namespace mtx {

namespace {
constexpr double kTabWidthPx  = 26.0;
constexpr double kTabHeightPx = 18.0;
constexpr double kTabGapPx    = 4.0;
constexpr double kToggleWPx   = 54.0;   // preferred width
constexpr double kToggleMinPx = 42.0;   // floor: still fits CURVE, the longest label
constexpr int    kToggleCount = 6;      // widest row: TIME PATH OPAC CURVE LIB LOAD
} // namespace

MultiTransformInteract::MultiTransformInteract(OfxInteractHandle handle,
                                               OFX::ImageEffect* effect)
    : OFX::OverlayInteract(handle)
    , _effect(effect)
{
    // Slaving to the parameters makes the host redraw the overlay whenever a
    // value changes, so edits made in the Inspector are reflected immediately
    // instead of only after the next viewer refresh.
    if (!_effect) return;

    // Slaving is an optimisation, not a requirement: if any of it fails the
    // overlay still works, it just redraws less eagerly. Letting an exception
    // escape a constructor here would cost the overlay entirely.
    try
    {
        addParamToSlaveTo(_effect->fetchChoiceParam(kParamStageCount));
        addParamToSlaveTo(_effect->fetchChoiceParam(kParamActiveStage));
        addParamToSlaveTo(_effect->fetchChoiceParam(kParamEditTarget));
        addParamToSlaveTo(_effect->fetchBooleanParam(kParamShowCurve));

        for (int i = 0; i < kMaxStages; ++i)
        {
            addParamToSlaveTo(_effect->fetchBooleanParam (StageParam(kParamEnabled,      i)));
            addParamToSlaveTo(_effect->fetchDoubleParam  (StageParam(kParamStartFrame,   i)));
            addParamToSlaveTo(_effect->fetchDoubleParam  (StageParam(kParamEndFrame,     i)));
            addParamToSlaveTo(_effect->fetchDoubleParam  (StageParam(kParamScaleFrom,    i)));
            addParamToSlaveTo(_effect->fetchDoubleParam  (StageParam(kParamScaleTo,      i)));
            addParamToSlaveTo(_effect->fetchDouble2DParam(StageParam(kParamPosFrom,      i)));
            addParamToSlaveTo(_effect->fetchDouble2DParam(StageParam(kParamPosTo,        i)));
            addParamToSlaveTo(_effect->fetchDoubleParam  (StageParam(kParamRotFrom,      i)));
            addParamToSlaveTo(_effect->fetchDoubleParam  (StageParam(kParamRotTo,        i)));
            addParamToSlaveTo(_effect->fetchDoubleParam  (StageParam(kParamOpacityFrom,  i)));
            addParamToSlaveTo(_effect->fetchDoubleParam  (StageParam(kParamOpacityTo,    i)));
            addParamToSlaveTo(_effect->fetchDouble2DParam(StageParam(kParamAnchor,       i)));
            addParamToSlaveTo(_effect->fetchDouble2DParam(StageParam(kParamPathC1,       i)));
            addParamToSlaveTo(_effect->fetchDouble2DParam(StageParam(kParamPathC2,       i)));
            addParamToSlaveTo(_effect->fetchDoubleParam  (StageParam(kParamEaseIn,       i)));
            addParamToSlaveTo(_effect->fetchDoubleParam  (StageParam(kParamEaseOut,      i)));
            addParamToSlaveTo(_effect->fetchDoubleParam  (StageParam(kParamAnticipation, i)));
            addParamToSlaveTo(_effect->fetchDoubleParam  (StageParam(kParamOvershoot,    i)));
            addParamToSlaveTo(_effect->fetchChoiceParam  (StageParam(kParamBounceType,   i)));
            addParamToSlaveTo(_effect->fetchDoubleParam  (StageParam(kParamBounceAmount, i)));
            addParamToSlaveTo(_effect->fetchDoubleParam  (StageParam(kParamBounceCount,  i)));
            addParamToSlaveTo(_effect->fetchDoubleParam  (StageParam(kParamBounceDamping,i)));
            addParamToSlaveTo(_effect->fetchDoubleParam  (StageParam(kParamBounceStart,  i)));
        }
    }
    catch (...)
    {
        mtx::ProbeLog("overlay: addParamToSlaveTo failed; overlay will still draw");
    }
}

void MultiTransformInteract::openEditBlock(const char* name)
{
    if (!_effect || _editBlockOpen) return;

    // Through the shared counter, so the widget's own blocks and the ones
    // changedParam opens underneath them collapse into this single one as far
    // as the host is concerned. See EditBlock.h.
    BeginEdit(_effect, name);
    _editBlockOpen = true;
}

void MultiTransformInteract::closeEditBlock()
{
    if (!_effect || !_editBlockOpen) return;
    _editBlockOpen = false;   // cleared first, so a throw cannot leave it stuck open
    EndEdit(_effect);
}

void MultiTransformInteract::setPreviewGhost(bool on)
{
    // Written unconditionally rather than only on a change. This is the switch
    // that keeps a preview out of a finished frame, and a redundant write costs
    // one redraw where a missed clear would leave a ghost sitting in the render.
    if (!_effect) return;
    try   { _effect->fetchBooleanParam(kParamPreviewGhost)->setValue(on); }
    catch (...) {}
}

void MultiTransformInteract::abandonDrag(const char* why)
{
    // Recovery for a drag that was started and never finished, because the host
    // did not deliver the mouse-up: released outside the viewer, a page switched
    // mid-drag, a modal dialog taking the release. Without this the block stays
    // open for the rest of the session, openEditBlock quietly does nothing ever
    // after, and the host is left holding an edit that never ends.
    if (!_captured && !_editBlockOpen && EditDepthOf(_effect) == 0) return;

    mtx::ProbeLog(std::string("overlay: abandoned drag (") + why + "), depth was "
                  + std::to_string(EditDepthOf(_effect)));

    _captured      = nullptr;
    _editBlockOpen = false;
    setPreviewGhost(false);
    ForceCloseEdits(_effect);
}

bool MultiTransformInteract::buildContext(OverlayContext& out, double time,
                                          const OfxPointD& pixelScale) const
{
    try
    {
        return buildContextUnsafe(out, time, pixelScale);
    }
    catch (...)
    {
        // An exception escaping into the host during a draw or pen action is a
        // far worse outcome than simply not drawing this frame.
        mtx::ProbeOnce("overlay-build-failed", "overlay: buildContext threw; overlay suppressed");
        return false;
    }
}

bool MultiTransformInteract::buildContextUnsafe(OverlayContext& out, double time,
                                                const OfxPointD& pixelScale) const
{
    if (!_effect || !OFX::Private::gDrawSuite) return false;

    OFX::Clip* src = _effect->fetchClip(kOfxImageEffectSimpleSourceClipName);
    if (!src || !src->isConnected()) return false;

    out.effect     = _effect;
    out.pixelScale = pixelScale;
    out.rod        = src->getRegionOfDefinition(time);

    // Everything the overlay shows and edits is in clip time, so the timeline
    // lanes read 0..clip length rather than raw timeline frame numbers, and a
    // dragged bar means the same thing after the clip is moved.
    double clipStart = 0.0, clipLength = 0.0;
    if (GetClipRange(_effect, clipStart, clipLength))
    {
        out.time       = time - clipStart;
        out.clipLength = clipLength;
    }
    else
    {
        out.time       = time;
        out.clipLength = 0.0;
    }

    if (out.rodWidth() <= 0.0 || out.rodHeight() <= 0.0) return false;

    // A degenerate pixel scale would make every screen-relative size collapse
    // to zero and the overlay would silently vanish.
    if (out.pixelScale.x <= 0.0) out.pixelScale.x = 1.0;
    if (out.pixelScale.y <= 0.0) out.pixelScale.y = 1.0;

    out.stageCount  = GetChoice(_effect, kParamStageCount) + 1;
    out.activeStage = GetChoice(_effect, kParamActiveStage);
    const int editTarget = GetChoice(_effect, kParamEditTarget);
    out.editTo      = (editTarget == 1);
    out.editBase    = (editTarget == 2);
    out.showCurve   = GetBool(_effect, kParamShowCurve, time);
    out.showLibrary = GetBool(_effect, kParamShowLibrary, time);
    out.showQuick   = GetBool(_effect, kParamShowQuick, time);
    out.dragPreview = GetBool(_effect, kParamDragPreview, time);
    out.showTimeline= GetBool(_effect, kParamShowTimeline, time);
    out.showPath    = GetBool(_effect, kParamShowPath, time);
    out.showOpacity = GetBool(_effect, kParamShowOpacity, time);
    out.shiftHeld   = _shiftHeld;
    out.ctrlHeld    = _ctrlHeld;

    if (out.activeStage >= out.stageCount) out.activeStage = out.stageCount - 1;
    if (out.activeStage < 0)               out.activeStage = 0;

    // Read the animation straight from the parameters, so the overlay always
    // shows what will actually render.
    out.anim = AnimParams::Default();
    out.anim.stageCount = out.stageCount;
    out.anim.clipStart  = static_cast<float>(clipStart);
    out.anim.clipLength = static_cast<float>(clipLength);

    {
        BasePose& b = out.anim.base;
        b.scaleX    = static_cast<float>(GetDouble(_effect, kParamBaseScale,   time));
        b.scaleY    = static_cast<float>(GetDouble(_effect, kParamBaseScaleY,  time));
        b.linkScale = GetBool  (_effect, kParamBaseLinkScale, time);
        b.rot       = static_cast<float>(GetDouble(_effect, kParamBaseRot,     time));
        b.tiltX     = static_cast<float>(GetDouble(_effect, kParamBaseTiltX,   time));
        b.swivelY   = static_cast<float>(GetDouble(_effect, kParamBaseSwivelY, time));
        b.opacity   = static_cast<float>(GetDouble(_effect, kParamBaseOpacity, time) * 0.01);

        double bx = 0.0, by = 0.0;
        GetDouble2D(_effect, kParamBasePos,    time, bx, by);
        b.posX = static_cast<float>(bx); b.posY = static_cast<float>(by);
        GetDouble2D(_effect, kParamBaseAnchor, time, bx, by);
        b.anchorX = static_cast<float>(bx); b.anchorY = static_cast<float>(by);
    }

    for (int i = 0; i < kMaxStages; ++i)
    {
        Stage& s = out.anim.stages[i];
        s.enabled    = GetBool  (_effect, StageParam(kParamEnabled,    i), time);
        s.anchor = GetChoice(_effect, StageParam(kParamAnchor2, i));

        // Normalised the same way the renderer does, so a not-yet-rewritten
        // legacy value is drawn where it actually plays rather than off-screen.
        // A stage anchored to the timeline stores absolute frames deliberately,
        // so it is left alone.
        const double rawStart = GetDouble(_effect, StageParam(kParamStartFrame, i), time);
        const double rawEnd   = GetDouble(_effect, StageParam(kParamEndFrame,   i), time);
        const bool   relative = (s.anchor != kAnchorTimeline);

        s.startFrame = static_cast<float>(
            relative ? NormaliseStageFrame(rawStart, clipStart) : rawStart);
        s.endFrame   = static_cast<float>(
            relative ? NormaliseStageFrame(rawEnd, clipStart) : rawEnd);
        s.scaleFrom  = static_cast<float>(GetDouble(_effect, StageParam(kParamScaleFrom,  i), time));
        s.scaleTo    = static_cast<float>(GetDouble(_effect, StageParam(kParamScaleTo,    i), time));
        s.rotFrom    = static_cast<float>(GetDouble(_effect, StageParam(kParamRotFrom,    i), time));
        s.rotTo      = static_cast<float>(GetDouble(_effect, StageParam(kParamRotTo,      i), time));
        s.linkScale  = GetBool  (_effect, StageParam(kParamLinkScale,  i), time);
        s.scaleYFrom = static_cast<float>(GetDouble(_effect, StageParam(kParamScaleYFrom, i), time));
        s.scaleYTo   = static_cast<float>(GetDouble(_effect, StageParam(kParamScaleYTo,   i), time));
        s.tiltXFrom  = static_cast<float>(GetDouble(_effect, StageParam(kParamTiltXFrom,  i), time));
        s.tiltXTo    = static_cast<float>(GetDouble(_effect, StageParam(kParamTiltXTo,    i), time));
        s.swivelYFrom = static_cast<float>(GetDouble(_effect, StageParam(kParamSwivelYFrom, i), time));
        s.swivelYTo   = static_cast<float>(GetDouble(_effect, StageParam(kParamSwivelYTo,   i), time));
        s.opacityFrom = static_cast<float>(GetDouble(_effect, StageParam(kParamOpacityFrom, i), time) * 0.01);
        s.opacityTo   = static_cast<float>(GetDouble(_effect, StageParam(kParamOpacityTo,   i), time) * 0.01);

        double x = 0.0, y = 0.0;
        GetDouble2D(_effect, StageParam(kParamPosFrom, i), time, x, y);
        s.posXFrom = static_cast<float>(x); s.posYFrom = static_cast<float>(y);
        GetDouble2D(_effect, StageParam(kParamPosTo, i), time, x, y);
        s.posXTo   = static_cast<float>(x); s.posYTo   = static_cast<float>(y);
        GetDouble2D(_effect, StageParam(kParamAnchor, i), time, x, y);
        s.anchorX  = static_cast<float>(x); s.anchorY  = static_cast<float>(y);
        GetDouble2D(_effect, StageParam(kParamPathC1, i), time, x, y);
        s.pathC1X  = static_cast<float>(x); s.pathC1Y  = static_cast<float>(y);
        GetDouble2D(_effect, StageParam(kParamPathC2, i), time, x, y);
        s.pathC2X  = static_cast<float>(x); s.pathC2Y  = static_cast<float>(y);

        s.easing = MakeEasing(
            static_cast<float>(GetDouble(_effect, StageParam(kParamEaseIn,        i), time)),
            static_cast<float>(GetDouble(_effect, StageParam(kParamEaseOut,       i), time)),
            static_cast<float>(GetDouble(_effect, StageParam(kParamAnticipation,  i), time)),
            static_cast<float>(GetDouble(_effect, StageParam(kParamOvershoot,     i), time)),
            GetChoice(_effect, StageParam(kParamBounceType, i)),
            static_cast<float>(GetDouble(_effect, StageParam(kParamBounceAmount,  i), time)),
            static_cast<float>(GetDouble(_effect, StageParam(kParamBounceCount,   i), time)),
            static_cast<float>(GetDouble(_effect, StageParam(kParamBounceDamping, i), time)),
            static_cast<float>(GetDouble(_effect, StageParam(kParamBounceStart,   i), time)));
    }

    return true;
}

// --- Toolbar -----------------------------------------------------------------

OfxRectD MultiTransformInteract::tabRect(const OverlayContext& c, int index) const
{
    const OfxRectD& tl = _timeline.rect();
    const double w = c.sx(kTabWidthPx);
    const double h = c.sy(kTabHeightPx);
    const double g = c.sx(kTabGapPx);

    OfxRectD r;
    r.x1 = tl.x1 + static_cast<double>(index) * (w + g);
    r.x2 = r.x1 + w;
    r.y1 = tl.y2 + c.sy(6.0);
    r.y2 = r.y1 + h;
    return r;
}

double MultiTransformInteract::toggleWidth(const OverlayContext& c) const
{
    // Sized to the room actually available rather than fixed.
    //
    // The buttons are a fixed size *on screen* while the timeline they sit above
    // is a fraction of the *image*, so the further the viewer is zoomed out the
    // more of that strip they occupy. At a fixed width the right-hand run
    // eventually runs into the stage tabs coming the other way -- which looks
    // exactly like the buttons being drawn in the wrong place.
    const OfxRectD& tl = _timeline.rect();
    const double scale   = c.pixelScale.x > 1e-9 ? c.pixelScale.x : 1.0;
    const double totalPx = (tl.x2 - tl.x1) / scale;

    // The left group is a fixed width now: all four tab slots are always drawn
    // (the unused ones as ghosts), then the remove button, then the enable
    // toggle. Sizing it from the live stage count would make the right-hand run
    // resize every time a stage was added, which is exactly the reflow the
    // fixed slots exist to avoid.
    const double leftPx = static_cast<double>(kMaxStages) * (kTabWidthPx + kTabGapPx)
                        + (kTabWidthPx + kTabGapPx * 2.0);

    const double avail = totalPx - leftPx - kTabGapPx * (kToggleCount + 2);
    double w = avail / (kToggleCount + 1);          // +1 for the enable toggle

    if (w > kToggleWPx) w = kToggleWPx;
    if (w < kToggleMinPx) w = kToggleMinPx;         // unreadable below this anyway
    return w;
}

OfxRectD MultiTransformInteract::rightButtonRect(const OverlayContext& c,
                                                 int indexFromRight, double /*widthPx*/,
                                                 int row) const
{
    const OfxRectD& tl = _timeline.rect();
    const double w   = c.sx(toggleWidth(c));
    const double h   = c.sy(kTabHeightPx);
    const double gap = c.sx(kTabGapPx);

    // Every button in the row is the same width, so stepping by index is enough.
    // A second row keeps the first from running past the left edge of the
    // timeline once there are more than five or six of them.
    OfxRectD r;
    r.x2 = tl.x2 - static_cast<double>(indexFromRight) * (w + gap);
    r.x1 = r.x2 - w;
    r.y1 = tl.y2 + c.sy(6.0) + static_cast<double>(row) * (h + c.sy(4.0));
    r.y2 = r.y1 + h;
    return r;
}

OfxRectD MultiTransformInteract::fromToRect(const OverlayContext& c, bool toButton) const
{
    return rightButtonRect(c, toButton ? 0 : 1, kToggleWPx);
}

OfxRectD MultiTransformInteract::baseTargetRect(const OverlayContext& c) const
{
    return rightButtonRect(c, 2, kToggleWPx);
}

OfxRectD MultiTransformInteract::dragPreviewRect(const OverlayContext& c) const
{
    // On the gizmo's own row rather than with the panel toggles above: this is
    // not a panel, it is how the gizmo behaves while being dragged.
    return rightButtonRect(c, 3, kToggleWPx);
}

OfxRectD MultiTransformInteract::quickToggleRect(const OverlayContext& c) const
{
    // On the lower row with the editing controls rather than the upper one with
    // the panel toggles: what it raises are edits -- copies, swaps, a flatten --
    // not another view of the animation.
    return rightButtonRect(c, 4, kToggleWPx);
}

OfxRectD MultiTransformInteract::curveToggleRect(const OverlayContext& c) const
{
    return rightButtonRect(c, 2, kToggleWPx, 1);
}

OfxRectD MultiTransformInteract::libraryToggleRect(const OverlayContext& c) const
{
    return rightButtonRect(c, 1, kToggleWPx, 1);
}

OfxRectD MultiTransformInteract::loadPresetRect(const OverlayContext& c) const
{
    return rightButtonRect(c, 0, kToggleWPx, 1);
}

OfxRectD MultiTransformInteract::timelineToggleRect(const OverlayContext& c) const
{
    return rightButtonRect(c, 5, kToggleWPx, 1);
}

OfxRectD MultiTransformInteract::pathToggleRect(const OverlayContext& c) const
{
    return rightButtonRect(c, 4, kToggleWPx, 1);
}

OfxRectD MultiTransformInteract::opacityToggleRect(const OverlayContext& c) const
{
    return rightButtonRect(c, 3, kToggleWPx, 1);
}

OfxRectD MultiTransformInteract::removeStageRect(const OverlayContext& c) const
{
    // Sits after *all four* tab slots, not after the ones currently in use.
    //
    // Every slot is drawn whatever the stage count is -- the unused ones as
    // ghosts you click to add a stage -- so the row never reflows. A button that
    // moved as the count changed would slide out from under the cursor exactly
    // when it is being clicked repeatedly, which is the one thing it must not do.
    const OfxRectD& tl = _timeline.rect();
    const double g = c.sx(kTabGapPx);

    OfxRectD r;
    r.x1 = tl.x1 + static_cast<double>(kMaxStages) * (c.sx(kTabWidthPx) + g);
    r.x2 = r.x1 + c.sx(kTabWidthPx);
    r.y1 = tl.y2 + c.sy(6.0);
    r.y2 = r.y1 + c.sy(kTabHeightPx);
    return r;
}

OfxRectD MultiTransformInteract::enableStageRect(const OverlayContext& c) const
{
    // On the left, after the tabs and the remove button, rather than at the end
    // of the right-hand run. It belongs with the stage selection, and another
    // right-hand button would have grown that row into the tabs: the buttons are
    // a fixed size *on screen* while the timeline is a fraction of the image, so
    // the further the viewer is zoomed out the more of the row they occupy.
    const OfxRectD& tl = _timeline.rect();

    OfxRectD r;
    r.x1 = removeStageRect(c).x2 + c.sx(kTabGapPx) * 2.0;
    r.x2 = r.x1 + c.sx(toggleWidth(c));
    r.y1 = tl.y2 + c.sy(6.0);
    r.y2 = r.y1 + c.sy(kTabHeightPx);
    return r;
}
void MultiTransformInteract::fireTrigger(const char* paramName)
{
    // Flipping a hidden boolean is the press. Not wrapped in an edit block: the
    // host dispatches changedParam synchronously, and these handlers open their
    // own blocks -- and Flatten can raise a modal warning, which must not run
    // inside somebody else's paramEditBegin.
    OFX::BooleanParam* t = _effect->fetchBooleanParam(paramName);
    bool v = false;
    t->getValue(v);
    t->setValue(!v);
}

void MultiTransformInteract::drawToolbar(const OverlayContext& c)
{
    // All four slots, always. The ones past the stage count are ghosts: click
    // one to raise the count to it, which is how stages are added without
    // going to the Inspector for the Stage Count dropdown.
    for (int i = 0; i < kMaxStages; ++i)
    {
        const OfxRectD r = tabRect(c, i);

        if (i >= c.stageCount)
        {
            // Hollow, so it reads as a slot waiting to be used rather than as a
            // stage that happens to be switched off -- which is what a dimmed
            // solid tab already means.
            SetColour(c, { 1.0f, 1.0f, 1.0f, 0.05f });
            FillRect(c, r.x1, r.y1, r.x2, r.y2);
            SetColour(c, { 1.0f, 1.0f, 1.0f, 0.22f });
            SetLineWidth(c, 1.0f);
            StrokeRect(c, r.x1, r.y1, r.x2, r.y2);

            SetColour(c, colours::kTextDim);
            const OfxPointD tp = { r.x1 + c.sx(6.0), (r.y1 + r.y2) * 0.5 - c.sy(4.0) };
            OFX::Private::gDrawSuite->drawText(c.ctx, std::to_string(i + 1).c_str(), &tp,
                                               kOfxDrawTextAlignmentLeft
                                               | kOfxDrawTextAlignmentBaseline);
            continue;
        }

        // A disabled stage reads as dimmed rather than bracketed. Brackets put
        // three characters in a 26px box, which was one of the labels running
        // outside its button -- and dimming says the same thing more quietly.
        const bool active = (i == c.activeStage);
        const bool on     = c.anim.stages[i].enabled;

        Button(c, r, std::to_string(i + 1), active && on,
               on ? colours::kStage[i % 4] : colours::kTextDim);
    }

    // Drops the last stage. Dimmed rather than hidden at one stage, so the row
    // stays the same shape whatever the count is.
    Button(c, removeStageRect(c), "-", false, colours::kAccent);

    // Enable / disable the active stage. A new stage arrives switched off, and
    // digging into the Timing section to turn it on was a step every single
    // time.
    {
        const bool on = c.anim.stages[c.activeStage].enabled;
        Button(c, enableStageRect(c), on ? "ON" : "OFF", on, colours::kStage[c.activeStage % 4]);
    }

    // From / To decides which end of the stage the gizmo poses.
    for (int k = 0; k < 2; ++k)
    {
        const bool toButton = (k == 1);
        Button(c, fromToRect(c, toButton), toButton ? "TO" : "FROM",
               toButton == c.editTo, toButton ? colours::kGizmoTo : colours::kGizmo);
    }

    // Poses the resting pose instead of a stage end, so the base can be set by
    // dragging on the picture rather than by typing in the Inspector.
    Button(c, baseTargetRect(c), "BASE", c.editBase, colours::kStage[3]);
    Button(c, dragPreviewRect(c), "GHOST", c.dragPreview, colours::kAccent);

    // Raises the Quick Control panel. Lit while it is up, since that is a state
    // -- unlike LOAD beside it, which is momentary.
    Button(c, quickToggleRect(c), "QUICK", c.showQuick, colours::kAccent);

    // Upper row: what is on screen. Every toggle stays visible while its panel
    // is hidden, or there would be no way to bring it back.
    //
    // Split from the row below on purpose. That one is "what am I editing" --
    // which stage, which end of it -- and this one is "what am I looking at".
    // Nine buttons in a single run outgrew the timeline they sit above, and
    // grouping them by kind reads better than sorting them by how often they
    // are used.
    Button(c, timelineToggleRect(c), "TIME",  c.showTimeline, colours::kAccent);
    Button(c, pathToggleRect(c),     "PATH",  c.showPath,     colours::kAccent);
    Button(c, opacityToggleRect(c),  "OPAC",  c.showOpacity,  colours::kAccent);
    Button(c, curveToggleRect(c),    "CURVE", c.showCurve,    colours::kAccent);
    Button(c, libraryToggleRect(c),  "LIB",   c.showLibrary,  colours::kAccent);

    // Load a preset. Momentary, so never lit.
    Button(c, loadPresetRect(c), "LOAD", false, colours::kAccent);
}
bool MultiTransformInteract::toolbarHit(const OverlayContext& c, const OfxPointD& p)
{
    // Each branch brackets its own write. A plugin-initiated paramSetValue that
    // is not inside an edit block leaves the host unaware the value changed.
    for (int i = 0; i < kMaxStages; ++i)
    {
        if (!Contains(tabRect(c, i), p)) continue;

        if (i < c.stageCount)
        {
            EditBlock block(_effect, "Select Stage");
            _effect->fetchChoiceParam(kParamActiveStage)->setValue(i);
            return true;
        }

        // A ghost slot: raise the count to include it, and select it, since
        // clicking a slot is unambiguously "I want to work on this one".
        //
        // Deliberately not switched on as well. Stage Count and Enabled are
        // separate gates and enabling changes what renders, so that stays an
        // explicit press of ON -- which is right beside this.
        EditBlock block(_effect, "Add Stage");
        _effect->fetchChoiceParam(kParamStageCount)->setValue(i);   // 0-based: i == i+1 stages
        _effect->fetchChoiceParam(kParamActiveStage)->setValue(i);
        return true;
    }

    if (Contains(removeStageRect(c), p))
    {
        if (c.stageCount > 1)
        {
            // The dropped stage keeps its values: Stage Count only gates what
            // renders, so raising the count again brings the work back rather
            // than handing over a blank stage.
            EditBlock block(_effect, "Remove Stage");
            _effect->fetchChoiceParam(kParamStageCount)->setValue(c.stageCount - 2);
        }
        return true;
    }
    for (int k = 0; k < 2; ++k)
    {
        if (Contains(fromToRect(c, k == 1), p))
        {
            EditBlock block(_effect, "Gizmo Edits");
            _effect->fetchChoiceParam(kParamEditTarget)->setValue(k);
            return true;
        }
    }
    if (Contains(enableStageRect(c), p))
    {
        EditBlock block(_effect, "Enable Stage");
        OFX::BooleanParam* en =
            _effect->fetchBooleanParam(StageParam(kParamEnabled, c.activeStage));
        bool on = false;
        en->getValue(on);
        en->setValue(!on);
        return true;
    }
    if (Contains(loadPresetRect(c), p))
    {
        // Flipping the hidden trigger is the press: OFX offers no way to fire a
        // push button from code, and changedParam is where the dialog belongs.
        //
        // Deliberately NOT inside an EditBlock. The host dispatches changedParam
        // synchronously from the setValue below, so the file dialog opens inside
        // this call -- and a modal Win32 dialog pumps its own message loop. Doing
        // that between a paramEditBegin and its End means the host is holding an
        // open edit while the user is free to click elsewhere, switch pages, and
        // re-enter this interact. Nothing is being edited yet in any case: the
        // trigger is hidden and not persisted, so there is no undo step to group.
        fireTrigger(kParamLoadFromOverlay);
        return true;
    }
    if (Contains(libraryToggleRect(c), p))
    {
        // Re-scanned every time it opens, so a curve saved since the last look
        // is there, and a file deleted outside Resolve is not.
        _library.invalidate();

        EditBlock block(_effect, "Show Curve Library");
        _effect->fetchBooleanParam(kParamShowLibrary)->setValue(!c.showLibrary);
        return true;
    }
    if (Contains(quickToggleRect(c), p))
    {
        EditBlock block(_effect, "Quick Control");
        _effect->fetchBooleanParam(kParamShowQuick)->setValue(!c.showQuick);
        return true;
    }
    if (Contains(dragPreviewRect(c), p))
    {
        EditBlock block(_effect, "Drag Preview");
        _effect->fetchBooleanParam(kParamDragPreview)->setValue(!c.dragPreview);
        return true;
    }
    if (Contains(baseTargetRect(c), p))
    {
        EditBlock block(_effect, "Gizmo Edits");
        _effect->fetchChoiceParam(kParamEditTarget)->setValue(2);
        return true;
    }
    if (Contains(curveToggleRect(c), p))
    {
        EditBlock block(_effect, "Show Curve Editor");
        _effect->fetchBooleanParam(kParamShowCurve)->setValue(!c.showCurve);
        return true;
    }

    // The remaining panel toggles, all the same shape.
    struct PanelToggle
    {
        OfxRectD (MultiTransformInteract::*rect)(const OverlayContext&) const;
        const char* param;
        bool        current;
        const char* undoName;
    };
    const PanelToggle panels[3] = {
        { &MultiTransformInteract::timelineToggleRect, kParamShowTimeline,
          c.showTimeline, "Show Timeline" },
        { &MultiTransformInteract::pathToggleRect,     kParamShowPath,
          c.showPath,     "Show Motion Path" },
        { &MultiTransformInteract::opacityToggleRect,  kParamShowOpacity,
          c.showOpacity,  "Show Opacity Slider" },
    };

    for (const PanelToggle& t : panels)
    {
        if (!Contains((this->*t.rect)(c), p)) continue;

        EditBlock block(_effect, t.undoName);
        _effect->fetchBooleanParam(t.param)->setValue(!t.current);
        return true;
    }

    return false;
}

// --- Modifier tracking -------------------------------------------------------

bool MultiTransformInteract::isShift(int keySymbol)
{
    return keySymbol == kOfxKey_Shift_L || keySymbol == kOfxKey_Shift_R;
}

bool MultiTransformInteract::isCtrl(int keySymbol)
{
    return keySymbol == kOfxKey_Control_L || keySymbol == kOfxKey_Control_R;
}

bool MultiTransformInteract::keyDown(const OFX::KeyArgs& args)
{
    const bool modifier = isShift(args.keySymbol) || isCtrl(args.keySymbol);

    if (isShift(args.keySymbol)) _shiftHeld = true;
    if (isCtrl (args.keySymbol)) _ctrlHeld  = true;

    // Any *other* key pressed mid-drag ends the drag.
    //
    // This is the recovery path that actually fires for the reproducible case:
    // press Tab while dragging a handle and Resolve moves keyboard focus, so the
    // mouse-up is delivered somewhere else and never reaches this interact. The
    // edit block stays open, the host keeps waiting for an edit that will never
    // end, and the viewer's on-screen controls stop responding everywhere --
    // including in Fusion -- until Resolve is restarted.
    //
    // loseFocus cannot be relied on here (the host need not send it), and the
    // penDown backstop cannot help either: with the overlay already wedged
    // there is no way to click and trigger it. A key press is the one event
    // still arriving at that moment, which makes it the place to recover.
    //
    // Shift and Control are excluded because they are pressed *during* drags on
    // purpose -- axis lock and paired handles -- and cancelling on those would
    // break the two features that depend on them.
    if (!modifier && (_captured || _editBlockOpen))
        abandonDrag("key pressed during a drag");

    // Never trapped. Returning true would block the key from every other
    // interact and from the host, so a plugin that merely wants to *observe*
    // Shift would swallow the host's own Shift shortcuts along with it.
    return false;
}

bool MultiTransformInteract::keyUp(const OFX::KeyArgs& args)
{
    if (isShift(args.keySymbol)) _shiftHeld = false;
    if (isCtrl (args.keySymbol)) _ctrlHeld  = false;
    return false;
}

void MultiTransformInteract::gainFocus(const OFX::FocusArgs& /*args*/)
{
    // Anything still marked as a drag when focus comes back belongs to an
    // interaction that ended somewhere else. Clearing it here means a wedge
    // survives at most until the viewer is clicked back into, whatever route
    // created it.
    abandonDrag("focus regained with a drag still open");

    // Modifier state cannot be trusted across a focus change: a key released
    // while another panel had focus produces no keyUp here.
    _shiftHeld = false;
    _ctrlHeld  = false;
}

void MultiTransformInteract::loseFocus(const OFX::FocusArgs& /*args*/)
{
    _shiftHeld = false;
    _ctrlHeld  = false;

    // Losing focus mid-drag means the mouse-up is going somewhere else and will
    // never arrive here.
    abandonDrag("focus lost");
}

// --- Interact actions --------------------------------------------------------

bool MultiTransformInteract::draw(const OFX::DrawArgs& args)
{
    OverlayContext c;
    c.ctx = args.context;
    if (!buildContext(c, args.time, args.pixelScale)) return false;
    if (!c.ctx) return false;

    try
    {
        _timeline.layout(c);
        _curve.layout(c);
        _gizmo.layout(c);
        _path.layout(c);
        _opacity.layout(c);
        _quick.layout(c);

        // Back to front: the gizmo lives on the image, the HUD panels above it.
        // The path sits over the gizmo: its handles are small and must not be
        // buried under the gizmo's outline.
        _gizmo.draw(c);
        if (c.showPath)     _path.draw(c);
        if (c.showOpacity)  _opacity.draw(c);
        if (c.showTimeline) _timeline.draw(c);
        if (c.showCurve)    _curve.draw(c);
        drawToolbar(c);

        // Last, and therefore on top of everything: these are modal pickers.
        if (c.showQuick)   _quick.draw(c);
        if (c.showLibrary) _library.draw(c);
    }
    catch (...)
    {
        mtx::ProbeOnce("overlay-draw-failed", "overlay: draw threw; overlay suppressed");
        return false;
    }

    return true;
}

bool MultiTransformInteract::penDown(const OFX::PenArgs& args)
{
    // A fresh press means any previous drag is over, however it ended. If one is
    // still marked as in progress the mouse-up for it never arrived, so clear it
    // here rather than letting it accumulate. This is the backstop for the cases
    // loseFocus does not see.
    abandonDrag("new press while a drag was still open");

    OverlayContext c;
    if (!buildContext(c, args.time, args.pixelScale)) return false;

    try
    {
        _timeline.layout(c);
        _curve.layout(c);
        _gizmo.layout(c);
        _path.layout(c);
        _opacity.layout(c);
        _quick.layout(c);

        const OfxPointD p = args.penPosition;

        if (toolbarHit(c, p)) { requestRedraw(); return true; }

        // The Quick panel is handled here rather than in the widget order below,
        // because that loop opens an edit block before offering the event and
        // these actions must not run inside one: they open their own, and
        // Flatten and Paste can raise a modal dialog, which pumps its own
        // message loop. Same reasoning as the LOAD button in toolbarHit.
        //
        // Behind the library, which is modal in its own right and drawn on top.
        if (c.showQuick && !c.showLibrary && _quick.penDown(c, p))
        {
            requestRedraw();
            return true;
        }

        // Front to back, the reverse of drawing order, so a click lands on
        // whatever is visually on top. A hidden curve panel is skipped entirely
        // rather than merely not drawn -- otherwise it would keep swallowing
        // clicks aimed at the motion path handles beneath it.
        // The library is first because it is modal: while it is up it takes
        // every click inside itself, and nothing beneath it should react.
        // A hidden panel is skipped entirely rather than merely not drawn.
        // Leaving one in the order would have it swallow clicks aimed at
        // whatever is underneath -- which the curve editor did to the motion
        // path handles once already.
        Widget* order[6] = { c.showLibrary  ? &_library  : nullptr,
                             c.showCurve    ? &_curve    : nullptr,
                             c.showOpacity  ? &_opacity  : nullptr,
                             c.showTimeline ? &_timeline : nullptr,
                             c.showPath     ? &_path     : nullptr,
                             &_gizmo };
        for (Widget* w : order)
        {
            if (!w) continue;

            // Opened before the widget writes anything, and held for the whole
            // drag: every paramSetValue between here and penUp belongs to one
            // edit. That is both what tells the host an edit happened at all,
            // and what makes a drag a single undo step rather than one per
            // mouse-move.
            openEditBlock("Multi Transform");

            if (w->penDown(c, p))
            {
                _captured = w;

                // Ask the renderer for a faded copy of the source at the pose
                // being dragged. Only for the gizmo: it is the only widget that
                // poses the image, and the only one where the viewer is showing
                // a different frame from the one being edited.
                if (w == &_gizmo && c.dragPreview) setPreviewGhost(true);

                requestRedraw();
                return true;
            }

            // That widget did not want it; close before offering the next.
            closeEditBlock();
        }
    }
    catch (...)
    {
        mtx::ProbeOnce("overlay-pendown-failed", "overlay: penDown threw");
        closeEditBlock();
        setPreviewGhost(false);
        _captured = nullptr;
        return false;
    }

    _captured = nullptr;
    return false;
}

bool MultiTransformInteract::penMotion(const OFX::PenArgs& args)
{
    if (!_captured) return false;

    OverlayContext c;
    if (!buildContext(c, args.time, args.pixelScale)) return false;

    try
    {
        _timeline.layout(c);
        _curve.layout(c);
        _gizmo.layout(c);
        _path.layout(c);
        _opacity.layout(c);

        if (_captured->penMotion(c, args.penPosition))
        {
            requestRedraw();
            return true;
        }
    }
    catch (...)
    {
        // Drop the capture: continuing to feed a widget that just failed to
        // write a parameter would repeat the failure on every mouse move.
        mtx::ProbeOnce("overlay-penmotion-failed", "overlay: penMotion threw; drag cancelled");
        closeEditBlock();
        setPreviewGhost(false);
        _captured = nullptr;
    }
    return false;
}

bool MultiTransformInteract::penUp(const OFX::PenArgs& args)
{
    if (!_captured) return false;

    OverlayContext c;
    const bool ok = buildContext(c, args.time, args.pixelScale);

    bool handled = false;
    try
    {
        handled = _captured->penUp(c, args.penPosition);
    }
    catch (...)
    {
        mtx::ProbeOnce("overlay-penup-failed", "overlay: penUp threw");
    }
    _captured = nullptr;
    setPreviewGhost(false);

    // Closing the block is what commits the drag as far as the host is
    // concerned, so it must happen however the drag ended.
    closeEditBlock();

    if (ok && handled) requestRedraw();
    return handled;
}

} // namespace mtx
