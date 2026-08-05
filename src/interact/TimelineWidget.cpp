#include "Widgets.h"

#include "AnimEngine.h"
#include "ParamNames.h"

#include <algorithm>
#include <cmath>

namespace mtx {

namespace {
constexpr double kLaneHeightPx  = 16.0;
constexpr double kLaneGapPx     = 4.0;
constexpr double kHeaderPx      = 18.0;
constexpr double kPadPx         = 8.0;
constexpr double kEdgeGrabPx    = 7.0;
} // namespace

void TimelineWidget::layout(const OverlayContext& c)
{
    const double lanes  = static_cast<double>(c.stageCount);
    const double height = c.sy(kHeaderPx + kPadPx * 2.0
                               + lanes * kLaneHeightPx + (lanes - 1.0) * kLaneGapPx);

    // Centred along the bottom of the image, wide enough to be usable but not
    // so wide it swamps the picture.
    double width = c.rodWidth() * 0.66;
    const double minWidth = c.sx(360.0);
    if (width < minWidth) width = std::min(minWidth, c.rodWidth() * 0.95);

    _rect.x1 = c.rod.x1 + (c.rodWidth() - width) * 0.5;
    _rect.x2 = _rect.x1 + width;
    _rect.y1 = c.rod.y1 + c.sy(20.0);
    _rect.y2 = _rect.y1 + height;

    // Visible frame range: every enabled stage, plus the playhead, plus a
    // margin so the bars never sit flush against the panel edge.
    double t0 = 0.0, t1 = 0.0;
    bool any = false;
    for (int i = 0; i < c.stageCount; ++i)
    {
        const Stage& s = c.anim.stages[i];
        if (!s.enabled) continue;
        const double lo = std::min<double>(s.startFrame, s.endFrame);
        const double hi = std::max<double>(s.startFrame, s.endFrame);
        if (!any) { t0 = lo; t1 = hi; any = true; }
        else      { t0 = std::min(t0, lo); t1 = std::max(t1, hi); }
    }
    if (!any) { t0 = c.time - 12.0; t1 = c.time + 12.0; }

    t0 = std::min(t0, c.time);
    t1 = std::max(t1, c.time);

    double span = t1 - t0;
    if (span < 1.0) span = 1.0;
    _t0 = t0 - span * 0.12;
    _t1 = t1 + span * 0.12;

    if (_drag != kNone) { _t0 = _dragT0; _t1 = _dragT1; }
}

double TimelineWidget::frameToX(double frame) const
{
    const double span = (_t1 - _t0) > 1e-9 ? (_t1 - _t0) : 1.0;
    return _rect.x1 + (frame - _t0) / span * (_rect.x2 - _rect.x1);
}

double TimelineWidget::xToFrame(double x) const
{
    const double w = (_rect.x2 - _rect.x1) > 1e-9 ? (_rect.x2 - _rect.x1) : 1.0;
    return _t0 + (x - _rect.x1) / w * (_t1 - _t0);
}

OfxRectD TimelineWidget::laneRect(const OverlayContext& c, int stage) const
{
    const double laneH = c.sy(kLaneHeightPx);
    const double gap   = c.sy(kLaneGapPx);
    const double top   = _rect.y2 - c.sy(kHeaderPx + kPadPx);

    OfxRectD r;
    r.y2 = top - static_cast<double>(stage) * (laneH + gap);
    r.y1 = r.y2 - laneH;
    r.x1 = _rect.x1 + c.sx(kPadPx);
    r.x2 = _rect.x2 - c.sx(kPadPx);
    return r;
}

void TimelineWidget::draw(const OverlayContext& c)
{
    Panel(c, _rect);

    // Header
    SetColour(c, colours::kTextDim);
    Text(c, "STAGE TIMING", _rect.x1 + c.sx(kPadPx), _rect.y2 - c.sy(6.0),
         kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentTop);

    SetColour(c, colours::kText);
    Text(c, "frame " + FrameLabel(c.time), _rect.x2 - c.sx(kPadPx), _rect.y2 - c.sy(6.0),
         kOfxDrawTextAlignmentRight | kOfxDrawTextAlignmentTop);

    for (int i = 0; i < c.stageCount; ++i)
    {
        const OfxRectD lane = laneRect(c, i);
        const Stage&   s    = c.anim.stages[i];

        // Lane background
        SetColour(c, colours::kGrid);
        SetLineWidth(c, 1.0f);
        Line(c, lane.x1, lane.y1 - c.sy(1.0), lane.x2, lane.y1 - c.sy(1.0));

        const bool active = (i == c.activeStage);

        // Stage number, brighter for the active stage.
        SetColour(c, active ? colours::kAccent : colours::kTextDim);
        Text(c, std::to_string(i + 1), _rect.x1 + c.sx(kPadPx * 0.4),
             (lane.y1 + lane.y2) * 0.5,
             kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentCenterV);

        if (!s.enabled)
        {
            SetColour(c, colours::kTextDim);
            Text(c, "off", lane.x1 + c.sx(4.0), (lane.y1 + lane.y2) * 0.5,
                 kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentCenterV);
            continue;
        }

        double bx1 = frameToX(std::min<double>(s.startFrame, s.endFrame));
        double bx2 = frameToX(std::max<double>(s.startFrame, s.endFrame));

        // A zero-length stage would otherwise be invisible and impossible to grab.
        if (bx2 - bx1 < c.sx(3.0)) bx2 = bx1 + c.sx(3.0);

        const Colour lc = colours::kStage[i % 4];
        SetColour(c, lc);
        FillRect(c, bx1, lane.y1, bx2, lane.y2);

        if (active)
        {
            SetColour(c, colours::kHandle);
            SetLineWidth(c, 2.0f);
            StrokeRect(c, bx1, lane.y1, bx2, lane.y2);
        }

        // Grab affordances at each end.
        SetColour(c, colours::kHandle);
        FillRect(c, bx1, lane.y1, bx1 + c.sx(2.0), lane.y2);
        FillRect(c, bx2 - c.sx(2.0), lane.y1, bx2, lane.y2);

        // Frame numbers, placed outside the bar so a short bar stays readable.
        SetColour(c, colours::kText);
        Text(c, FrameLabel(s.startFrame), bx1 - c.sx(4.0), (lane.y1 + lane.y2) * 0.5,
             kOfxDrawTextAlignmentRight | kOfxDrawTextAlignmentCenterV);
        Text(c, FrameLabel(s.endFrame), bx2 + c.sx(4.0), (lane.y1 + lane.y2) * 0.5,
             kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentCenterV);
    }

    // Playhead last, so it reads on top of the lanes.
    const double px = frameToX(c.time);
    if (px >= _rect.x1 && px <= _rect.x2)
    {
        SetColour(c, colours::kPlayhead);
        SetLineWidth(c, 1.5f);
        Line(c, px, _rect.y1 + c.sy(3.0), px, _rect.y2 - c.sy(kHeaderPx));
    }
}

bool TimelineWidget::penDown(const OverlayContext& c, const OfxPointD& p)
{
    if (!Contains(_rect, p)) return false;

    for (int i = 0; i < c.stageCount; ++i)
    {
        const OfxRectD lane = laneRect(c, i);
        if (p.y < lane.y1 || p.y > lane.y2) continue;

        const Stage& s = c.anim.stages[i];
        if (!s.enabled) return true;   // swallow, but nothing to drag

        const double bx1 = frameToX(s.startFrame);
        const double bx2 = frameToX(s.endFrame);
        const double tol = c.sx(kEdgeGrabPx);

        _dragStage = i;
        _grabFrame = xToFrame(p.x);
        _grabStart = s.startFrame;
        _grabEnd   = s.endFrame;
        _dragT0    = _t0;
        _dragT1    = _t1;

        if      (std::fabs(p.x - bx1) <= tol) _drag = kDragStart;
        else if (std::fabs(p.x - bx2) <= tol) _drag = kDragEnd;
        else if (p.x > bx1 && p.x < bx2)      _drag = kDragWhole;
        else                                  _drag = kNone;

        // Clicking a lane also selects that stage, so the gizmo and curve
        // editor follow along without a separate selection step.
        if (_drag != kNone && i != c.activeStage)
            c.effect->fetchChoiceParam(kParamActiveStage)->setValue(i);

        return true;
    }

    return true;   // clicks inside the panel never fall through to the image
}

bool TimelineWidget::penMotion(const OverlayContext& c, const OfxPointD& p)
{
    if (_drag == kNone) return false;

    const double frame  = xToFrame(p.x);
    const double delta  = frame - _grabFrame;
    const std::string sName = StageParam(kParamStartFrame, _dragStage);
    const std::string eName = StageParam(kParamEndFrame,   _dragStage);

    // Snap to whole frames: sub-frame start/end values are meaningless to an
    // editor and make the numbers look broken.
    const auto snap = [](double v) { return std::floor(v + 0.5); };

    if (_drag == kDragStart)
    {
        // Never let start cross end; that would invert the stage.
        const double v = std::min(snap(_grabStart + delta), _grabEnd);
        SetDouble(c.effect, sName, v);
    }
    else if (_drag == kDragEnd)
    {
        const double v = std::max(snap(_grabEnd + delta), _grabStart);
        SetDouble(c.effect, eName, v);
    }
    else if (_drag == kDragWhole)
    {
        const double shift = snap(delta);
        SetDouble(c.effect, sName, _grabStart + shift);
        SetDouble(c.effect, eName, _grabEnd   + shift);
    }
    return true;
}

bool TimelineWidget::penUp(const OverlayContext& /*c*/, const OfxPointD& /*p*/)
{
    if (_drag == kNone) return false;
    _drag = kNone;
    return true;
}

} // namespace mtx
