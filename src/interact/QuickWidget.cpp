#include "Widgets.h"

#include "EditBlock.h"
#include "ParamNames.h"

namespace mtx {

namespace {
constexpr double kPadPx      = 10.0;
constexpr double kHeaderPx   = 22.0;
constexpr double kRowWPx     = 156.0;   // fits "Flatten to Stage 1", the longest label
constexpr double kRowHPx     = 20.0;
constexpr double kRowGapPx   = 3.0;

/// A wider gap separates the three kinds of action, so what a row is about to
/// overwrite -- one end of a stage, a whole stage, or the whole effect -- is
/// visible from the layout rather than only from reading the label.
constexpr double kGroupGapPx = 9.0;

/// 0 for the FROM/TO transfers, 1 for the stage clipboard, 2 for the effect.
int GroupOf(int action)
{
    if (action <= kQuickSwapEnds)   return 0;
    if (action <= kQuickFlatten)    return 1;
    return 2;
}

/// The tint a row is drawn in: its group's, so the three read as three blocks.
Colour GroupTint(int action)
{
    switch (GroupOf(action))
    {
        case 0:  return colours::kGizmo;      // the two ends, matching FROM/TO
        case 1:  return colours::kStage[1];   // a stage
        default: return colours::kStage[3];   // the whole effect
    }
}
} // namespace

void QuickWidget::layout(const OverlayContext& c)
{
    const double w = c.sx(kPadPx * 2.0 + kRowWPx);
    const double h = c.sy(kPadPx * 2.0 + kHeaderPx
                          + kQuickActionCount * kRowHPx
                          + (kQuickActionCount - 1) * kRowGapPx
                          + 2.0 * kGroupGapPx);

    // Centred over the image, like the curve library and for the same reason:
    // it is dismissed the moment something is picked, so it can take the middle
    // of the frame while it is up.
    _rect.x1 = c.rod.x1 + (c.rodWidth()  - w) * 0.5;
    _rect.x2 = _rect.x1 + w;
    _rect.y2 = c.rod.y1 + (c.rodHeight() + h) * 0.5;
    _rect.y1 = _rect.y2 - h;
}

OfxRectD QuickWidget::rowRect(const OverlayContext& c, int action) const
{
    // Group gaps accumulate: a row in the second group is pushed down by one,
    // a row in the third by two.
    const double y = c.sy(kHeaderPx)
                   + action * c.sy(kRowHPx + kRowGapPx)
                   + GroupOf(action) * c.sy(kGroupGapPx);

    OfxRectD r;
    r.x1 = _rect.x1 + c.sx(kPadPx);
    r.x2 = _rect.x2 - c.sx(kPadPx);
    r.y2 = _rect.y2 - c.sy(kPadPx) - y;
    r.y1 = r.y2 - c.sy(kRowHPx);
    return r;
}

void QuickWidget::draw(const OverlayContext& c)
{
    layout(c);
    Panel(c, _rect);

    SetColour(c, colours::kText);
    Text(c, "QUICK CONTROL", _rect.x1 + c.sx(kPadPx), _rect.y2 - c.sy(6.0),
         kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentTop);

    // Which stage the first six act on. Without it the panel would be the one
    // place in the overlay where "the stage" is not right next to the answer.
    SetColour(c, colours::kTextDim);
    Text(c, std::string("Stage ") + std::to_string(c.activeStage + 1),
         _rect.x2 - c.sx(kPadPx + 46.0), _rect.y2 - c.sy(6.0),
         kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentTop);

    for (int a = 0; a < kQuickActionCount; ++a)
    {
        // Never lit: these are one-shot commands, not states, so a filled row
        // would be claiming something is switched on.
        Button(c, rowRect(c, a), QuickActionLabel(a), false, GroupTint(a));
    }
}

bool QuickWidget::penDown(const OverlayContext& c, const OfxPointD& p)
{
    if (!Contains(_rect, p)) return false;

    for (int a = 0; a < kQuickActionCount; ++a)
    {
        if (!Contains(rowRect(c, a), p)) continue;

        // Close first, and outside any edit block.
        //
        // The trigger below is dispatched synchronously by the host, and the
        // handlers it reaches open their own edit blocks and can raise a modal
        // dialog -- Flatten warns, Paste reports an empty clipboard. A modal
        // Win32 dialog pumps its own message loop, so running one inside a
        // paramEditBegin leaves the host holding an open edit while the user is
        // free to click elsewhere and re-enter this interact. See the LOAD
        // button in OverlayInteract::toolbarHit, which had the same problem.
        c.effect->fetchBooleanParam(kParamShowQuick)->setValue(false);

        // The choice parameter carries the argument. Writing it here means the
        // Inspector's dropdown also lands on whatever was last run from the
        // overlay, which is the honest reading of a single shared setting.
        c.effect->fetchChoiceParam(kParamQuickAction)->setValue(a);

        OFX::BooleanParam* t = c.effect->fetchBooleanParam(kParamQuickFromOverlay);
        bool v = false;
        t->getValue(v);
        t->setValue(!v);
        return true;
    }

    return true;   // clicks inside the panel never fall through to the image
}

bool QuickWidget::penMotion(const OverlayContext& /*c*/, const OfxPointD& /*p*/) { return false; }
bool QuickWidget::penUp    (const OverlayContext& /*c*/, const OfxPointD& /*p*/) { return false; }

} // namespace mtx
