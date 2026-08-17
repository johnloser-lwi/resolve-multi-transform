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

namespace {

/** The stages composed to the left of the active one, at clip time @p t. This
 *  is what carries a point in the stage's own space out to where it appears. */
Mat3 OuterAt(const OverlayContext& c, double t)
{
    return EvaluateStageContext(c.anim, c.activeStage, static_cast<float>(t),
                                static_cast<float>(c.rodWidth()),
                                static_cast<float>(c.rodHeight())).outer;
}

} // namespace

double PathWidget::endpointTime(const OverlayContext& c, bool end) const
{
    // Plus the position channel's own timing offset: with a staggered position
    // the image genuinely starts and finishes travelling at the shifted frames,
    // and the pinned endpoints have to sit where the image really is at those
    // moments -- pinning to the stage's un-shifted frames would draw the route's
    // ends where the image never was.
    const Stage& s = c.anim.stages[c.activeStage];
    return ClipTimeFromStageFrame(c.anim, s,
                                  (end ? s.endFrame : s.startFrame) + s.offsetPos);
}

OfxPointD PathWidget::toScreen(const OverlayContext& c, float nx, float ny, double t) const
{
    // Position is a normalised offset applied to the anchor, so the point that
    // actually travels the path is the anchor displaced by that offset. (Scale
    // and rotation pivot about the anchor and so leave it where it is; only the
    // translation moves it.)
    const Stage& s = c.anim.stages[c.activeStage];

    // Carried out through the preceding stages, or the route would be drawn
    // where this stage alone would put it rather than where the image goes.
    float sx, sy;
    OuterAt(c, t).Apply(
        static_cast<float>((static_cast<double>(s.anchorX) + nx) * c.rodWidth()),
        static_cast<float>((static_cast<double>(s.anchorY) + ny) * c.rodHeight()),
        sx, sy);

    OfxPointD p;
    p.x = c.rod.x1 + sx;
    p.y = c.rod.y1 + sy;
    return p;
}

void PathWidget::toNormalised(const OverlayContext& c, const OfxPointD& p, double t,
                              double& nx, double& ny) const
{
    const Stage& s = c.anim.stages[c.activeStage];
    const double w = c.rodWidth()  > 1e-9 ? c.rodWidth()  : 1.0;
    const double h = c.rodHeight() > 1e-9 ? c.rodHeight() : 1.0;

    // The exact inverse of toScreen at the same @p t, so a dragged handle lands
    // under the cursor.
    float lx, ly;
    Invert(OuterAt(c, t)).Apply(static_cast<float>(p.x - c.rod.x1),
                                static_cast<float>(p.y - c.rod.y1), lx, ly);

    nx = static_cast<double>(lx) / w - static_cast<double>(s.anchorX);
    ny = static_cast<double>(ly) / h - static_cast<double>(s.anchorY);
}

void PathWidget::draw(const OverlayContext& c)
{
    const Stage& s = c.anim.stages[c.activeStage];
    if (!s.enabled) return;

    float c1x, c1y, c2x, c2y;
    PathControlPoints(s, c1x, c1y, c2x, c2y);

    // Each end of the path, and the tangent handle belonging to it, is pinned to
    // that end's own frame. So the start of the route is drawn where the image
    // genuinely begins the move and the end where it genuinely finishes, and
    // both stay put while the playhead travels.
    const double tA = endpointTime(c, false);
    const double tB = endpointTime(c, true);

    const OfxPointD p0 = toScreen(c, s.posXFrom, s.posYFrom, tA);
    const OfxPointD p3 = toScreen(c, s.posXTo,   s.posYTo,   tB);
    const OfxPointD h1 = toScreen(c, c1x, c1y, tA);
    const OfxPointD h2 = toScreen(c, c2x, c2y, tB);

    // A path with no extent and no bend has nothing to show, and drawing a
    // cluster of overlapping handles on a stationary stage is just clutter.
    const double dx = p3.x - p0.x, dy = p3.y - p0.y;
    const bool straightAndStill =
        (dx * dx + dy * dy) < c.sx(2.0) * c.sx(2.0) &&
        s.pathC1X == 0.0f && s.pathC1Y == 0.0f &&
        s.pathC2X == 0.0f && s.pathC2Y == 0.0f;
    if (straightAndStill) return;

    // The trajectory, swept over the stage's own span of time rather than over
    // the bezier's parameter. Those differ once other stages are also moving
    // during this window: the image's real route then bends with them, and the
    // route is what this is meant to show. Sweeping time also picks up easing
    // that leaves 0..1 -- an overshoot really does carry the image past its end
    // point and back, and now that shows.
    OfxPointD pts[kPathSegments + 1];
    for (int i = 0; i <= kPathSegments; ++i)
    {
        const double f = static_cast<double>(i) / kPathSegments;
        const double t = tA + (tB - tA) * f;

        // StageProgress already applies the easing, so this is the eased value
        // the renderer would use at that frame.
        float px, py;
        EvaluatePath(s, StageProgress(s, StageLocalTime(c.anim, s, static_cast<float>(t))
                                        - s.offsetPos),
                     px, py);
        pts[i] = toScreen(c, px, py, t);
    }

    HaloPolyline(c, pts, kPathSegments + 1, colours::kStage[c.activeStage % 4], 1.5);

    // Tethers from each end to its handle, so it is obvious which is which.
    HaloLine(c, p0.x, p0.y, h1.x, h1.y, { 0.85f, 0.88f, 0.95f, 0.6f }, 1.0);
    HaloLine(c, p3.x, p3.y, h2.x, h2.y, { 0.85f, 0.88f, 0.95f, 0.6f }, 1.0);

    // Ticks along the path show where the easing spends its time: bunched ticks
    // mean slow, spread ticks mean fast. Without them a curved path gives no
    // clue about pacing.
    for (int i = 1; i < 10; ++i)
    {
        const double tt = tA + (tB - tA) * (static_cast<double>(i) / 10.0);
        float px, py;
        EvaluatePath(s, StageProgress(s, StageLocalTime(c.anim, s, static_cast<float>(tt))
                                        - s.offsetPos),
                     px, py);
        const OfxPointD d = toScreen(c, px, py, tt);
        HaloDot(c, d.x, d.y, c.sx(2.0), c.sy(2.0), { 1.0f, 1.0f, 1.0f, 0.55f });
    }

    // Endpoints, then handles on top.
    HaloDot(c, p0.x, p0.y, c.sx(5.0), c.sy(5.0), colours::kGizmo);
    HaloDot(c, p3.x, p3.y, c.sx(5.0), c.sy(5.0), colours::kGizmoTo);

    Handle(c, h1.x, h1.y, _drag == kDragC1 ? colours::kPlayhead : colours::kHandle, 4.5);
    Handle(c, h2.x, h2.y, _drag == kDragC2 ? colours::kPlayhead : colours::kHandle, 4.5);
}

