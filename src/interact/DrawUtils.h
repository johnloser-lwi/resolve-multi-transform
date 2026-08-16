#pragma once

// Thin helpers over OfxDrawSuiteV1.
//
// Everything the overlay draws is in *canonical image coordinates*, because
// that is the space the host sets up the projection for. Anything that should
// look a fixed size on screen regardless of viewer zoom has to be scaled by
// pixelScale, which is the size of one screen pixel in canonical units. Getting
// that wrong is the classic overlay bug where handles become unusably tiny when
// you zoom out.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

#include "AnimEngine.h"

#include "ofxDrawSuite.h"
#include "ofxsImageEffect.h"
#include "ofxsSupportPrivate.h"

namespace mtx {

struct Colour
{
    float r, g, b, a;
};

// A restrained palette: the overlay sits on top of the user's picture, so it
// stays legible without shouting over the image.
namespace colours {
constexpr Colour kPanel      = { 0.08f, 0.09f, 0.11f, 0.82f };
constexpr Colour kPanelEdge  = { 0.35f, 0.38f, 0.44f, 0.90f };
constexpr Colour kText       = { 0.88f, 0.90f, 0.94f, 1.00f };
constexpr Colour kTextDim    = { 0.55f, 0.58f, 0.64f, 1.00f };
constexpr Colour kGrid       = { 0.28f, 0.30f, 0.35f, 0.70f };
constexpr Colour kAccent     = { 0.20f, 0.72f, 1.00f, 1.00f };
constexpr Colour kPlayhead   = { 1.00f, 0.85f, 0.25f, 1.00f };
constexpr Colour kGizmo      = { 0.20f, 0.85f, 1.00f, 0.95f };
constexpr Colour kGizmoTo    = { 1.00f, 0.55f, 0.20f, 0.95f };
constexpr Colour kHandle     = { 1.00f, 1.00f, 1.00f, 1.00f };

/** @brief The dark backing drawn under every bright mark on the picture.
 *
 *  ## Why not simply pick a colour that always shows
 *
 *  There isn't one. Any single ink -- red included -- is a point in the same
 *  colour space as the footage, so some footage sits right on top of it: red
 *  disappears into skin tones, sunsets, tail lights and a Resolve colour-page
 *  scope. Choosing a colour only moves which shots the overlay is invisible on.
 *
 *  Drawing each mark twice does not have that problem, because it stops
 *  depending on the *colour* of the background and starts depending only on its
 *  *brightness*: a wide near-black pass first, then the real colour thinner on
 *  top. On white footage the black rim carries the shape; on black footage the
 *  bright core does; on mid greys both edges read. The mark also keeps its
 *  meaning -- cyan still means FROM, orange still means TO -- which a uniform
 *  red would have thrown away.
 *
 *  Not fully opaque, so the picture underneath is still readable through the
 *  rim rather than being blocked out by a heavy outline.
 */
constexpr Colour kHalo       = { 0.00f, 0.00f, 0.00f, 0.72f };

/// Per-stage lane colours, distinct enough to tell apart at a glance.
constexpr Colour kStage[4] = {
    { 0.30f, 0.72f, 1.00f, 0.90f },   // blue
    { 0.45f, 0.85f, 0.45f, 0.90f },   // green
    { 1.00f, 0.72f, 0.30f, 0.90f },   // amber
    { 0.85f, 0.55f, 1.00f, 0.90f }    // violet
};
} // namespace colours

/** @brief Everything a widget needs to draw itself and interpret a click. */
struct OverlayContext
{
    OFX::ImageEffect*    effect = nullptr;
    OfxDrawContextHandle ctx    = nullptr;

    /// Clip-relative: 0 is the clip's first frame, matching how stage timing is
    /// authored, so nothing in the overlay has to know about timeline position.
    double               time   = 0.0;

