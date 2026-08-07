#pragma once

#include "DrawUtils.h"

#include "CurvePreset.h"

#include <vector>

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
    /** @brief Shade a stage's bar by how fast it is moving, and mark the peak.
     *
     * @p xa and @p xb are the start and end of the stage in screen X *in that
     * order*, which is not the same as left and right: a stage whose end frame
     * precedes its start plays backwards, and the shading has to run with it.
     */
    void drawVelocity(const OverlayContext& c, const Stage& s, const OfxRectD& lane,
                      double xa, double xb, bool active) const;

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

    /** @brief Apply the Shift axis lock, relative to where the handle started.
     *
     * On this panel the two axes are two different questions: horizontal is the
     * timing (Ease In / Ease Out) and vertical is the amount of anticipation or
     * overshoot. Locking one lets either be adjusted without disturbing the
     * other, which is otherwise almost impossible to do by hand.
     */
    OfxPointD constrain(const OverlayContext& c, const OfxPointD& unit) const;

    /// The dragged handle's position when the drag began, in unit coordinates.
    OfxPointD _grabUnit{};

    OfxRectD _rect{};
    OfxRectD _plot{};
    double   _yMin = kYMinDefault;
    double   _yMax = kYMaxDefault;
};

/** @brief The saved-curve picker: a grid of curves drawn as curves.
 *
 * A dropdown of names would say "Heavy Settle" and leave you to remember what
 * that looked like. Each cell here is plotted with the renderer's own
 * evaluator, so the thumbnail is the curve, and picking one is recognition
 * rather than recall.
 *
 * The library is read from disk when the panel opens rather than on every
 * redraw -- a directory scan and a parse per frame during a drag would be
 * absurd, and curves do not change while the panel is up.
 */
class LibraryWidget : public Widget
{
public:
    void layout(const OverlayContext& c) override;
    void draw(const OverlayContext& c) override;
    bool penDown  (const OverlayContext& c, const OfxPointD& p) override;
    bool penMotion(const OverlayContext& c, const OfxPointD& p) override;
    bool penUp    (const OverlayContext& c, const OfxPointD& p) override;

    const OfxRectD& rect() const { return _rect; }

    /// Re-scan the curve folder. Called when the panel is opened, and after a
    /// save, so a curve just stored shows up without a reload.
    void invalidate() { _loaded = false; }

private:
    struct Entry
    {
        CurvePreset curve;
        std::string label;
    };

    void      reloadIfNeeded(const OverlayContext& c);
    OfxRectD  cellRect(const OverlayContext& c, int index) const;
    void      applyCurve(const OverlayContext& c, const Entry& e) const;

    OfxRectD             _rect{};
    std::vector<Entry>   _entries;
    bool                 _loaded = false;
    int                  _columns = 1;
    int                  _rows    = 1;
};

/** @brief The trajectory the active stage travels, drawn over the image.
 *
 * Position otherwise interpolates in a straight line; dragging these two
 * handles bends it. Easing still governs the speed along the route, so this
 * changes where the move goes without touching how it accelerates.
 */
class PathWidget : public Widget
{
public:
    void layout(const OverlayContext& c) override;
    void draw(const OverlayContext& c) override;
    bool penDown  (const OverlayContext& c, const OfxPointD& p) override;
    bool penMotion(const OverlayContext& c, const OfxPointD& p) override;
    bool penUp    (const OverlayContext& c, const OfxPointD& p) override;

private:
    enum DragMode { kNone = 0, kDragC1, kDragC2 };

    /// Where the grabbed handle sat when the drag began, in normalised units.
    /// Shift locks movement relative to this rather than to the cursor, so the
    /// handle slides along one axis instead of jumping to the pointer.
    double _grabNx = 0.0, _grabNy = 0.0;

    /** Clip time of the stage's start or end. Every mapping below is pinned to
     *  one of these two, never to the playhead: the route the image travels is
     *  a fixed feature of the animation and must not slide about as you scrub. */
    double endpointTime(const OverlayContext& c, bool end) const;

    /// Normalised position -> canonical image coordinates, as of clip time @p t.
    OfxPointD toScreen(const OverlayContext& c, float nx, float ny, double t) const;
    /// The exact inverse, so a handle dragged at @p t lands under the cursor.
    void      toNormalised(const OverlayContext& c, const OfxPointD& p, double t,
                           double& nx, double& ny) const;
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

    struct Pose
    {
        double scale, scaleY, rot, posX, posY, anchorX, anchorY;
        bool   linkScale;   ///< when set, scaleY is ignored and never written

        /// Read-only here: the gizmo draws the box foreshortened so the outline
        /// matches what renders, but there is no handle that edits them.
        double tiltX, swivelY;
    };

    Pose readPose(const OverlayContext& c) const;
    void writePose(const OverlayContext& c, const Pose& p) const;

    /// The stage's own matrix, in the space the stage's numbers are written in.
    Mat3 poseMatrix(const OverlayContext& c, const Pose& p) const;

    /** @brief Clip time the surrounding stages are sampled at.
     *
     * The stage's *own* start or end frame, never the playhead. Sampling the
     * neighbours at the playhead instead makes the whole frame of reference
     * drift as you scrub, so the gizmo and the motion path swim across the
     * picture rather than marking fixed places in the move.
     */
    double refTime(const OverlayContext& c) const;

    /// The same pose composed with the surrounding stages, which is where the
    /// image actually is. Everything drawn and hit-tested uses this.
    Mat3 displayMatrix(const OverlayContext& c, const Pose& p) const;

    /** @brief A screen point mapped back into the stage's own space.
     *
     * Drags are measured here rather than on screen. The stages to the left of
     * this one may scale or rotate everything it does, so a raw screen delta
     * would write the wrong numbers into the stage -- and increasingly wrong the
     * more the other stages are doing.
     */
    OfxPointD toStageSpace(const OverlayContext& c, const OfxPointD& p) const;

    /// Screen position of the pose's anchor. The anchor lives in the stage's own
    /// input space, so it goes through `outer * Sᵢ` and not through `inner`.
    OfxPointD anchorScreen(const OverlayContext& c, const Pose& p) const;

    Pose      _grabPose{};
    OfxPointD _anchorScreen{};   ///< screen space, for hit-testing the anchor handle
    // Drag state, all in the stage's own space rather than on screen -- see
    // toStageSpace(). Mixing the two was the bug this exists to prevent.
    OfxPointD _grabPoint{};
    OfxPointD _anchorLocal{};
    double    _grabAngle  = 0.0;
    double    _grabRadius = 1.0;
};

} // namespace mtx
