#include "Widgets.h"

#include "AnimEngine.h"
#include "ParamNames.h"

#include <algorithm>
#include <cmath>

namespace mtx {

namespace {
constexpr double kTrackWPx   = 10.0;
constexpr double kTrackHPx   = 150.0;
constexpr double kInsetPx    = 22.0;
constexpr double kKnobPx     = 7.0;
constexpr double kGrabPx     = 12.0;

double Clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
} // namespace

void OpacityWidget::layout(const OverlayContext& c)
{
    // Left edge, vertically centred. The bottom of the frame belongs to the
    // timeline and the top right to the curve editor, so this is the one side
    // with nothing to collide with.
    const double h = c.sy(kTrackHPx);

    _track.x1 = c.rod.x1 + c.sx(kInsetPx);
    _track.x2 = _track.x1 + c.sx(kTrackWPx);
    _track.y1 = c.rod.y1 + (c.rodHeight() - h) * 0.5;
    _track.y2 = _track.y1 + h;
}

double OpacityWidget::value(const OverlayContext& c) const
{
    // Read from the same snapshot everything else uses, so the knob cannot
    // disagree with what will render. AnimParams keeps opacity as 0..1 while
    // the parameters are percentages, hence the scaling here and in write().
    if (c.editBase) return c.anim.base.opacity * 100.0;

    const Stage& s = c.anim.stages[c.activeStage];
    return (c.editTo ? s.opacityTo : s.opacityFrom) * 100.0;
}

void OpacityWidget::write(const OverlayContext& c, double percent) const
{
    const double v = std::max(0.0, std::min(100.0, percent));

    if (c.editBase) { SetDouble(c.effect, kParamBaseOpacity, v); return; }

    SetDouble(c.effect,
              StageParam(c.editTo ? kParamOpacityTo : kParamOpacityFrom, c.activeStage), v);
}

double OpacityWidget::valueAt(const OverlayContext& c, double y) const
{
    const double h = _track.y2 - _track.y1;
    if (h < 1e-9) return value(c);
    return Clamp01((y - _track.y1) / h) * 100.0;
}

void OpacityWidget::draw(const OverlayContext& c)
{
    layout(c);

    const double pct = value(c);
    const double f   = Clamp01(pct * 0.01);
    const double knobY = _track.y1 + f * (_track.y2 - _track.y1);

    // The target's own colour, so the slider says which of From / To / Base it
    // is acting on without needing a label for it.
    const Colour tint = c.editBase ? colours::kStage[3]
                                   : (c.editTo ? colours::kGizmoTo : colours::kGizmo);

    // Track, then the filled portion below the knob.
    SetColour(c, { 0.0f, 0.0f, 0.0f, 0.55f });
    FillRect(c, _track.x1, _track.y1, _track.x2, _track.y2);

    SetColour(c, tint);
    FillRect(c, _track.x1, _track.y1, _track.x2, knobY);

    SetColour(c, colours::kPanelEdge);
    SetLineWidth(c, 1.0f);
    StrokeRect(c, _track.x1, _track.y1, _track.x2, _track.y2);

    Handle(c, (_track.x1 + _track.x2) * 0.5, knobY, colours::kHandle, kKnobPx);

    // Value above the track, and what it belongs to below it.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f%%", pct);

    // Haloed: unlike the track, these sit straight on the picture with no dark
    // backdrop of their own.
    HaloText(c, buf, _track.x1, _track.y2 + c.sy(6.0), colours::kText,
             kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBottom);

    HaloText(c, "OPACITY", _track.x1, _track.y1 - c.sy(6.0), colours::kText,
             kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentTop);
}

bool OpacityWidget::penDown(const OverlayContext& c, const OfxPointD& p)
{
    // A generous grab box: the track is deliberately thin to stay out of the
    // way, which would make it fiddly to hit at its drawn width.
    const bool onTrack = p.x >= _track.x1 - c.sx(kGrabPx)
                      && p.x <= _track.x2 + c.sx(kGrabPx)
                      && p.y >= _track.y1 - c.sy(kGrabPx)
                      && p.y <= _track.y2 + c.sy(kGrabPx);
    if (!onTrack) return false;

    // Repeat-click restores full opacity, matching the gizmo's handles: each
    // control resets exactly what it drives.
    if (_clicks.isDouble(c, p, 1))
    {
        write(c, 100.0);
        _drag = 0;
        return true;
    }

    // Clicking anywhere on the track jumps the knob there, so a value can be set
    // with one press rather than having to find the knob first.
    _drag = 1;
    write(c, valueAt(c, p.y));
    return true;
}

bool OpacityWidget::penMotion(const OverlayContext& c, const OfxPointD& p)
{
    if (_drag == 0) return false;
    write(c, valueAt(c, p.y));
    return true;
}

bool OpacityWidget::penUp(const OverlayContext& /*c*/, const OfxPointD& /*p*/)
{
    if (_drag == 0) return false;
    _drag = 0;
    return true;
}

} // namespace mtx