bool PathWidget::penDown(const OverlayContext& c, const OfxPointD& p)
{
    const Stage& s = c.anim.stages[c.activeStage];
    if (!s.enabled) return false;

    float c1x, c1y, c2x, c2y;
    PathControlPoints(s, c1x, c1y, c2x, c2y);

    const OfxPointD h1 = toScreen(c, c1x, c1y, endpointTime(c, false));
    const OfxPointD h2 = toScreen(c, c2x, c2y, endpointTime(c, true));

    if      (NearPoint(c, p, h1.x, h1.y, kGrabPx)) _drag = kDragC1;
    else if (NearPoint(c, p, h2.x, h2.y, kGrabPx)) _drag = kDragC2;
    else                                           return false;   // fall through to the gizmo

    // Repeat-click straightens that half of the route: a zero offset is exactly
    // a straight line, not merely a nearly straight one -- see PathControlPoints.
    if (_clicks.isDouble(c, p, _drag))
    {
        SetDouble2D(c.effect,
                    StageParam(_drag == kDragC1 ? kParamPathC1 : kParamPathC2, c.activeStage),
                    0.0, 0.0);
        _drag = kNone;
        return true;
    }

    // The handle's own starting place, for Shift to constrain against.
    _grabNx = (_drag == kDragC1) ? c1x : c2x;
    _grabNy = (_drag == kDragC1) ? c1y : c2y;

    return true;
}

bool PathWidget::penMotion(const OverlayContext& c, const OfxPointD& p)
{
    if (_drag == kNone) return false;

    const Stage& s = c.anim.stages[c.activeStage];

    // Inverted at the same frame the handle was drawn at, or it would jump away
    // from the cursor the moment another stage is doing anything.
    double nx = 0.0, ny = 0.0;
    toNormalised(c, p, endpointTime(c, _drag == kDragC2), nx, ny);

    if (c.shiftHeld)
    {
        double dx = nx - _grabNx;
        double dy = ny - _grabNy;
        LockToAxis(dx, dy);
        nx = _grabNx + dx;
        ny = _grabNy + dy;
    }

    // Stored as an offset from the straight-line position, so the handle stays
    // put relative to the path when From or To is moved afterwards.
    const double dx = static_cast<double>(s.posXTo - s.posXFrom);
    const double dy = static_cast<double>(s.posYTo - s.posYFrom);

    // The offset this drag produces, in the same units both handles store.
    const double offX = (_drag == kDragC1)
                      ? nx - (static_cast<double>(s.posXFrom) + dx / 3.0)
                      : nx - (static_cast<double>(s.posXFrom) + dx * 2.0 / 3.0);
    const double offY = (_drag == kDragC1)
                      ? ny - (static_cast<double>(s.posYFrom) + dy / 3.0)
                      : ny - (static_cast<double>(s.posYFrom) + dy * 2.0 / 3.0);

    // Control bends both halves of the route together.
    //
    // Both handles are stored as offsets from their own point along the
    // straight line -- one third and two thirds of the way -- so giving them
    // the *same* offset bows the path symmetrically rather than producing an S.
    // That is what "together" means for a trajectory, and it is why this is a
    // plain copy rather than a mirror.
    if (c.ctrlHeld)
    {
        SetDouble2D(c.effect, StageParam(kParamPathC1, c.activeStage), offX, offY);
        SetDouble2D(c.effect, StageParam(kParamPathC2, c.activeStage), offX, offY);
        return true;
    }

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
