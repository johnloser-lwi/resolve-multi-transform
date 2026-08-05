#include "OverlayInteract.h"

#include "AnimEngine.h"
#include "HostProbe.h"
#include "ParamNames.h"

#include <cmath>

namespace mtx {

namespace {
constexpr double kTabWidthPx  = 26.0;
constexpr double kTabHeightPx = 18.0;
constexpr double kTabGapPx    = 4.0;
constexpr double kToggleWPx   = 46.0;
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
    out.time       = time;
    out.pixelScale = pixelScale;
    out.rod        = src->getRegionOfDefinition(time);

    if (out.rodWidth() <= 0.0 || out.rodHeight() <= 0.0) return false;

    // A degenerate pixel scale would make every screen-relative size collapse
    // to zero and the overlay would silently vanish.
    if (out.pixelScale.x <= 0.0) out.pixelScale.x = 1.0;
    if (out.pixelScale.y <= 0.0) out.pixelScale.y = 1.0;

    out.stageCount  = GetChoice(_effect, kParamStageCount) + 1;
    out.activeStage = GetChoice(_effect, kParamActiveStage);
    out.editTo      = GetChoice(_effect, kParamEditTarget) != 0;
    out.showCurve   = GetBool(_effect, kParamShowCurve, time);

    if (out.activeStage >= out.stageCount) out.activeStage = out.stageCount - 1;
    if (out.activeStage < 0)               out.activeStage = 0;

