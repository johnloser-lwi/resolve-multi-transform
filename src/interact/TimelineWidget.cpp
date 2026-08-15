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
constexpr double kSyncWPx       = 78.0;   // fits "SYNC TIME" without trimming
constexpr double kSyncHPx       = 14.0;
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

    // The clip is the frame of reference: the ruler runs 0..clip length, so a
    // bar's position reads directly as "this far into the clip" rather than as
    // an absolute timeline frame that stops meaning anything once the clip is
    // moved.
    double t0 = 0.0, t1 = 0.0;

    if (c.clipLength > 0.0)
    {
        t0 = 0.0;
        t1 = c.clipLength;
    }
    else
    {
        // No clip extent from the host: fall back to framing the stages.
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
    }

    // A stage dragged outside the clip still has to stay visible and grabbable,
    // otherwise it becomes impossible to drag back. Stage frames are relative to
    // whatever that stage is anchored to, so they are shifted into clip time
    // before being compared against the ruler.
    for (int i = 0; i < c.stageCount; ++i)
    {
        const Stage& s = c.anim.stages[i];
        if (!s.enabled) continue;
        const double a = ClipTimeFromStageFrame(c.anim, s, s.startFrame);
        const double b = ClipTimeFromStageFrame(c.anim, s, s.endFrame);
        t0 = std::min(t0, std::min(a, b));
        t1 = std::max(t1, std::max(a, b));
    }

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

OfxRectD TimelineWidget::syncButtonRect(const OverlayContext& c, bool easing) const
{
    // In the panel's header strip, between "STAGE TIMING" and the frame
    // read-out. That band was empty, and both buttons act on the very thing the
    // lanes below them show -- which is also the only space left: the toolbar's
    // right-hand run is already six buttons deep.
    const double w = c.sx(kSyncWPx);
    const double h = c.sy(kSyncHPx);

    OfxRectD r;
    r.x2 = _rect.x2 - c.sx(kPadPx) - c.sx(58.0)          // clear of "frame 123"
         - (easing ? 0.0 : (w + c.sx(kLaneGapPx)));
    r.x1 = r.x2 - w;
    r.y2 = _rect.y2 - c.sy(3.0);
    r.y1 = r.y2 - h;
    return r;
}

void TimelineWidget::drawSyncButtons(const OverlayContext& c)
{
    // Momentary, so neither is ever drawn lit.
    Button(c, syncButtonRect(c, false), "SYNC TIME", false, colours::kAccent);
    Button(c, syncButtonRect(c, true),  "SYNC EASE", false, colours::kAccent);
}

bool TimelineWidget::syncButtonHit(const OverlayContext& c, const OfxPointD& p) const
{
    for (int k = 0; k < 2; ++k)
    {
        const bool easing = (k == 1);
        if (!Contains(syncButtonRect(c, easing), p)) continue;

        // Flip the hidden trigger; changedParam reads the flip as the press and
        // does the work, so the overlay and the Inspector run the same code.
        // Not wrapped in an edit block here: the handler opens its own, and both
        // can raise a message, which must not happen inside somebody else's
        // paramEditBegin.
        OFX::BooleanParam* t = c.effect->fetchBooleanParam(
            easing ? kParamSyncEaseFromOverlay : kParamSyncPeakFromOverlay);
        bool v = false;
        t->getValue(v);
        t->setValue(!v);
        return true;
    }
    return false;
}

