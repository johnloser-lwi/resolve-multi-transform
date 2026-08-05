#include "Widgets.h"

#include "AnimEngine.h"
#include "ParamNames.h"

#include <cmath>

namespace mtx {

namespace {
constexpr int    kPathSegments = 64;
constexpr double kGrabPx       = 10.0;
} // namespace

void PathWidget::layout(const OverlayContext& /*c*/) { }

OfxPointD PathWidget::toScreen(const OverlayContext& c, float nx, float ny) const
{
    // Position is a normalised offset applied to the anchor, so the point that
    // actually travels the path is the anchor displaced by that offset.
    const Stage& s = c.anim.stages[c.activeStage];
    OfxPointD p;
    p.x = c.rod.x1 + (static_cast<double>(s.anchorX) + nx) * c.rodWidth();
    p.y = c.rod.y1 + (static_cast<double>(s.anchorY) + ny) * c.rodHeight();
    return p;
}

void PathWidget::toNormalised(const OverlayContext& c, const OfxPointD& p,
                              double& nx, double& ny) const
{
    const Stage& s = c.anim.stages[c.activeStage];
    const double w = c.rodWidth()  > 1e-9 ? c.rodWidth()  : 1.0;
    const double h = c.rodHeight() > 1e-9 ? c.rodHeight() : 1.0;

    nx = (p.x - c.rod.x1) / w - static_cast<double>(s.anchorX);
    ny = (p.y - c.rod.y1) / h - static_cast<double>(s.anchorY);
}

void PathWidget::draw(const OverlayContext& c)
{
    const Stage& s = c.anim.stages[c.activeStage];
    if (!s.enabled) return;

    float c1x, c1y, c2x, c2y;
    PathControlPoints(s, c1x, c1y, c2x, c2y);

    const OfxPointD p0 = toScreen(c, s.posXFrom, s.posYFrom);
    const OfxPointD p3 = toScreen(c, s.posXTo,   s.posYTo);
    const OfxPointD h1 = toScreen(c, c1x, c1y);
    const OfxPointD h2 = toScreen(c, c2x, c2y);

    // A path with no extent and no bend has nothing to show, and drawing a
    // cluster of overlapping handles on a stationary stage is just clutter.
    const double dx = p3.x - p0.x, dy = p3.y - p0.y;
    const bool straightAndStill =
        (dx * dx + dy * dy) < c.sx(2.0) * c.sx(2.0) &&
        s.pathC1X == 0.0f && s.pathC1Y == 0.0f &&
        s.pathC2X == 0.0f && s.pathC2Y == 0.0f;
    if (straightAndStill) return;

    // The trajectory, sampled through the same evaluator the renderer uses.
    OfxPointD pts[kPathSegments + 1];
    for (int i = 0; i <= kPathSegments; ++i)
    {
        float px, py;
        EvaluatePath(s, static_cast<float>(i) / kPathSegments, px, py);
        pts[i] = toScreen(c, px, py);
    }

    SetColour(c, { 0.0f, 0.0f, 0.0f, 0.55f });
    SetLineWidth(c, 3.0f);
    Polyline(c, pts, kPathSegments + 1);

    SetColour(c, colours::kStage[c.activeStage % 4]);
    SetLineWidth(c, 1.5f);
    Polyline(c, pts, kPathSegments + 1);

    // Tethers from each end to its handle, so it is obvious which is which.
    SetColour(c, { 0.85f, 0.88f, 0.95f, 0.5f });
    SetLineWidth(c, 1.0f);
    Line(c, p0.x, p0.y, h1.x, h1.y);
    Line(c, p3.x, p3.y, h2.x, h2.y);

    // Ticks along the path show where the easing spends its time: bunched ticks
    // mean slow, spread ticks mean fast. Without them a curved path gives no
    // clue about pacing.
    SetColour(c, { 1.0f, 1.0f, 1.0f, 0.45f });
    for (int i = 1; i < 10; ++i)
    {
        const float e = ApplyEasing(static_cast<float>(i) / 10.0f, s.easing);
        float px, py;
        EvaluatePath(s, e, px, py);
        const OfxPointD t = toScreen(c, px, py);
        Ellipse(c, t.x, t.y, c.sx(2.0), c.sy(2.0));
    }

    // Endpoints, then handles on top.
    SetColour(c, colours::kGizmo);
    Ellipse(c, p0.x, p0.y, c.sx(5.0), c.sy(5.0));
    SetColour(c, colours::kGizmoTo);
    Ellipse(c, p3.x, p3.y, c.sx(5.0), c.sy(5.0));

    Handle(c, h1.x, h1.y, _drag == kDragC1 ? colours::kPlayhead : colours::kHandle, 4.5);
    Handle(c, h2.x, h2.y, _drag == kDragC2 ? colours::kPlayhead : colours::kHandle, 4.5);
}

bool PathWidget::penDown(const OverlayContext& c, const OfxPointD& p)
{
    const Stage& s = c.anim.stages[c.activeStage];
    if (!s.enabled) return false;

    float c1x, c1y, c2x, c2y;
    PathControlPoints(s, c1x, c1y, c2x, c2y);

    const OfxPointD h1 = toScreen(c, c1x, c1y);
    const OfxPointD h2 = toScreen(c, c2x, c2y);

    if      (NearPoint(c, p, h1.x, h1.y, kGrabPx)) _drag = kDragC1;
    else if (NearPoint(c, p, h2.x, h2.y, kGrabPx)) _drag = kDragC2;
    else                                           return false;   // fall through to the gizmo

    return true;
}

bool PathWidget::penMotion(const OverlayContext& c, const OfxPointD& p)
{
    if (_drag == kNone) return false;

    const Stage& s = c.anim.stages[c.activeStage];

    double nx = 0.0, ny = 0.0;
    toNormalised(c, p, nx, ny);

    // Stored as an offset from the straight-line position, so the handle stays
    // put relative to the path when From or To is moved afterwards.
    const double dx = static_cast<double>(s.posXTo - s.posXFrom);
    const double dy = static_cast<double>(s.posYTo - s.posYFrom);

    if (_drag == kDragC1)
    {
        SetDouble2D(c.effect, StageParam(kParamPathC1, c.activeStage),
                    nx - (static_cast<double>(s.posXFrom) + dx / 3.0),
                    ny - (static_cast<double>(s.posYFrom) + dy / 3.0));
    }
    else
    {
        SetDouble2D(c.effect, StageParam(kParamPathC2, c.activeStage),
                    nx - (static_cast<double>(s.posXFrom) + dx * 2.0 / 3.0),
                    ny - (static_cast<double>(s.posYFrom) + dy * 2.0 / 3.0));
    }
    return true;
}

bool PathWidget::penUp(const OverlayContext& /*c*/, const OfxPointD& /*p*/)
{
    if (_drag == kNone) return false;
    _drag = kNone;
    return true;
}

} // namespace mtx
