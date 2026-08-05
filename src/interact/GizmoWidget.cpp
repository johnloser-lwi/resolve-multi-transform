#include "Widgets.h"

#include "AnimEngine.h"
#include "ParamNames.h"

#include <cmath>

namespace mtx {

namespace {
constexpr double kRotateArmPx = 34.0;

double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace

void GizmoWidget::layout(const OverlayContext& /*c*/) { }

GizmoWidget::Pose GizmoWidget::readPose(const OverlayContext& c) const
{
    const int i = c.activeStage;
    const Stage& s = c.anim.stages[i];

    Pose p;
    p.scale   = c.editTo ? s.scaleTo : s.scaleFrom;
    p.rot     = c.editTo ? s.rotTo   : s.rotFrom;
    p.posX    = c.editTo ? s.posXTo  : s.posXFrom;
    p.posY    = c.editTo ? s.posYTo  : s.posYFrom;
    p.anchorX = s.anchorX;
    p.anchorY = s.anchorY;
    return p;
}

void GizmoWidget::writePose(const OverlayContext& c, const Pose& p) const
{
    const int i = c.activeStage;

    SetDouble(c.effect, StageParam(c.editTo ? kParamScaleTo : kParamScaleFrom, i), p.scale);
    SetDouble(c.effect, StageParam(c.editTo ? kParamRotTo   : kParamRotFrom,   i), p.rot);
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
    return MakeTransform(static_cast<float>(p.anchorX * W),
                         static_cast<float>(p.anchorY * H),
                         static_cast<float>(p.scale), static_cast<float>(p.scale),
                         static_cast<float>(p.rot),
                         static_cast<float>(p.posX * W),
                         static_cast<float>(p.posY * H));
}

void GizmoWidget::draw(const OverlayContext& c)
{
    const Pose pose = readPose(c);
    const Mat3 m    = poseMatrix(c, pose);

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

    const Colour tint = c.editTo ? colours::kGizmoTo : colours::kGizmo;

    SetColour(c, { 0.0f, 0.0f, 0.0f, 0.55f });
    SetLineWidth(c, 3.0f);
    LineLoop(c, corner, 4);
    SetColour(c, tint);
    SetLineWidth(c, 1.5f);
    LineLoop(c, corner, 4);

    // Anchor, which is where scale and rotation pivot.
    float ax, ay;
    m.Apply(static_cast<float>(pose.anchorX * W), static_cast<float>(pose.anchorY * H), ax, ay);
    const double anchorX = c.rod.x1 + ax;
    const double anchorY = c.rod.y1 + ay;

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
    Text(c, std::string("Stage ") + std::to_string(c.activeStage + 1)
            + (c.editTo ? "  -  TO" : "  -  FROM"),
         anchorX, anchorY + c.sy(22.0),
         kOfxDrawTextAlignmentCenterH | kOfxDrawTextAlignmentBottom);
}

bool GizmoWidget::penDown(const OverlayContext& c, const OfxPointD& p)
{
    const Pose pose = readPose(c);
    const Mat3 m    = poseMatrix(c, pose);
    const double W  = c.rodWidth();
    const double H  = c.rodHeight();

    float ax, ay;
    m.Apply(static_cast<float>(pose.anchorX * W), static_cast<float>(pose.anchorY * H), ax, ay);
    _anchorScreen.x = c.rod.x1 + ax;
    _anchorScreen.y = c.rod.y1 + ay;

    _grabPose  = pose;
    _grabPoint = p;

    const double dx = p.x - _anchorScreen.x;
    const double dy = p.y - _anchorScreen.y;
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

    switch (_drag)
    {
        case kDragMove:
        {
            pose.posX = _grabPose.posX + (p.x - _grabPoint.x) / (W > 1e-9 ? W : 1.0);
            pose.posY = _grabPose.posY + (p.y - _grabPoint.y) / (H > 1e-9 ? H : 1.0);
            break;
        }
        case kDragScale:
        {
            // Scale by how much the cursor's distance from the anchor changed.
            const double dx = p.x - _anchorScreen.x;
            const double dy = p.y - _anchorScreen.y;
            const double r  = std::sqrt(dx * dx + dy * dy);
            pose.scale = Clamp(_grabPose.scale * (r / _grabRadius), 0.001, 100.0);
            break;
        }
        case kDragRotate:
        {
            const double dx = p.x - _anchorScreen.x;
            const double dy = p.y - _anchorScreen.y;
            const double a  = std::atan2(dy, dx);
            pose.rot = _grabPose.rot + (a - _grabAngle) * 180.0 / kPi;
            break;
        }
        case kDragAnchor:
        {
            // Put the anchor under the cursor, in source space. Moving the anchor
            // deliberately does not compensate the position, so the image shifts
            // -- matching how anchor points behave everywhere else.
            const Mat3 inv = Invert(poseMatrix(c, _grabPose));
            float sxp, syp;
            inv.Apply(static_cast<float>(p.x - c.rod.x1),
                      static_cast<float>(p.y - c.rod.y1), sxp, syp);
            pose.anchorX = Clamp(sxp / (W > 1e-9 ? W : 1.0), -2.0, 3.0);
            pose.anchorY = Clamp(syp / (H > 1e-9 ? H : 1.0), -2.0, 3.0);
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
