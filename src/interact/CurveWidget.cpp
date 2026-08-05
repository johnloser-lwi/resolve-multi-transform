#include "Widgets.h"

#include "AnimEngine.h"
#include "ParamNames.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mtx {

namespace {
constexpr double kPanelPx  = 168.0;
constexpr double kPadPx    = 10.0;
constexpr double kHeaderPx = 16.0;
constexpr int    kCurveSegments = 96;

double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace

void CurveWidget::fitRange(const OverlayContext& c)
{
    // Grow the plotted range to contain whatever the curve actually does. A
    // spring bounce can swing well past the default window, and a curve drawn
    // flat against the panel edge tells you nothing about its shape.
    const Easing e = c.anim.stages[c.activeStage].easing;

    double lo = 0.0, hi = 1.0;
    for (int i = 0; i <= kCurveSegments; ++i)
    {
        const double y = ApplyEasing(static_cast<float>(i) / kCurveSegments, e);
        if (y < lo) lo = y;
        if (y > hi) hi = y;
    }

    // Include the bezier handles, which are draggable and so must stay reachable.
    lo = std::min({ lo, static_cast<double>(e.y1), static_cast<double>(e.y2) });
    hi = std::max({ hi, static_cast<double>(e.y1), static_cast<double>(e.y2) });

    const double margin = 0.12 * std::max(1.0, hi - lo);
    _yMin = std::min(kYMinDefault, lo - margin);
    _yMax = std::max(kYMaxDefault, hi + margin);
}

void CurveWidget::layout(const OverlayContext& c)
{
    fitRange(c);

    const double size = c.sx(kPanelPx);

    // Top-right of the image, clear of the timeline strip along the bottom.
    _rect.x2 = c.rod.x2 - c.sx(20.0);
    _rect.x1 = _rect.x2 - size;
    _rect.y2 = c.rod.y2 - c.sy(20.0);
    _rect.y1 = _rect.y2 - (size + c.sy(kHeaderPx));

    _plot.x1 = _rect.x1 + c.sx(kPadPx);
    _plot.x2 = _rect.x2 - c.sx(kPadPx);
    _plot.y1 = _rect.y1 + c.sy(kPadPx);
    _plot.y2 = _rect.y2 - c.sy(kPadPx + kHeaderPx);
}

OfxPointD CurveWidget::unitToPanel(double ux, double uy) const
{
    OfxPointD p;
    p.x = _plot.x1 + ux * (_plot.x2 - _plot.x1);
    p.y = _plot.y1 + (uy - _yMin) / (_yMax - _yMin) * (_plot.y2 - _plot.y1);
    return p;
}

OfxPointD CurveWidget::panelToUnit(const OfxPointD& p) const
{
    const double w = (_plot.x2 - _plot.x1) > 1e-9 ? (_plot.x2 - _plot.x1) : 1.0;
    const double h = (_plot.y2 - _plot.y1) > 1e-9 ? (_plot.y2 - _plot.y1) : 1.0;
    OfxPointD u;
    u.x = (p.x - _plot.x1) / w;
    u.y = _yMin + (p.y - _plot.y1) / h * (_yMax - _yMin);
    return u;
}

void CurveWidget::draw(const OverlayContext& c)
{
    Panel(c, _rect);

    const Stage& s = c.anim.stages[c.activeStage];
    const Easing e = s.easing;

    SetColour(c, colours::kTextDim);
    Text(c, "EASING  stage " + std::to_string(c.activeStage + 1),
         _rect.x1 + c.sx(kPadPx), _rect.y2 - c.sy(5.0),
         kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentTop);

    // Reference frame: the 0 and 1 lines bound the useful range; anything drawn
    // outside them is anticipation or overshoot.
    const OfxPointD zero0 = unitToPanel(0.0, 0.0);
    const OfxPointD zero1 = unitToPanel(1.0, 0.0);
    const OfxPointD one0  = unitToPanel(0.0, 1.0);
    const OfxPointD one1  = unitToPanel(1.0, 1.0);

    SetColour(c, colours::kGrid);
    SetLineWidth(c, 1.0f);
    Line(c, zero0.x, zero0.y, zero1.x, zero1.y);
    Line(c, one0.x,  one0.y,  one1.x,  one1.y);
    StrokeRect(c, _plot.x1, _plot.y1, _plot.x2, _plot.y2);

    // A faint diagonal shows what linear would look like, which makes the
    // amount of easing legible at a glance.
    SetColour(c, { 0.35f, 0.38f, 0.44f, 0.55f });
    Line(c, zero0.x, zero0.y, one1.x, one1.y);

    // The curve itself, plotted through the same evaluator the renderer uses --
    // so what is drawn here is exactly what the animation does.
    OfxPointD pts[kCurveSegments + 1];
    for (int i = 0; i <= kCurveSegments; ++i)
    {
        const double u = static_cast<double>(i) / kCurveSegments;
        pts[i] = unitToPanel(u, ApplyEasing(static_cast<float>(u), e));
    }
    SetColour(c, colours::kAccent);
    SetLineWidth(c, 2.0f);
    Polyline(c, pts, kCurveSegments + 1);

    // Control handles, with tethers to the endpoints they belong to.
    const OfxPointD p0 = unitToPanel(0.0, 0.0);
    const OfxPointD p3 = unitToPanel(1.0, 1.0);
    const OfxPointD p1 = unitToPanel(e.x1, e.y1);
    const OfxPointD p2 = unitToPanel(e.x2, e.y2);

    SetColour(c, { 0.85f, 0.88f, 0.95f, 0.55f });
    SetLineWidth(c, 1.0f);
    Line(c, p0.x, p0.y, p1.x, p1.y);
    Line(c, p3.x, p3.y, p2.x, p2.y);

    Handle(c, p1.x, p1.y, _drag == kDragP1 ? colours::kPlayhead : colours::kHandle, 4.5);
    Handle(c, p2.x, p2.y, _drag == kDragP2 ? colours::kPlayhead : colours::kHandle, 4.5);

    // Playhead position along this stage's own progress, so you can see where
    // the current frame sits on the curve.
    const double prog = StageProgress(s, static_cast<float>(c.time));
    if (c.time >= std::min<double>(s.startFrame, s.endFrame) &&
        c.time <= std::max<double>(s.startFrame, s.endFrame))
    {
        const double raw = (s.endFrame - s.startFrame) > 1e-6
                         ? (c.time - s.startFrame) / (s.endFrame - s.startFrame) : 0.0;
        const OfxPointD ph = unitToPanel(Clamp(raw, 0.0, 1.0), prog);
        SetColour(c, colours::kPlayhead);
        Ellipse(c, ph.x, ph.y, c.sx(4.0), c.sy(4.0));
    }

    // Numeric read-out: the curve is for feel, the numbers for repeatability.
    char buf[96];
    snprintf(buf, sizeof(buf), "in %d   out %d",
             static_cast<int>(GetDouble(c.effect, StageParam(kParamEaseIn,  c.activeStage), c.time) + 0.5),
             static_cast<int>(GetDouble(c.effect, StageParam(kParamEaseOut, c.activeStage), c.time) + 0.5));
    SetColour(c, colours::kText);
    Text(c, buf, _rect.x2 - c.sx(kPadPx), _rect.y2 - c.sy(5.0),
         kOfxDrawTextAlignmentRight | kOfxDrawTextAlignmentTop);
}

void CurveWidget::writeHandle(const OverlayContext& c, int which, const OfxPointD& unit)
{
    const double ux = Clamp(unit.x, 0.0, 1.0);
    const double uy = Clamp(unit.y, _yMin, _yMax);

    // Horizontal movement is clamped to 0..1 because outside that range the
    // bezier folds back on itself and a single time maps to two values -- the
    // solver would have no well-defined answer. Every curve editor constrains
    // this, Resolve's included.
    //
    // Vertical movement is deliberately *not* one-sided. Dragging a handle past
    // the opposite rail is what produces the steep, snappy curves, and clamping
    // it to only anticipation-below / overshoot-above made half the curve space
    // unreachable.
    if (which == kDragP1)
    {
        SetDouble(c.effect, StageParam(kParamEaseIn, c.activeStage), ux * 100.0);
        SetDouble(c.effect, StageParam(kParamAnticipation, c.activeStage),
                  Clamp((-uy / 0.55) * 100.0, -200.0, 200.0));
    }
    else
    {
        // x2 is measured from the right-hand end, so ease-out grows as the
        // handle is pulled left.
        SetDouble(c.effect, StageParam(kParamEaseOut, c.activeStage), (1.0 - ux) * 100.0);
        SetDouble(c.effect, StageParam(kParamOvershoot, c.activeStage),
                  Clamp(((uy - 1.0) / 0.55) * 100.0, -200.0, 200.0));
    }
}

bool CurveWidget::penDown(const OverlayContext& c, const OfxPointD& p)
{
    if (!Contains(_rect, p)) return false;

    const Easing e = c.anim.stages[c.activeStage].easing;
    const OfxPointD p1 = unitToPanel(e.x1, e.y1);
    const OfxPointD p2 = unitToPanel(e.x2, e.y2);

    if      (NearPoint(c, p, p1.x, p1.y, 10.0)) _drag = kDragP1;
    else if (NearPoint(c, p, p2.x, p2.y, 10.0)) _drag = kDragP2;
    else                                        _drag = kNone;

    if (_drag != kNone) writeHandle(c, _drag, panelToUnit(p));
    return true;
}

bool CurveWidget::penMotion(const OverlayContext& c, const OfxPointD& p)
{
    if (_drag == kNone) return false;
    writeHandle(c, _drag, panelToUnit(p));
    return true;
}

bool CurveWidget::penUp(const OverlayContext& /*c*/, const OfxPointD& /*p*/)
{
    if (_drag == kNone) return false;
    _drag = kNone;
    return true;
}

} // namespace mtx