    /// Length of the clip in frames, or 0 when the host does not report it.
    double               clipLength = 0.0;
    OfxPointD            pixelScale{ 1.0, 1.0 };
    OfxRectD             rod{ 0.0, 0.0, 0.0, 0.0 };   ///< image bounds, canonical

    int  activeStage = 0;      ///< which stage the gizmo and curve editor act on
    /// Gizmo edits the "To" state rather than "From" -- shown as B and A in the
    /// interface. The internal names stayed as From/To when the labels changed,
    /// so that saved projects and presets kept working; see the EndSpec table in
    /// describeInContext.
    bool editTo      = true;

    /// The gizmo poses the Base Transform instead of a stage end. A third
    /// setting of the same Gizmo Edits parameter, so the three are exclusive.
    bool editBase    = false;
    int  stageCount  = 1;

    /// The curve panel sits over the image and is hit-tested before the motion
    /// path, so it can swallow path handles underneath it. Turning it off gets
    /// it out of the way.
    bool showCurve   = true;

    /// The saved-curve picker. Modal while it is up: it covers the middle of
    /// the image and takes every click inside itself.
    bool showLibrary = false;

    /// The Quick Control panel, raised from the toolbar's QUICK button. Modal
    /// on the same terms as the library, and dismissed as soon as an action is
    /// picked.
    bool showQuick   = false;

    /// Whether dragging a gizmo previews the picture at the dragged pose.
    /// Not always wanted: on a pure zoom the preview says little the outline
    /// does not already say.
    bool dragPreview  = true;

    // Every panel is independently switchable, so the overlay can be pared back
    // to just what a given edit needs rather than showing all of its furniture
    // over the picture at once. A hidden panel is skipped for hit-testing too,
    // not merely undrawn -- see the widget order in OverlayInteract::penDown.
    bool showTimeline = true;
    bool showPath     = false;
    bool showOpacity  = false;

    /// Shift held. OFX's pen actions carry no modifier state at all -- PenArgs
    /// has position and pressure and nothing else -- so this is tracked from
    /// keyDown/keyUp and folded in here rather than read off the drag.
    bool shiftHeld   = false;

    /// Control held: drag both handles of a pair together, mirrored. Combines
    /// with Shift, giving an axis-locked symmetric edit.
    bool ctrlHeld    = false;

    /// The animation as the renderer will see it, read straight from the
    /// parameters, so the overlay can never show something different.
    AnimParams anim = AnimParams::Default();

    /// Convert a screen-pixel length into canonical units.
    double sx(double screenPixels) const { return screenPixels * pixelScale.x; }
    double sy(double screenPixels) const { return screenPixels * pixelScale.y; }

    double rodWidth()  const { return rod.x2 - rod.x1; }
    double rodHeight() const { return rod.y2 - rod.y1; }
};

/** @brief Detects a repeat click from a stream of single presses.
 *
 * OFX has no double-click action and PenArgs carries no click count -- it has a
 * position and a pressure and nothing else -- so the only way to recognise one
 * is to remember the last press and time the next.
 *
 * @p slot identifies *what* was hit, so two quick clicks on different handles
 * are two single clicks rather than a repeat on whichever came second.
 *
 * ## It takes three clicks in Resolve, not two
 *
 * Known and accepted rather than unexplained. The window here is already very
 * generous -- widening it from 400 ms to 900 ms changed nothing, which rules
 * out the obvious culprit of the host delivering the second press late. What
 * fits the behaviour is that Resolve consumes the *first* press to give the
 * on-screen controls focus, so the plugin only ever sees presses two and three;
 * those two then form the pair that fires.
 *
 * A duplicated press would produce the opposite symptom -- a reset on a single
 * click -- so that is ruled out too. Nothing here can recover a press that never
 * arrives, so the gesture is three clicks in this host and the feature is kept
 * on that basis.
 */
class ClickTracker
{
public:
    bool isDouble(const OverlayContext& c, const OfxPointD& p, int slot)
    {
        const double now = Seconds();

        const bool quick = (now - _time) < kIntervalSeconds;
        const bool near  = std::fabs(p.x - _point.x) <= c.sx(kSlopPx)
                        && std::fabs(p.y - _point.y) <= c.sy(kSlopPx);
        const bool same  = (slot == _slot);

        _point = p;
        _slot  = slot;

        if (quick && near && same)
        {
            // Forgotten immediately, so a further click starts a fresh pair
            // rather than firing again.
            _time = -1e9;
            return true;
        }

        _time = now;
        return false;
    }

private:
    static double Seconds()
    {
        using clock = std::chrono::steady_clock;
        return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    }

