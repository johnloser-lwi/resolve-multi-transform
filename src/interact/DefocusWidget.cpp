#include "Widgets.h"

#include "AnimEngine.h"
#include "ParamNames.h"

#include <algorithm>
#include <cmath>

namespace mtx {

namespace {
constexpr double kTrackWPx   = 10.0;
constexpr double kTrackHPx   = 150.0;
// Beside the opacity track, with room for its "OPACITY" label between them.
// A fixed slot, not "next to whatever is showing": a slider that slid sideways
// when the other was toggled would move out from under the cursor.
constexpr double kInsetPx    = 22.0 + 80.0;
constexpr double kKnobPx     = 7.0;
constexpr double kGrabPx     = 12.0;
constexpr double kTrackMaxPx = 100.0;   ///< radius at the top of the track

double Clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

/// Knob height (0..1) for a radius, and back: quadratic, so the bottom of the
/// track -- the few-pixel softness that needs precision -- gets most of it.
double FractionOf(double px) { return std::sqrt(Clamp01(px / kTrackMaxPx)); }
double RadiusAt(double f)    { const double c = Clamp01(f); return c * c * kTrackMaxPx; }
} // namespace

void DefocusWidget::layout(const OverlayContext& c)
{
    const double h = c.sy(kTrackHPx);

    _track.x1 = c.rod.x1 + c.sx(kInsetPx);
    _track.x2 = _track.x1 + c.sx(kTrackWPx);
    _track.y1 = c.rod.y1 + (c.rodHeight() - h) * 0.5;
    _track.y2 = _track.y1 + h;
}

double DefocusWidget::value(const OverlayContext& c) const
{
    // The same snapshot everything else draws from, so the knob cannot
    // disagree with what will render. Pixels in both the engine and the
    // parameters, so no unit conversion here, unlike opacity.
    if (c.editBase) return c.anim.base.defocus;

    const Stage& s = c.anim.stages[c.activeStage];
    return c.editTo ? s.defocusTo : s.defocusFrom;
}

void DefocusWidget::write(const OverlayContext& c, double px) const
{
    // Rounded to a tenth of a pixel: nobody can see the difference finer than
    // that, and the Inspector then shows a number worth reading.
    const double v = std::round(std::max(0.0, px) * 10.0) / 10.0;

    if (c.editBase) { SetDouble(c.effect, kParamBaseDefocus, v); return; }

    SetDouble(c.effect,
              StageParam(c.editTo ? kParamDefocusTo : kParamDefocusFrom, c.activeStage), v);
}

double DefocusWidget::valueAt(const OverlayContext& c, double y) const
{
    const double h = _track.y2 - _track.y1;
    if (h < 1e-9) return value(c);
    return RadiusAt((y - _track.y1) / h);
}

void DefocusWidget::draw(const OverlayContext& c)
{
    layout(c);

    const double px    = value(c);
    const double knobY = _track.y1 + FractionOf(px) * (_track.y2 - _track.y1);

    // The target's own colour, as on the opacity slider, so it says which of
    // From / To / Base it is acting on without a label for it.
    const Colour tint = c.editBase ? colours::kStage[3]
                                   : (c.editTo ? colours::kGizmoTo : colours::kGizmo);

    SetColour(c, { 0.0f, 0.0f, 0.0f, 0.55f });
    FillRect(c, _track.x1, _track.y1, _track.x2, _track.y2);

    SetColour(c, tint);
    FillRect(c, _track.x1, _track.y1, _track.x2, knobY);

    SetColour(c, colours::kPanelEdge);
    SetLineWidth(c, 1.0f);
    StrokeRect(c, _track.x1, _track.y1, _track.x2, _track.y2);

    Handle(c, (_track.x1 + _track.x2) * 0.5, knobY, colours::kHandle, kKnobPx);

    char buf[32];
    std::snprintf(buf, sizeof(buf), px < 10.0 ? "%.1f px" : "%.0f px", px);

    HaloText(c, buf, _track.x1, _track.y2 + c.sy(6.0), colours::kText,
             kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBottom);

    HaloText(c, "BLUR", _track.x1, _track.y1 - c.sy(6.0), colours::kText,
             kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentTop);
}

bool DefocusWidget::penDown(const OverlayContext& c, const OfxPointD& p)
{
    const bool onTrack = p.x >= _track.x1 - c.sx(kGrabPx)
                      && p.x <= _track.x2 + c.sx(kGrabPx)
                      && p.y >= _track.y1 - c.sy(kGrabPx)
                      && p.y <= _track.y2 + c.sy(kGrabPx);
    if (!onTrack) return false;

    // Repeat-click returns to sharp, as the opacity slider returns to full:
    // each control resets exactly what it drives.
    if (_clicks.isDouble(c, p, 1))
    {
        write(c, 0.0);
        _drag = 0;
        return true;
    }

    _drag = 1;
    write(c, valueAt(c, p.y));
    return true;
}

bool DefocusWidget::penMotion(const OverlayContext& c, const OfxPointD& p)
{
    if (_drag == 0) return false;
    write(c, valueAt(c, p.y));
    return true;
}

bool DefocusWidget::penUp(const OverlayContext& /*c*/, const OfxPointD& /*p*/)
{
    if (_drag == 0) return false;
    _drag = 0;
    return true;
}

} // namespace mtx
