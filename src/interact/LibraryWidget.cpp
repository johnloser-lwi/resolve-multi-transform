#include "Widgets.h"

#include "AnimEngine.h"
#include "EditBlock.h"
#include "ParamNames.h"
#include "PresetIO.h"

#include <algorithm>
#include <cmath>

namespace mtx {

namespace {
constexpr double kPadPx      = 10.0;
constexpr double kHeaderPx   = 20.0;
constexpr double kCellWPx    = 92.0;
constexpr double kCellHPx    = 74.0;
constexpr double kCellGapPx  = 8.0;
constexpr int    kPlotSteps  = 40;

/// The curve's y range, widened to whatever the curve actually reaches so an
/// overshoot or a bounce is visible in the thumbnail rather than clipped off.
void CurveRange(const Easing& e, double& lo, double& hi)
{
    lo = 0.0;
    hi = 1.0;
    for (int k = 0; k <= kPlotSteps; ++k)
    {
        const double y = ApplyEasing(static_cast<float>(k) / kPlotSteps, e);
        lo = std::min(lo, y);
        hi = std::max(hi, y);
    }
    const double pad = (hi - lo) * 0.12;
    lo -= pad;
    hi += pad;
    if (hi - lo < 1e-6) { lo -= 0.5; hi += 0.5; }
}
} // namespace

void LibraryWidget::reloadIfNeeded(const OverlayContext& /*c*/)
{
    if (_loaded) return;
    _loaded = true;
    _entries.clear();

    for (const std::string& path : ListJsonFiles(CurveFolder()))
    {
        std::string text, error;
        if (!ReadTextFile(path, text, error)) continue;

        CurvePreset c;
        // Files that are not curves are skipped in silence rather than reported.
        // The folder is the user's own, and a stray effect preset dropped in it
        // is not an error worth a dialog every time the panel opens.
        if (!CurveFromJson(text, c, error)) continue;

        Entry e;
        e.curve = c;
        e.label = c.name.empty() ? StemOf(path) : c.name;
        _entries.push_back(e);
    }
}

void LibraryWidget::layout(const OverlayContext& c)
{
    reloadIfNeeded(c);

    const double cellW = c.sx(kCellWPx);
    const double cellH = c.sy(kCellHPx);
    const double gap   = c.sx(kCellGapPx);

    // Sized to the contents up to a ceiling, so a library of three curves is a
    // small panel rather than a mostly-empty one.
    const int count = static_cast<int>(_entries.size());
    const int shown = count > 0 ? count : 1;

    _columns = std::min(shown, 5);
    _rows    = (shown + _columns - 1) / _columns;

    const double w = c.sx(kPadPx) * 2.0 + _columns * cellW + (_columns - 1) * gap;
    const double h = c.sy(kPadPx) * 2.0 + c.sy(kHeaderPx)
                   + _rows * cellH + (_rows - 1) * c.sy(kCellGapPx);

    // Centred over the image. The panel is temporary -- it is dismissed as soon
    // as a curve is picked -- so it can take the middle of the frame, where the
    // grid is easiest to scan.
    _rect.x1 = c.rod.x1 + (c.rodWidth()  - w) * 0.5;
    _rect.x2 = _rect.x1 + w;
    _rect.y2 = c.rod.y1 + (c.rodHeight() + h) * 0.5;
    _rect.y1 = _rect.y2 - h;
}

OfxRectD LibraryWidget::cellRect(const OverlayContext& c, int index) const
{
    const double cellW = c.sx(kCellWPx);
    const double cellH = c.sy(kCellHPx);
    const double gapX  = c.sx(kCellGapPx);
    const double gapY  = c.sy(kCellGapPx);

    const int col = index % _columns;
    const int row = index / _columns;

    OfxRectD r;
    r.x1 = _rect.x1 + c.sx(kPadPx) + col * (cellW + gapX);
    r.x2 = r.x1 + cellW;
    r.y2 = _rect.y2 - c.sy(kPadPx + kHeaderPx) - row * (cellH + gapY);
    r.y1 = r.y2 - cellH;
    return r;
}

void LibraryWidget::draw(const OverlayContext& c)
{
    layout(c);
    Panel(c, _rect);

    SetColour(c, colours::kText);
    Text(c, "CURVE LIBRARY", _rect.x1 + c.sx(kPadPx), _rect.y2 - c.sy(6.0),
         kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentTop);

    if (_entries.empty())
    {
        SetColour(c, colours::kTextDim);
        Text(c, "No saved curves yet - name one under Easing and click Save Curve",
             (_rect.x1 + _rect.x2) * 0.5, (_rect.y1 + _rect.y2) * 0.5,
             kOfxDrawTextAlignmentCenterH | kOfxDrawTextAlignmentCenterV);
        return;
    }

    for (int i = 0; i < static_cast<int>(_entries.size()); ++i)
    {
        const OfxRectD cell = cellRect(c, i);
        const Easing   e    = _entries[i].curve.ToEasing();

        SetColour(c, { 1.0f, 1.0f, 1.0f, 0.06f });
        FillRect(c, cell.x1, cell.y1, cell.x2, cell.y2);
        SetColour(c, colours::kPanelEdge);
        SetLineWidth(c, 1.0f);
        StrokeRect(c, cell.x1, cell.y1, cell.x2, cell.y2);

        // Plot area: the cell less a strip at the bottom for the name.
        const double labelH = c.sy(13.0);
        const double px1 = cell.x1 + c.sx(6.0);
        const double px2 = cell.x2 - c.sx(6.0);
        const double py1 = cell.y1 + labelH;
        const double py2 = cell.y2 - c.sy(4.0);

        double lo = 0.0, hi = 1.0;
        CurveRange(e, lo, hi);
        const double span = (hi - lo) > 1e-9 ? (hi - lo) : 1.0;

        // The 0 and 1 rails, so overshoot and anticipation are readable as
        // "past the end" rather than just as a wigglier line.
        SetColour(c, { 1.0f, 1.0f, 1.0f, 0.13f });
        for (int k = 0; k < 2; ++k)
        {
            const double y = py1 + ((k == 0 ? 0.0 : 1.0) - lo) / span * (py2 - py1);
            Line(c, px1, y, px2, y);
        }

        OfxPointD pts[kPlotSteps + 1];
        for (int k = 0; k <= kPlotSteps; ++k)
        {
            const double t = static_cast<double>(k) / kPlotSteps;
            const double y = ApplyEasing(static_cast<float>(t), e);
            pts[k].x = px1 + t * (px2 - px1);
            pts[k].y = py1 + (y - lo) / span * (py2 - py1);
        }

        SetColour(c, colours::kAccent);
        SetLineWidth(c, 1.5f);
        Polyline(c, pts, kPlotSteps + 1);

        // Left aligned inside the cell, not centred: CenterH is defined as
        // Left|Right and Resolve resolves that to a single edge, so a centred
        // label lands about half a word from where it belongs. See Button().
        SetColour(c, colours::kText);
        const double labelPx = kCellWPx - 8.0;
        Text(c, FitLabel(_entries[i].label, labelPx), cell.x1 + c.sx(4.0), cell.y1 + c.sy(2.0),
             kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBottom);
    }
}

void LibraryWidget::applyCurve(const OverlayContext& c, const Entry& e) const
{
    const int i = c.activeStage;

    EditBlock block(c.effect, "Apply Curve");

    // The dropdown is set to Custom rather than left alone. It is only a label
    // for the four amounts below it, and leaving it reading "Smooth" over a
    // curve that is no longer smooth would be a lie the next edit would act on.
    c.effect->fetchChoiceParam(StageParam(kParamEasingPreset, i))->setValue(kEasingCustom);

    SetDouble(c.effect, StageParam(kParamEaseIn,        i), e.curve.easeIn);
    SetDouble(c.effect, StageParam(kParamEaseOut,       i), e.curve.easeOut);
    SetDouble(c.effect, StageParam(kParamAnticipation,  i), e.curve.anticipation);
    SetDouble(c.effect, StageParam(kParamOvershoot,     i), e.curve.overshoot);
    SetDouble(c.effect, StageParam(kParamBounceAmount,  i), e.curve.bounceAmount);
    SetDouble(c.effect, StageParam(kParamBounceCount,   i), e.curve.bounceCount);
    SetDouble(c.effect, StageParam(kParamBounceDamping, i), e.curve.bounceDamping);
    SetDouble(c.effect, StageParam(kParamBounceStart,   i), e.curve.bounceStart);

    c.effect->fetchChoiceParam(StageParam(kParamBounceType, i))->setValue(e.curve.bounceType);
}

bool LibraryWidget::penDown(const OverlayContext& c, const OfxPointD& p)
{
    if (!Contains(_rect, p)) return false;

    for (int i = 0; i < static_cast<int>(_entries.size()); ++i)
    {
        if (!Contains(cellRect(c, i), p)) continue;

        applyCurve(c, _entries[i]);

        // Dismissed on pick. The panel covers the middle of the image, and
        // leaving it up over the result of the curve just chosen would hide the
        // one thing worth looking at next.
        c.effect->fetchBooleanParam(kParamShowLibrary)->setValue(false);
        return true;
    }

    return true;   // clicks inside the panel never fall through to the image
}

bool LibraryWidget::penMotion(const OverlayContext& /*c*/, const OfxPointD& /*p*/) { return false; }
bool LibraryWidget::penUp    (const OverlayContext& /*c*/, const OfxPointD& /*p*/) { return false; }

} // namespace mtx