    /// Deliberately loose. The tight position and handle tests are what stop
    /// this firing on two deliberate separate clicks, so the time limit can
    /// afford slack -- and slack is worth having when a press is being eaten.
    static constexpr double kIntervalSeconds = 0.9;
    static constexpr double kSlopPx          = 6.0;

    double    _time  = -1e9;
    OfxPointD _point{};
    int       _slot  = -1;
};

/** @brief Drop the smaller component of a drag, locking it to one axis.
 *
 * Decided from the drag so far rather than latched when Shift went down: the
 * gesture is "hold Shift *while* dragging", so pressing and releasing it
 * mid-drag has to make the constraint come and go.
 */
inline void LockToAxis(double& dx, double& dy)
{
    if (std::fabs(dx) >= std::fabs(dy)) dy = 0.0;
    else                                dx = 0.0;
}

/** @brief Round @p v to the nearest multiple of @p step.
 *
 * Shift means "coarser" throughout the overlay: 15-degree rotation on the
 * gizmo, five-frame timing on the timeline. Keeping one implementation means
 * those behave identically at the boundaries -- rounding to nearest, not
 * truncating, so a value already on a step does not creep when Shift is pressed
 * mid-drag. */
inline double SnapTo(double v, double step)
{
    if (step <= 0.0) return v;
    return std::floor(v / step + 0.5) * step;
}

// --- Primitive helpers -------------------------------------------------------

inline void SetColour(const OverlayContext& c, const Colour& col)
{
    const OfxRGBAColourF v = { col.r, col.g, col.b, col.a };
    OFX::Private::gDrawSuite->setColour(c.ctx, &v);
}

inline void SetLineWidth(const OverlayContext& c, float w)
{
    OFX::Private::gDrawSuite->setLineWidth(c.ctx, w);
}

inline void FillRect(const OverlayContext& c, double x1, double y1, double x2, double y2)
{
    const OfxPointD p[2] = { { x1, y1 }, { x2, y2 } };
    OFX::Private::gDrawSuite->draw(c.ctx, kOfxDrawPrimitiveRectangle, p, 2);
}

inline void StrokeRect(const OverlayContext& c, double x1, double y1, double x2, double y2)
{
    const OfxPointD p[4] = { { x1, y1 }, { x2, y1 }, { x2, y2 }, { x1, y2 } };
    OFX::Private::gDrawSuite->draw(c.ctx, kOfxDrawPrimitiveLineLoop, p, 4);
}

inline void Line(const OverlayContext& c, double x1, double y1, double x2, double y2)
{
    const OfxPointD p[2] = { { x1, y1 }, { x2, y2 } };
    OFX::Private::gDrawSuite->draw(c.ctx, kOfxDrawPrimitiveLines, p, 2);
}

inline void Polyline(const OverlayContext& c, const OfxPointD* pts, int n)
{
    if (n >= 2) OFX::Private::gDrawSuite->draw(c.ctx, kOfxDrawPrimitiveLineStrip, pts, n);
}

/** @brief Filled convex polygon. The only way to shade a *rotated* quad --
 *  kOfxDrawPrimitiveRectangle takes two corners and is axis-aligned. */
inline void Polygon(const OverlayContext& c, const OfxPointD* pts, int n)
{
    if (n >= 3) OFX::Private::gDrawSuite->draw(c.ctx, kOfxDrawPrimitivePolygon, pts, n);
}

inline void LineLoop(const OverlayContext& c, const OfxPointD* pts, int n)
{
    if (n >= 2) OFX::Private::gDrawSuite->draw(c.ctx, kOfxDrawPrimitiveLineLoop, pts, n);
}

inline void Ellipse(const OverlayContext& c, double cx, double cy, double rx, double ry)
{
    const OfxPointD p[2] = { { cx - rx, cy - ry }, { cx + rx, cy + ry } };
    OFX::Private::gDrawSuite->draw(c.ctx, kOfxDrawPrimitiveEllipse, p, 2);
}

inline void Text(const OverlayContext& c, const std::string& s, double x, double y,
                 int alignment = kOfxDrawTextAlignmentLeft)
{
    const OfxPointD p = { x, y };
    OFX::Private::gDrawSuite->drawText(c.ctx, s.c_str(), &p, alignment);
}

/** @brief A square handle of a fixed on-screen size, with a contrasting outline
 *  so it stays visible over both bright and dark footage. */
inline void Handle(const OverlayContext& c, double x, double y,
                   const Colour& fill, double screenSize = 5.0)
{
    const double hx = c.sx(screenSize);
    const double hy = c.sy(screenSize);

    SetColour(c, { 0.0f, 0.0f, 0.0f, 0.7f });
    FillRect(c, x - hx - c.sx(1), y - hy - c.sy(1), x + hx + c.sx(1), y + hy + c.sy(1));
    SetColour(c, fill);
    FillRect(c, x - hx, y - hy, x + hx, y + hy);
}

// --- Haloed primitives -------------------------------------------------------
//
// Use these for anything drawn *over the picture*. Marks that sit on a panel do
// not need them: the panel is already a dark backdrop of known brightness, so
// the contrast problem is solved there by the panel itself.
//
// The backing pass is `kHaloExtraPx` wider on each side, which is what makes the
// rim visible either side of a thin line. See colours::kHalo for why this is
// done instead of choosing a more visible colour.

constexpr double kHaloExtraPx = 2.0;

inline void HaloLine(const OverlayContext& c, double x1, double y1, double x2, double y2,
                     const Colour& col, double widthPx = 1.5)
{
    SetColour(c, colours::kHalo);
    SetLineWidth(c, static_cast<float>(widthPx + kHaloExtraPx));
    Line(c, x1, y1, x2, y2);

    SetColour(c, col);
    SetLineWidth(c, static_cast<float>(widthPx));
    Line(c, x1, y1, x2, y2);
}

inline void HaloLineLoop(const OverlayContext& c, const OfxPointD* pts, int n,
                         const Colour& col, double widthPx = 1.5)
{
    SetColour(c, colours::kHalo);
    SetLineWidth(c, static_cast<float>(widthPx + kHaloExtraPx));
    LineLoop(c, pts, n);

    SetColour(c, col);
    SetLineWidth(c, static_cast<float>(widthPx));
    LineLoop(c, pts, n);
}

inline void HaloPolyline(const OverlayContext& c, const OfxPointD* pts, int n,
                         const Colour& col, double widthPx = 1.5)
{
    SetColour(c, colours::kHalo);
    SetLineWidth(c, static_cast<float>(widthPx + kHaloExtraPx));
    Polyline(c, pts, n);

    SetColour(c, col);
    SetLineWidth(c, static_cast<float>(widthPx));
    Polyline(c, pts, n);
}

/** @brief A filled dot with a dark rim around it.
 *
 *  The rim is a *larger filled* ellipse rather than a stroked one, because a
 *  stroke of the same radius would be centred on the edge and eat half its width
 *  out of the dot itself, leaving these small markers noticeably thinner. */
inline void HaloDot(const OverlayContext& c, double cx, double cy,
                    double rx, double ry, const Colour& col)
{
    SetColour(c, colours::kHalo);
    Ellipse(c, cx, cy, rx + c.sx(kHaloExtraPx * 0.5), ry + c.sy(kHaloExtraPx * 0.5));
    SetColour(c, col);
    Ellipse(c, cx, cy, rx, ry);
}

/** @brief Text with a dark outline, for labels that sit on the picture.
 *
 *  Drawn as four offset copies rather than one drop shadow: a shadow only
 *  protects the two edges it falls on, and a label over white footage needs all
 *  four. There is no text-metrics or outline call in OfxDrawSuite to do this
 *  properly, so it is five draws of the same string -- cheap enough, since only
 *  a handful of labels are ever over the image rather than on a panel. */
inline void HaloText(const OverlayContext& c, const std::string& s, double x, double y,
                     const Colour& col, int alignment = kOfxDrawTextAlignmentLeft)
{
    const double ox = c.sx(1.0);
    const double oy = c.sy(1.0);

    SetColour(c, colours::kHalo);
    Text(c, s, x - ox, y - oy, alignment);
    Text(c, s, x + ox, y - oy, alignment);
    Text(c, s, x - ox, y + oy, alignment);
    Text(c, s, x + ox, y + oy, alignment);

    SetColour(c, col);
    Text(c, s, x, y, alignment);
}

/** @brief Rough on-screen width of a string, in screen pixels.
 *
 * An estimate, and it has to be: OfxDrawSuite has no text-metrics call at all.
 * `drawText` is the only text entry point and nothing reports a width, so the
 * true size of a label is simply not knowable from a plugin.
 *
 * Deliberately a little pessimistic. Erring wide costs at most one trimmed
 * character; erring narrow puts text back outside its box, which is the bug
 * this exists to stop.
 */
inline double EstimateTextPx(const std::string& text)
{
    constexpr double kGlyphAdvancePx = 7.0;
    return static_cast<double>(text.size()) * kGlyphAdvancePx;
}

/** @brief Trim @p text until the estimate fits @p maxPx. */
inline std::string FitLabel(const std::string& text, double maxPx)
{
    if (maxPx <= 0.0) return std::string();
    if (EstimateTextPx(text) <= maxPx) return text;

    constexpr double kGlyphAdvancePx = 7.0;
    size_t n = static_cast<size_t>(maxPx / kGlyphAdvancePx);
    if (n >= text.size()) return text;

    // A box too small for even one character shows nothing rather than a stray
    // glyph hanging over the edge.
    return n == 0 ? std::string() : text.substr(0, n);
}

/** @brief Draw a button: filled rect, outline, and a label inside it.
 *
 * The label is placed with **single-bit alignment flags only**, and that is the
 * point of this existing at all.
 *
 * OFX defines the centring flags as combinations of the edge ones --
 * `kOfxDrawTextAlignmentCenterH` is literally `Left|Right`, and `CenterV` is
 * `Top|Baseline`. A host that tests those bits in order rather than matching
 * the combined value will see the Left bit (or the Top bit) and align to that
 * edge instead of to the centre. Resolve appears to do exactly this: labels
 * came out offset from their boxes by roughly half a word, which is what
 * "centred" degrades into when the centring flag is read as an edge.
 *
 * `Left` (0x1) and `Baseline` (0x10) are single bits with no such ambiguity, so
 * the text is positioned by hand against them and the host has nothing left to
 * interpret. There is no way to measure text in OfxDrawSuite -- no metrics call
 * exists -- so true centring cannot be computed by the plugin either; a fixed
 * inset is both simpler and stable.
 */
inline void Button(const OverlayContext& c, const OfxRectD& r, const std::string& label,
                   bool active, const Colour& activeFill)
{
    SetColour(c, active ? activeFill : colours::kPanel);
    FillRect(c, r.x1, r.y1, r.x2, r.y2);

    SetColour(c, active ? colours::kHandle : colours::kPanelEdge);
    SetLineWidth(c, 1.0f);
    StrokeRect(c, r.x1, r.y1, r.x2, r.y2);

    SetColour(c, active ? Colour{ 0.05f, 0.06f, 0.08f, 1.0f } : colours::kText);

    // Trimmed to the box before it is drawn. This is the backstop: no caller
    // can overflow, including ones written later, and a layout that grows a
    // label or shrinks a box degrades to a clipped word instead of text running
    // across whatever is beside it.
    constexpr double kInsetPx = 6.0;
    const double avail = (r.x2 - r.x1) / (c.pixelScale.x > 1e-9 ? c.pixelScale.x : 1.0)
                       - kInsetPx * 2.0;
    const std::string shown = FitLabel(label, avail);
    if (shown.empty()) return;

    // Baseline sits a little below the box's middle, which is where a cap-height
    // glyph looks vertically centred.
    const OfxPointD p = { r.x1 + c.sx(kInsetPx), (r.y1 + r.y2) * 0.5 - c.sy(4.0) };
    OFX::Private::gDrawSuite->drawText(c.ctx, shown.c_str(), &p,
                                       kOfxDrawTextAlignmentLeft
                                       | kOfxDrawTextAlignmentBaseline);
}

/** @brief A filled panel with a border, used as the backdrop for HUD widgets. */
inline void Panel(const OverlayContext& c, const OfxRectD& r)
{
    SetColour(c, colours::kPanel);
    FillRect(c, r.x1, r.y1, r.x2, r.y2);
    SetColour(c, colours::kPanelEdge);
    SetLineWidth(c, 1.0f);
    StrokeRect(c, r.x1, r.y1, r.x2, r.y2);
}

inline bool Contains(const OfxRectD& r, const OfxPointD& p)
{
    return p.x >= r.x1 && p.x <= r.x2 && p.y >= r.y1 && p.y <= r.y2;
}

/** @brief Hit test against a point with a fixed on-screen tolerance. */
inline bool NearPoint(const OverlayContext& c, const OfxPointD& p,
                      double x, double y, double screenRadius = 8.0)
{
    return p.x >= x - c.sx(screenRadius) && p.x <= x + c.sx(screenRadius)
        && p.y >= y - c.sy(screenRadius) && p.y <= y + c.sy(screenRadius);
}

// --- Parameter access --------------------------------------------------------
// The interact reads and writes parameters by name. Handles are not cached
// because, unlike the render path, this runs at interaction rates where the
// lookup cost is irrelevant next to keeping the code simple.

inline double GetDouble(OFX::ImageEffect* e, const std::string& name, double t)
{
    return e->fetchDoubleParam(name)->getValueAtTime(t);
}

inline void SetDouble(OFX::ImageEffect* e, const std::string& name, double v)
{
    e->fetchDoubleParam(name)->setValue(v);
}

inline void GetDouble2D(OFX::ImageEffect* e, const std::string& name, double t,
                        double& x, double& y)
{
    e->fetchDouble2DParam(name)->getValueAtTime(t, x, y);
}

inline void SetDouble2D(OFX::ImageEffect* e, const std::string& name, double x, double y)
{
    e->fetchDouble2DParam(name)->setValue(x, y);
}

inline int GetChoice(OFX::ImageEffect* e, const std::string& name)
{
    int v = 0;
    e->fetchChoiceParam(name)->getValue(v);
    return v;
}

inline bool GetBool(OFX::ImageEffect* e, const std::string& name, double t)
{
    return e->fetchBooleanParam(name)->getValueAtTime(t);
}

/** @brief Format a frame number without trailing noise. */
inline std::string FrameLabel(double frame)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", static_cast<int>(frame + (frame < 0 ? -0.5 : 0.5)));
    return buf;
}

} // namespace mtx