    // Read the animation straight from the parameters, so the overlay always
    // shows what will actually render.
    out.anim = AnimParams::Default();
    out.anim.stageCount = out.stageCount;
    for (int i = 0; i < kMaxStages; ++i)
    {
        Stage& s = out.anim.stages[i];
        s.enabled    = GetBool  (_effect, StageParam(kParamEnabled,    i), time);
        s.startFrame = static_cast<float>(GetDouble(_effect, StageParam(kParamStartFrame, i), time));
        s.endFrame   = static_cast<float>(GetDouble(_effect, StageParam(kParamEndFrame,   i), time));
        s.scaleFrom  = static_cast<float>(GetDouble(_effect, StageParam(kParamScaleFrom,  i), time));
        s.scaleTo    = static_cast<float>(GetDouble(_effect, StageParam(kParamScaleTo,    i), time));
        s.rotFrom    = static_cast<float>(GetDouble(_effect, StageParam(kParamRotFrom,    i), time));
        s.rotTo      = static_cast<float>(GetDouble(_effect, StageParam(kParamRotTo,      i), time));
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

OfxRectD MultiTransformInteract::rightButtonRect(const OverlayContext& c,
                                                 int indexFromRight, double widthPx) const
{
    const OfxRectD& tl = _timeline.rect();
    const double w   = c.sx(widthPx);
    const double h   = c.sy(kTabHeightPx);
    const double gap = c.sx(kTabGapPx);

    // Every button in the row is the same width, so stepping by index is enough.
    OfxRectD r;
    r.x2 = tl.x2 - static_cast<double>(indexFromRight) * (c.sx(kToggleWPx) + gap);
    r.x1 = r.x2 - w;
    r.y1 = tl.y2 + c.sy(6.0);
    r.y2 = r.y1 + h;
    return r;
}

OfxRectD MultiTransformInteract::fromToRect(const OverlayContext& c, bool toButton) const
{
    return rightButtonRect(c, toButton ? 0 : 1, kToggleWPx);
}

OfxRectD MultiTransformInteract::curveToggleRect(const OverlayContext& c) const
{
    return rightButtonRect(c, 2, kToggleWPx);
}

void MultiTransformInteract::drawToolbar(const OverlayContext& c)
{
    for (int i = 0; i < c.stageCount; ++i)
    {
        const OfxRectD r = tabRect(c, i);
        const bool active = (i == c.activeStage);

        SetColour(c, active ? colours::kStage[i % 4] : colours::kPanel);
        FillRect(c, r.x1, r.y1, r.x2, r.y2);
        SetColour(c, active ? colours::kHandle : colours::kPanelEdge);
        SetLineWidth(c, 1.0f);
        StrokeRect(c, r.x1, r.y1, r.x2, r.y2);

        SetColour(c, active ? Colour{ 0.05f, 0.06f, 0.08f, 1.0f } : colours::kText);
        Text(c, std::to_string(i + 1), (r.x1 + r.x2) * 0.5, (r.y1 + r.y2) * 0.5,
             kOfxDrawTextAlignmentCenterH | kOfxDrawTextAlignmentCenterV);
    }

    // From / To decides which end of the stage the gizmo poses.
    for (int k = 0; k < 2; ++k)
    {
        const bool toButton = (k == 1);
        const OfxRectD r = fromToRect(c, toButton);
        const bool active = (toButton == c.editTo);

        SetColour(c, active ? (toButton ? colours::kGizmoTo : colours::kGizmo) : colours::kPanel);
        FillRect(c, r.x1, r.y1, r.x2, r.y2);
        SetColour(c, active ? colours::kHandle : colours::kPanelEdge);
        SetLineWidth(c, 1.0f);
        StrokeRect(c, r.x1, r.y1, r.x2, r.y2);

        SetColour(c, active ? Colour{ 0.05f, 0.06f, 0.08f, 1.0f } : colours::kText);
        Text(c, toButton ? "TO" : "FROM", (r.x1 + r.x2) * 0.5, (r.y1 + r.y2) * 0.5,
             kOfxDrawTextAlignmentCenterH | kOfxDrawTextAlignmentCenterV);
    }

    // Curve editor toggle. Stays visible when the panel is hidden, or there
    // would be no way to bring it back.
    {
        const OfxRectD r = curveToggleRect(c);

        SetColour(c, c.showCurve ? colours::kAccent : colours::kPanel);
        FillRect(c, r.x1, r.y1, r.x2, r.y2);
        SetColour(c, c.showCurve ? colours::kHandle : colours::kPanelEdge);
        SetLineWidth(c, 1.0f);
        StrokeRect(c, r.x1, r.y1, r.x2, r.y2);

        SetColour(c, c.showCurve ? Colour{ 0.05f, 0.06f, 0.08f, 1.0f } : colours::kTextDim);
        Text(c, "CURVE", (r.x1 + r.x2) * 0.5, (r.y1 + r.y2) * 0.5,
             kOfxDrawTextAlignmentCenterH | kOfxDrawTextAlignmentCenterV);
    }
}

bool MultiTransformInteract::toolbarHit(const OverlayContext& c, const OfxPointD& p)
{
    for (int i = 0; i < c.stageCount; ++i)
    {
        if (Contains(tabRect(c, i), p))
        {
            _effect->fetchChoiceParam(kParamActiveStage)->setValue(i);
            return true;
        }
    }
    for (int k = 0; k < 2; ++k)
    {
        if (Contains(fromToRect(c, k == 1), p))
        {
            _effect->fetchChoiceParam(kParamEditTarget)->setValue(k);
            return true;
        }
    }
    if (Contains(curveToggleRect(c), p))
    {
        _effect->fetchBooleanParam(kParamShowCurve)->setValue(!c.showCurve);
        return true;
    }
    return false;
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

        // Back to front: the gizmo lives on the image, the HUD panels above it.
        // The path sits over the gizmo: its handles are small and must not be
        // buried under the gizmo's outline.
        _gizmo.draw(c);
        _path.draw(c);
        _timeline.draw(c);
        if (c.showCurve) _curve.draw(c);
        drawToolbar(c);
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
    OverlayContext c;
    if (!buildContext(c, args.time, args.pixelScale)) return false;

    try
    {
        _timeline.layout(c);
        _curve.layout(c);
        _gizmo.layout(c);
        _path.layout(c);

        const OfxPointD p = args.penPosition;

        if (toolbarHit(c, p)) { requestRedraw(); return true; }

        // Front to back, the reverse of drawing order, so a click lands on
        // whatever is visually on top. A hidden curve panel is skipped entirely
        // rather than merely not drawn -- otherwise it would keep swallowing
        // clicks aimed at the motion path handles beneath it.
        Widget* order[4] = { c.showCurve ? &_curve : nullptr, &_timeline, &_path, &_gizmo };
        for (Widget* w : order)
        {
            if (!w) continue;
            if (w->penDown(c, p))
            {
                _captured = w;
                requestRedraw();
                return true;
            }
        }
    }
    catch (...)
    {
        mtx::ProbeOnce("overlay-pendown-failed", "overlay: penDown threw");
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

    if (ok && handled) requestRedraw();
    return handled;
}

} // namespace mtx