void TimelineWidget::drawVelocity(const OverlayContext& c, const Stage& s,
                                  const OfxRectD& lane, double xa, double xb,
                                  bool active) const
{
    // A stage that holds a pose rather than changing it has no velocity worth
    // plotting, and a bar only a few pixels wide has nowhere to plot it.
    if (!StageMoves(s)) return;
    if (std::fabs(xb - xa) < c.sx(14.0)) return;

    constexpr int kSteps = kVelocitySteps;
    double v[kSteps];
    double vMax = 0.0;

    for (int k = 0; k < kSteps; ++k)
    {
        v[k] = EasingSpeed(s.easing, k);
        if (v[k] > vMax) vMax = v[k];
    }
    if (vMax <= 1e-9) return;      // a flat curve: nothing to show

    // Overlaid on the bar as a brightness ramp. Deliberately additive-looking
    // white rather than a second hue: the bar's colour already identifies the
    // stage, and a competing hue made the lane hard to read at a glance.
    for (int k = 0; k < kSteps; ++k)
    {
        const double f0 = static_cast<double>(k)     / static_cast<double>(kSteps);
        const double f1 = static_cast<double>(k + 1) / static_cast<double>(kSteps);
        const double sx0 = xa + (xb - xa) * f0;
        const double sx1 = xa + (xb - xa) * f1;

        const float a = static_cast<float>(0.42 * (v[k] / vMax));
        SetColour(c, { 1.0f, 1.0f, 1.0f, a });
        FillRect(c, std::min(sx0, sx1), lane.y1, std::max(sx0, sx1), lane.y2);
    }

    // The peak itself: where the move is at its fastest, and so where a cut or a
    // hit should usually land.
    const double px = xa + (xb - xa) * static_cast<double>(PeakVelocityProgress(s.easing));

    SetColour(c, colours::kPlayhead);
    SetLineWidth(c, active ? 2.0f : 1.0f);
    Line(c, px, lane.y1, px, lane.y2);

    // A tick above the lane, so the peak is still findable when the bar is
    // crowded or the lane is short.
    if (active)
    {
        SetColour(c, colours::kPlayhead);
        Line(c, px - c.sx(3.0), lane.y2 + c.sy(3.0), px, lane.y2);
        Line(c, px + c.sx(3.0), lane.y2 + c.sy(3.0), px, lane.y2);
    }
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

    drawSyncButtons(c);

    // The clip's own extent, so it is obvious where frame 0 and the last frame
    // are and whether a stage has been pushed outside them.
    if (c.clipLength > 0.0)
    {
        const double cx0 = frameToX(0.0);
        const double cx1 = frameToX(c.clipLength);
        const double top = _rect.y2 - c.sy(kHeaderPx);

        SetColour(c, { 1.0f, 1.0f, 1.0f, 0.10f });
        FillRect(c, cx0, _rect.y1 + c.sy(3.0), cx1, top);

        SetColour(c, colours::kTextDim);
        SetLineWidth(c, 1.0f);
        Line(c, cx0, _rect.y1 + c.sy(3.0), cx0, top);
        Line(c, cx1, _rect.y1 + c.sy(3.0), cx1, top);

        Text(c, "0", cx0 + c.sx(3.0), _rect.y1 + c.sy(4.0),
             kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBottom);
        Text(c, FrameLabel(c.clipLength), cx1 - c.sx(3.0), _rect.y1 + c.sy(4.0),
             kOfxDrawTextAlignmentRight | kOfxDrawTextAlignmentBottom);
    }

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

        // Shifted into clip time, since the ruler is clip time but the stage's
        // own frames are measured from its anchor.
        const double ca = ClipTimeFromStageFrame(c.anim, s, s.startFrame);
        const double cb = ClipTimeFromStageFrame(c.anim, s, s.endFrame);
        double bx1 = frameToX(std::min(ca, cb));
        double bx2 = frameToX(std::max(ca, cb));

        // A zero-length stage would otherwise be invisible and impossible to grab.
        if (bx2 - bx1 < c.sx(3.0)) bx2 = bx1 + c.sx(3.0);

        const Colour lc = colours::kStage[i % 4];
        SetColour(c, lc);
        FillRect(c, bx1, lane.y1, bx2, lane.y2);

        // Where the move is actually happening. Drawn between the bar and its
        // outline so the shading never covers the grab affordances.
        drawVelocity(c, s, lane, frameToX(ca), frameToX(cb), active);

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

    if (syncButtonHit(c, p)) return true;

    for (int i = 0; i < c.stageCount; ++i)
    {
        const OfxRectD lane = laneRect(c, i);
        if (p.y < lane.y1 || p.y > lane.y2) continue;

        const Stage& s = c.anim.stages[i];
        if (!s.enabled) return true;   // swallow, but nothing to drag

        const double bx1 = frameToX(ClipTimeFromStageFrame(c.anim, s, s.startFrame));
        const double bx2 = frameToX(ClipTimeFromStageFrame(c.anim, s, s.endFrame));
        const double tol = c.sx(kEdgeGrabPx);

        _dragStage = i;
        // Recorded in the stage's own units, so a drag delta is directly usable
        // even when the anchor scales time rather than merely shifting it.
        _grabFrame = StageLocalTime(c.anim, s, static_cast<float>(xToFrame(p.x)));
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

    const Stage& s = c.anim.stages[_dragStage];

    // Converted into the stage's units before differencing, so a Stretch stage
    // -- whose values are percentages -- moves by the right amount rather than
    // by raw frame counts.
    const double frame  = StageLocalTime(c.anim, s, static_cast<float>(xToFrame(p.x)));
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
