#pragma once

#include "DrawUtils.h"

namespace mtx {

/** @brief A piece of the overlay that draws itself and may claim pen input.
 *
 * Returning true from a pen handler claims the event, which stops it reaching
 * other widgets *and* stops the host acting on it. Widgets are offered events
 * in front-to-back order so HUD panels win over the gizmo underneath them.
 */
class Widget
{
public:
    virtual ~Widget() = default;

    virtual void layout(const OverlayContext& c) = 0;
    virtual void draw(const OverlayContext& c)   = 0;

    virtual bool penDown  (const OverlayContext& c, const OfxPointD& p) = 0;
    virtual bool penMotion(const OverlayContext& c, const OfxPointD& p) = 0;
    virtual bool penUp    (const OverlayContext& c, const OfxPointD& p) = 0;

    bool dragging() const { return _drag != 0; }

protected:
    int _drag = 0;   ///< 0 = idle; widget-specific drag mode otherwise
};

/** @brief Timeline strip: one lane per stage, showing its start and end frame.
 *
 * This is the control that makes staggering tangible -- the lanes make the
 * offsets between stages visible instead of being four pairs of numbers spread
 * down the Inspector.
 */
class TimelineWidget : public Widget
{
public:
    void layout(const OverlayContext& c) override;
    void draw(const OverlayContext& c) override;
    bool penDown  (const OverlayContext& c, const OfxPointD& p) override;
    bool penMotion(const OverlayContext& c, const OfxPointD& p) override;
    bool penUp    (const OverlayContext& c, const OfxPointD& p) override;

    /// Lane rectangle for a stage, used by the stage-tab hit testing.
    OfxRectD laneRect(const OverlayContext& c, int stage) const;
    const OfxRectD& rect() const { return _rect; }

private:
    enum DragMode { kNone = 0, kDragStart, kDragEnd, kDragWhole };

    double frameToX(double frame) const;
    double xToFrame(double x) const;

    OfxRectD _rect{};
    double   _t0 = 0.0, _t1 = 1.0;   ///< visible frame range

    int    _dragStage = 0;
    double _grabFrame = 0.0;         ///< frame under the cursor at pen-down
    double _grabStart = 0.0;
    double _grabEnd   = 0.0;
    // The visible range is frozen for the duration of a drag: recomputing it
    // live would move the bar out from under the cursor as it is dragged.
    double _dragT0 = 0.0, _dragT1 = 1.0;
};

/** @brief Bezier curve editor for the active stage's easing.
 *
 * Writes back to the same four amounts the Inspector exposes (Ease In, Ease
 * Out, Anticipation, Overshoot), so the two views never disagree.
 */
class CurveWidget : public Widget
{
public:
    void layout(const OverlayContext& c) override;
    void draw(const OverlayContext& c) override;
    bool penDown  (const OverlayContext& c, const OfxPointD& p) override;
    bool penMotion(const OverlayContext& c, const OfxPointD& p) override;
    bool penUp    (const OverlayContext& c, const OfxPointD& p) override;

    const OfxRectD& rect() const { return _rect; }

private:
    enum DragMode { kNone = 0, kDragP1, kDragP2 };

    // The plotted y range is wider than 0..1 so anticipation and overshoot are
    // visible rather than clipped. A spring can exceed even this, so the view
    // grows to fit the curve actually being drawn -- see fitRange().
    static constexpr double kYMinDefault = -0.65;
    static constexpr double kYMaxDefault =  1.65;

    void      fitRange(const OverlayContext& c);
    OfxPointD unitToPanel(double ux, double uy) const;
    OfxPointD panelToUnit(const OfxPointD& p) const;
    void      writeHandle(const OverlayContext& c, int which, const OfxPointD& unit);

    OfxRectD _rect{};
    OfxRectD _plot{};
    double   _yMin = kYMinDefault;
    double   _yMax = kYMaxDefault;
};

/** @brief On-image transform gizmo for the active stage.
 *
 * Shows either the From or the To state, so a stage's two ends can be posed
 * directly on the picture instead of typed as numbers.
 */
class GizmoWidget : public Widget
{
public:
    void layout(const OverlayContext& c) override;
    void draw(const OverlayContext& c) override;
    bool penDown  (const OverlayContext& c, const OfxPointD& p) override;
    bool penMotion(const OverlayContext& c, const OfxPointD& p) override;
    bool penUp    (const OverlayContext& c, const OfxPointD& p) override;

private:
    enum DragMode { kNone = 0, kDragMove, kDragScale, kDragRotate, kDragAnchor };

    struct Pose { double scale, rot, posX, posY, anchorX, anchorY; };

    Pose readPose(const OverlayContext& c) const;
    void writePose(const OverlayContext& c, const Pose& p) const;
    Mat3 poseMatrix(const OverlayContext& c, const Pose& p) const;

    Pose      _grabPose{};
    OfxPointD _grabPoint{};
    OfxPointD _anchorScreen{};
    double    _grabAngle  = 0.0;
    double    _grabRadius = 1.0;
};

} // namespace mtx
