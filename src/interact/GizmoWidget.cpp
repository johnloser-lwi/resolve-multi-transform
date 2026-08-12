#include "Widgets.h"

#include "AnimEngine.h"
#include "ParamNames.h"

#include <cmath>

namespace mtx {

namespace {
constexpr double kRotateArmPx = 34.0;

double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

/// Nearest multiple of @p step. Used to snap rotation to tidy angles.
double SnapTo(double v, double step)
{
    return std::floor(v / step + 0.5) * step;
}

constexpr double kRotateSnapDegrees = 15.0;
} // namespace

void GizmoWidget::layout(const OverlayContext& /*c*/) { }

GizmoWidget::Pose GizmoWidget::readPose(const OverlayContext& c) const
{
    const int i = c.activeStage;
    const Stage& s = c.anim.stages[i];

    if (c.editBase)
    {
        const BasePose& b = c.anim.base;

        Pose bp;
        bp.scale     = b.scaleX;
        bp.scaleY    = b.scaleY;
        bp.rot       = b.rot;
        bp.posX      = b.posX;
        bp.posY      = b.posY;
        bp.anchorX   = b.anchorX;
        bp.anchorY   = b.anchorY;
        bp.linkScale = b.linkScale;
        bp.tiltX     = b.tiltX;
        bp.swivelY   = b.swivelY;
        return bp;
    }

    Pose p;
    p.scale   = c.editTo ? s.scaleTo  : s.scaleFrom;
    p.scaleY  = c.editTo ? s.scaleYTo : s.scaleYFrom;
    p.rot     = c.editTo ? s.rotTo    : s.rotFrom;
    p.posX    = c.editTo ? s.posXTo   : s.posXFrom;
    p.posY    = c.editTo ? s.posYTo   : s.posYFrom;
    p.anchorX = s.anchorX;
    p.anchorY = s.anchorY;
    p.linkScale = s.linkScale;
    p.tiltX     = c.editTo ? s.tiltXTo   : s.tiltXFrom;
    p.swivelY   = c.editTo ? s.swivelYTo : s.swivelYFrom;
    return p;
}

void GizmoWidget::writePose(const OverlayContext& c, const Pose& p) const
{
    if (c.editBase)
    {
        SetDouble  (c.effect, kParamBaseScale,  p.scale);
        SetDouble  (c.effect, kParamBaseRot,    p.rot);
        SetDouble2D(c.effect, kParamBasePos,    p.posX, p.posY);
        SetDouble2D(c.effect, kParamBaseAnchor, p.anchorX, p.anchorY);
        if (!p.linkScale) SetDouble(c.effect, kParamBaseScaleY, p.scaleY);
        return;
    }

    const int i = c.activeStage;

    SetDouble(c.effect, StageParam(c.editTo ? kParamScaleTo : kParamScaleFrom, i), p.scale);
    SetDouble(c.effect, StageParam(c.editTo ? kParamRotTo   : kParamRotFrom,   i), p.rot);

    // Only written while unlinked. Writing it under a link would leave a stale
    // value behind that jumped into effect the moment the link was released.
    if (!p.linkScale)
        SetDouble(c.effect, StageParam(c.editTo ? kParamScaleYTo : kParamScaleYFrom, i), p.scaleY);

    SetDouble2D(c.effect, StageParam(c.editTo ? kParamPosTo : kParamPosFrom,   i),
                p.posX, p.posY);
    // The anchor is shared by both ends: a pivot that moved between From and To
    // would make the motion impossible to reason about.
    SetDouble2D(c.effect, StageParam(kParamAnchor, i), p.anchorX, p.anchorY);
}

Mat3 GizmoWidget::poseMatrix(const OverlayContext& c, const Pose& p) const
{
    const double W = c.rodWidth();
    const double H = c.rodHeight();

    // Foreshortening from the orthographic tilt and swivel is folded in, so the
    // outline sits on the rendered image rather than beside it. It is a constant
    // factor during a drag, so every ratio below is unaffected by it.
    float orthoX = 1.0f, orthoY = 1.0f;
    OrthographicScale(static_cast<float>(p.tiltX), static_cast<float>(p.swivelY),
                      orthoX, orthoY);

    const double sx = p.scale;
    const double sy = p.linkScale ? p.scale : p.scaleY;

    return MakeTransform(static_cast<float>(p.anchorX * W),
                         static_cast<float>(p.anchorY * H),
                         static_cast<float>(sx) * orthoX,
                         static_cast<float>(sy) * orthoY,
                         static_cast<float>(p.rot),
                         static_cast<float>(p.posX * W),
                         static_cast<float>(p.posY * H));
}

double GizmoWidget::refTime(const OverlayContext& c) const
{
    // The base is deliberately the exception: it follows the playhead.
    //
    // A stage gizmo is pinned to that stage's own start or end frame because it
    // marks a fixed place in the move, and pinning is what stops it sliding
    // about while scrubbing. The base marks no place in any move -- it is a
    // static resting pose underneath all of them -- so the useful thing is a
    // handle that sits on the picture as it looks right now, whenever that is.
    if (c.editBase) return c.time;

    const Stage& s = c.anim.stages[c.activeStage];
    return ClipTimeFromStageFrame(c.anim, s, c.editTo ? s.endFrame : s.startFrame);
}

/** The transforms surrounding whatever this gizmo is posing. */
StageContext GizmoWidget::context(const OverlayContext& c) const
{
    const float t = static_cast<float>(refTime(c));
    const float w = static_cast<float>(c.rodWidth());
    const float h = static_cast<float>(c.rodHeight());

    // The base composes rightmost, so every stage is outside it and nothing is
    // inside -- which EvaluateStageContext cannot express, since it always folds
    // the base into `inner`.
    return c.editBase ? EvaluateBaseContext(c.anim, t, w, h)
                      : EvaluateStageContext(c.anim, c.activeStage, t, w, h);
}

Mat3 GizmoWidget::displayMatrix(const OverlayContext& c, const Pose& p) const
{
    const StageContext ctx = context(c);

    return ctx.outer * poseMatrix(c, p) * ctx.inner;
}

OfxPointD GizmoWidget::toStageSpace(const OverlayContext& c, const OfxPointD& p) const
{
    const StageContext ctx = context(c);

    float lx, ly;
    Invert(ctx.outer).Apply(static_cast<float>(p.x - c.rod.x1),
                            static_cast<float>(p.y - c.rod.y1), lx, ly);
    return { static_cast<double>(lx), static_cast<double>(ly) };
}

OfxPointD GizmoWidget::anchorScreen(const OverlayContext& c, const Pose& p) const
{
    const StageContext ctx = context(c);


    // The anchor is expressed in the space the stage's matrix consumes, so it
    // must not be pushed through `inner` -- only through the stage itself and
    // whatever comes after it.
    const Mat3 m = ctx.outer * poseMatrix(c, p);

    float ax, ay;
    m.Apply(static_cast<float>(p.anchorX * c.rodWidth()),
            static_cast<float>(p.anchorY * c.rodHeight()), ax, ay);
    return { c.rod.x1 + ax, c.rod.y1 + ay };
}

void GizmoWidget::draw(const OverlayContext& c)
{
    const Pose pose = readPose(c);
    const Mat3 m    = displayMatrix(c, pose);

    const double W = c.rodWidth();
    const double H = c.rodHeight();

    // Image outline under the pose.
    const double cxs[4] = { 0.0, W,   W,   0.0 };
    const double cys[4] = { 0.0, 0.0, H,   H   };
    OfxPointD corner[4];
    for (int i = 0; i < 4; ++i)
    {
        float ox, oy;
        m.Apply(static_cast<float>(cxs[i]), static_cast<float>(cys[i]), ox, oy);
        corner[i].x = c.rod.x1 + ox;
        corner[i].y = c.rod.y1 + oy;
    }

    // Violet for the base, matching its toolbar button, so it is obvious at a
    // glance that a drag is moving the resting pose and not a keyframe.
    const Colour tint = c.editBase ? colours::kStage[3]
                                   : (c.editTo ? colours::kGizmoTo : colours::kGizmo);

    SetColour(c, { 0.0f, 0.0f, 0.0f, 0.55f });
    SetLineWidth(c, 3.0f);
    LineLoop(c, corner, 4);
    SetColour(c, tint);
    SetLineWidth(c, 1.5f);
    LineLoop(c, corner, 4);

    // Anchor, which is where scale and rotation pivot.
    const OfxPointD anchorPt = anchorScreen(c, pose);
    const double anchorX = anchorPt.x;
    const double anchorY = anchorPt.y;

    SetColour(c, tint);
    SetLineWidth(c, 1.5f);
    Ellipse(c, anchorX, anchorY, c.sx(9.0), c.sy(9.0));
    Line(c, anchorX - c.sx(14.0), anchorY, anchorX + c.sx(14.0), anchorY);
    Line(c, anchorX, anchorY - c.sy(14.0), anchorX, anchorY + c.sy(14.0));

    // Corner handles scale about the anchor.
    for (int i = 0; i < 4; ++i)
        Handle(c, corner[i].x, corner[i].y, tint, 5.0);

    // Rotation arm, projected off the midpoint of the top edge.
    const double midX = (corner[2].x + corner[3].x) * 0.5;
    const double midY = (corner[2].y + corner[3].y) * 0.5;
    double dirX = midX - anchorX;
    double dirY = midY - anchorY;
    const double len = std::sqrt(dirX * dirX + dirY * dirY);
    if (len > 1e-6)
    {
        dirX /= len;
        dirY /= len;
        const double rx = midX + dirX * c.sx(kRotateArmPx);
        const double ry = midY + dirY * c.sy(kRotateArmPx);

        SetColour(c, tint);
        SetLineWidth(c, 1.5f);
        Line(c, midX, midY, rx, ry);
        Handle(c, rx, ry, colours::kHandle, 5.0);
    }

    // Label: which stage, and which end of it, is being posed.
    SetColour(c, tint);
    Text(c, c.editBase ? std::string("BASE TRANSFORM")
                       : std::string("Stage ") + std::to_string(c.activeStage + 1)
                         + (c.editTo ? "  -  TO" : "  -  FROM"),
         anchorX, anchorY + c.sy(22.0),
         kOfxDrawTextAlignmentCenterH | kOfxDrawTextAlignmentBottom);
}

bool GizmoWidget::penDown(const OverlayContext& c, const OfxPointD& p)
{
    const Pose pose = readPose(c);
    const Mat3 m    = displayMatrix(c, pose);
    const double W  = c.rodWidth();
    const double H  = c.rodHeight();

    _anchorScreen = anchorScreen(c, pose);

    _grabPose  = pose;

    // Drag state is recorded in the stage's own space. Measuring it on screen
    // would fold the other stages' scale and rotation into every delta, so a
    // drag on stage 2 would write numbers that mean something different by the
    // time stage 1 has finished with them.
    const Mat3 poseM = poseMatrix(c, pose);
    float lax, lay;
    poseM.Apply(static_cast<float>(pose.anchorX * W),
                static_cast<float>(pose.anchorY * H), lax, lay);
    _anchorLocal.x = lax;
    _anchorLocal.y = lay;

    _grabPoint = toStageSpace(c, p);

    const double dx = _grabPoint.x - _anchorLocal.x;
    const double dy = _grabPoint.y - _anchorLocal.y;
    _grabAngle  = std::atan2(dy, dx);
    _grabRadius = std::sqrt(dx * dx + dy * dy);
    if (_grabRadius < 1e-6) _grabRadius = 1e-6;

    // Corners
    const double cxs[4] = { 0.0, W,   W,   0.0 };
    const double cys[4] = { 0.0, 0.0, H,   H   };
    OfxPointD corner[4];
    for (int i = 0; i < 4; ++i)
    {
        float ox, oy;
        m.Apply(static_cast<float>(cxs[i]), static_cast<float>(cys[i]), ox, oy);
        corner[i].x = c.rod.x1 + ox;
        corner[i].y = c.rod.y1 + oy;
    }

    // Rotation handle first: it sits outside the box and would otherwise be
    // shadowed by the move region.
    const double midX = (corner[2].x + corner[3].x) * 0.5;
    const double midY = (corner[2].y + corner[3].y) * 0.5;
    double dirX = midX - _anchorScreen.x;
    double dirY = midY - _anchorScreen.y;
    const double len = std::sqrt(dirX * dirX + dirY * dirY);
    if (len > 1e-6)
    {
        dirX /= len; dirY /= len;
        const double rx = midX + dirX * c.sx(kRotateArmPx);
        const double ry = midY + dirY * c.sy(kRotateArmPx);
        if (NearPoint(c, p, rx, ry, 10.0)) { _drag = kDragRotate; return true; }
    }

    for (int i = 0; i < 4; ++i)
    {
        if (NearPoint(c, p, corner[i].x, corner[i].y, 10.0))
        {
            _drag = kDragScale;
            return true;
        }
    }

    if (NearPoint(c, p, _anchorScreen.x, _anchorScreen.y, 12.0))
    {
        _drag = kDragAnchor;
        return true;
    }

    // Inside the transformed box: move. Tested by inverse-mapping the cursor
    // into source space, which handles rotation correctly without a polygon test.
    const Mat3 inv = Invert(m);
    float sxp, syp;
    inv.Apply(static_cast<float>(p.x - c.rod.x1), static_cast<float>(p.y - c.rod.y1), sxp, syp);
    if (sxp >= 0.0f && sxp <= static_cast<float>(W) &&
        syp >= 0.0f && syp <= static_cast<float>(H))
    {
        _drag = kDragMove;
        return true;
    }

    _drag = kNone;
    return false;
}

bool GizmoWidget::penMotion(const OverlayContext& c, const OfxPointD& p)
{
    if (_drag == kNone) return false;

    const double W = c.rodWidth();
    const double H = c.rodHeight();
    Pose pose = _grabPose;

    // Everything below works in the stage's own space, so the numbers written
    // are the stage's own regardless of what the surrounding stages are doing.
    const OfxPointD q = toStageSpace(c, p);

    switch (_drag)
    {
        case kDragMove:
        {
            double dx = q.x - _grabPoint.x;
            double dy = q.y - _grabPoint.y;

            // Constrained in the stage's own axes, not the screen's, so a
            // rotated stage locks along its own edges -- which is the axis the
            // number being written actually means.
            if (c.shiftHeld) LockToAxis(dx, dy);

            pose.posX = _grabPose.posX + dx / (W > 1e-9 ? W : 1.0);
            pose.posY = _grabPose.posY + dy / (H > 1e-9 ? H : 1.0);
            break;
        }
        case kDragScale:
        {
            const double dx = q.x - _anchorLocal.x;
            const double dy = q.y - _anchorLocal.y;

            if (_grabPose.linkScale)
            {
                // Scale by how much the cursor's distance from the anchor changed.
                const double r = std::sqrt(dx * dx + dy * dy);
                pose.scale = Clamp(_grabPose.scale * (r / _grabRadius), 0.001, 100.0);
            }
            else
            {
                // Unlinked, each axis follows its own component of the drag, so a
                // corner can stretch the box rather than only resize it. Both the
                // grab and the current offset are rotated back into the pose's own
                // axes first: on a rotated stage the screen's X is not the image's
                // X, and using it raw would swap the axes as the box turned.
                const double a  = -_grabPose.rot * kPi / 180.0;
                const double ca = std::cos(a);
                const double sa = std::sin(a);

                const double g0x = _grabPoint.x - _anchorLocal.x;
                const double g0y = _grabPoint.y - _anchorLocal.y;
                const double gx = g0x * ca - g0y * sa;
                const double gy = g0x * sa + g0y * ca;
                const double cx = dx * ca - dy * sa;
                const double cy = dx * sa + dy * ca;

                // With Shift, only the axis the drag favours changes, so one
                // side can be stretched without nudging the other.
                bool doX = true, doY = true;
                if (c.shiftHeld)
                {
                    const double mx = std::fabs(cx - gx);
                    const double my = std::fabs(cy - gy);
                    if (mx >= my) doY = false; else doX = false;
                }

                // A near-zero grab offset gives no usable ratio -- leave that axis
                // alone rather than dividing by it and flinging the scale away.
                if (doX && std::fabs(gx) > 1e-6)
                    pose.scale  = Clamp(_grabPose.scale  * (cx / gx), 0.001, 100.0);
                if (doY && std::fabs(gy) > 1e-6)
                    pose.scaleY = Clamp(_grabPose.scaleY * (cy / gy), 0.001, 100.0);
            }
            break;
        }
        case kDragRotate:
        {
            const double dx = q.x - _anchorLocal.x;
            const double dy = q.y - _anchorLocal.y;
            const double a  = std::atan2(dy, dx);
            pose.rot = _grabPose.rot + (a - _grabAngle) * 180.0 / kPi;

            // Rotation has no axes to lock, so Shift snaps to tidy angles
            // instead -- the same gesture, the same intent, and 45 or 90 degrees
            // by hand is otherwise guesswork.
            if (c.shiftHeld) pose.rot = SnapTo(pose.rot, kRotateSnapDegrees);
            break;
        }
        case kDragAnchor:
        {
            // Put the anchor under the cursor, in source space. Moving the anchor
            // deliberately does not compensate the position, so the image shifts
            // -- matching how anchor points behave everywhere else.
            // The cursor is already in the stage's space, so only the stage's own
            // matrix has to be undone to reach the space the anchor is written in.
            const Mat3 inv = Invert(poseMatrix(c, _grabPose));
            float sxp, syp;
            inv.Apply(static_cast<float>(q.x), static_cast<float>(q.y), sxp, syp);

            double ax = sxp / (W > 1e-9 ? W : 1.0);
            double ay = syp / (H > 1e-9 ? H : 1.0);

            if (c.shiftHeld)
            {
                // Locked relative to where the anchor started, so Shift slides
                // it along one axis rather than snapping it to the grab point.
                double dx = ax - _grabPose.anchorX;
                double dy = ay - _grabPose.anchorY;
                LockToAxis(dx, dy);
                ax = _grabPose.anchorX + dx;
                ay = _grabPose.anchorY + dy;
            }

            pose.anchorX = Clamp(ax, -2.0, 3.0);
            pose.anchorY = Clamp(ay, -2.0, 3.0);
            break;
        }
        default: return false;
    }

    writePose(c, pose);
    return true;
}

bool GizmoWidget::penUp(const OverlayContext& /*c*/, const OfxPointD& /*p*/)
{
    if (_drag == kNone) return false;
    _drag = kNone;
    return true;
}

} // namespace mtx
