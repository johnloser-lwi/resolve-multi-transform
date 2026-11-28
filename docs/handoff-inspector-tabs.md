# Handoff: tabs in the Resolve Inspector for an OFX plugin

**For:** whoever adds Inspector tabs to another of our OFX plugins (TextAnimator, or anything
with too many parameters for one scrolling list).
**From:** the Multi Transform work, November 2026. Implemented in `resolve-multi-transform`,
branch `inspector-tabs` (from commit `6d7010a`). Verified on macOS, DaVinci Resolve Studio 21.2.
**Read time:** 20 minutes. **Implementation time:** an afternoon, most of it moving parameters
into sections.

This document assumes no knowledge of the Multi Transform conversation. Everything needed is
here. Nothing in it is documented by Blackmagic: it was worked out from Resolve's own binary, its
logs, its crash dumps and a shipping third-party plugin (AnimizoFX by Clipizo) that uses the same
mechanism. Treat the numbers as facts about Resolve 21.2 and re-check them if a later Resolve
misbehaves (section 9 says how).

---

## 1. What you get

```
Active Stage   [Stage 1      v]                     <- pinned: always visible
               ( Save Preset to File... )
               ( Load Preset from File... )
 [Stage] [Base] [ A ] [ B ] [Easing]                <- tab row 1
 [Render][Overlay][Quick][Presets]                  <- tab row 2
 ...only the selected tab's controls...
```

- **Edit / Color page:** outlined, rounded buttons; the selected one has a darker fill and an
  orange underline. Styled by us, from images shipped in the bundle.
- **Fusion page:** Fusion's own native bordered buttons, selection highlighted in Fusion's grey.
  Same parameter, drawn by Fusion; we deliberately let it.
- **Any other host:** one ordinary **Section** dropdown with every tab in it.
- Tabs are navigation only: not saved with the project, not in undo, no re-render.

## 2. Why not OFX pages

The only tab mechanism in OFX 1.4 is multiple `PageParam`s. Resolve does not draw them: the host
reports `kOfxParamHostPropMaxPages = 0` and flattens every page into one list. (Measured: a
second page called "Render" was simply appended to the end of the list.) Do not spend time here.

What Resolve has instead is an **undocumented parameter type, `"OfxParamTypeButtonRow"`** -- the
row of exclusive buttons its own effects use (the Tracker's, for one). Its name, its widget
(`UiOFXPluginParamButtonRowWidget`) and its assertions are all strings in the Resolve binary.

## 3. The ButtonRow contract

Define it through the raw parameter suite -- the C++ support library has no class for an unknown
type: `gParamSuite->paramDefine(paramSet, "OfxParamTypeButtonRow", name, &props)`, then set:

| Property | Value | Notes |
|---|---|---|
| `kOfxPropLabel` | `""` | No label column; the row is the control. |
| `kOfxParamPropScriptName`, `kOfxParamPropHint` | as usual | |
| `kOfxParamPropChoiceEnum` | one **id** per button | What the value stores. Never shown; never change one. |
| `kOfxParamPropChoiceOption` | one **label** per button | Same count as the enums (asserted). |
| `"OfxParamPropChoiceIcon"` | **exactly 8 strings per button** | See below. Not in the SDK headers; declare the name yourself. |
| `kOfxParamPropDefault` | a **string**: an enum id, or `""` | `""` = nothing selected. Set with `propSetString`. |
| `kOfxParamPropAnimates`, `CanUndo`, `Persistant`, `EvaluateOnChange` | all `0` | Navigation, not a value. This is what AnimizoFX does too. |

**The value is a string** (the selected button's id), not an index. Read it with
`paramGetValue(handle, const char**)`; write with `paramSetValue(handle, "id")`. Writing `""`
deselects every button, which works and is how one row clears the other.

**Only define it in Resolve.** Gate on `hostName == "DaVinciResolve" || "DaVinciResolveLite"`
(AnimizoFX uses exactly this check). Everywhere else define an ordinary Choice with the same
labels. Note the Fusion *page inside Resolve* also reports `DaVinciResolve` and gets the button
row -- that is fine, see section 5. (Standalone Fusion Studio is untested.)

### The 8 icons per button -- crash first

Resolve asserts, when it **creates an instance** (i.e. on project load):

```
Assertion failed: ((size == optionProp->getDimension()) &&
  ((size * OFX::s_ButtonRowNumIcons) == iconProp->getDimension())),
  function ResolveOFXButtonRowInstance, file OFXHostParamInstance.cpp
```

`s_ButtonRowNumIcons` is 8. Any other count -- including **no icons at all** -- does not reject
the parameter; it **aborts Resolve**, every time a project containing the effect is opened. If
you get this wrong, test in a throwaway project, and keep a known-good build to reinstall.

## 4. The Edit page: how the 8 strings are used (the hard part)

The 8 strings are **image paths**, one per button state. Resolve builds each button's style sheet
from a fixed template (verbatim from the binary; state order confirmed from the code that fills
it):

```
QPushButton{min-width: 26px;max-width: 26px;min-height: 26px;max-height: 26px;
  image-position: center center;border-style: none;border-width: 0px;border-image: none;
  margin: 0px;padding: 0px;background-color: transparent;}
QPushButton{image: url(%1);}                     <- icon 0  normal
QPushButton:hover{image: url(%1);}               <- icon 1
QPushButton:pressed{image: url(%1);}             <- icon 2
QPushButton:checked{image: url(%1);}             <- icon 3
QPushButton:checked:hover{image: url(%1);}       <- icon 4
QPushButton:checked:pressed{image: url(%1);}     <- icon 5
QPushButton:disabled{image: url(%1);}            <- icon 6
QPushButton:disabled:checked{image: url(%1);}    <- icon 7
```

`%1` is filled with `QString::arg`; **empty strings are skipped** (that rule is not emitted).
The button's own text is never set from `ChoiceOption` on this page.

So a plain image path gives a 26 px icon button with no text. To get a **text button of your own
size**, each string *closes the url itself*, adds declarations, and **ends by reopening
`image:url("...face..."`** so that Resolve's own `);}` completes it:

```
QPushButton:checked{image: url("/…/checked.png");min-width:56px;max-width:56px;
  min-height:22px;max-height:22px;border-radius:0px;color:#f4f4f4;image:url("/…/checked.png");}
                              ^ from our string                       ^ closed by Resolve
```

Rules that cost a round trip each to learn:

1. **The ending must balance.** Our first version ended `...;qproperty-text:"Stage";` and left
   Resolve's `)` stray. Qt does **not** skip a bad declaration here -- it rejects the **whole
   sheet**. Symptom: blank default buttons, and in Resolve's log
   `Could not parse widget stylesheet`, once per button. Always end with `image:url("<path>"`.
2. **The label is `qproperty-text:"Label";` and goes only in a rule with no pseudo-state** (the
   normal rule). Qt applies `qproperty-*` once, when the widget is polished, and only from such a
   rule. Putting it in all 8 rules showed no text at all.
3. **Set an explicit size** (`min/max-width/height`), or the base rule's 26 px square wins.
4. **Set the text colour per state.** Nothing about the host's default can be relied on.
5. **Use real image files**, absolute paths, forward slashes, quoted. We ship plain face PNGs in
   the bundle (section 7) and get the path from `kOfxPluginPropFilePath`. (Whether an empty
   `url("")` would also work was never isolated -- the build that tried it also had the bad
   ending. Do not depend on it.)

## 5. The Fusion page: same strings, different widget

Fusion's page does **not** describe the plugin again -- it reuses the Edit page's description,
so it gets the same 8 strings, and nothing at describe time tells the two apart (every host
property we logged was identical). But it draws the row as Fusion's native **MultiButton**
(`Fusion::CLSID_InputControl_MultiButton`, in `Libraries/Fusion/Plugins/openfx.plugin`), and it
reads **icon `i*8` and icon `i*8+1` of each button as images**. Our style-sheet strings are broken
images there, and the buttons came out as bare text with no frame.

Fusion skips an icon whose string is empty, and then draws its ordinary bordered buttons with
the label from `ChoiceOption`. So:

- **Icons 0 and 1 (normal, hover) are left empty.** Fusion draws native buttons; the Edit page
  skips those two rules.
- **Icon 2 (pressed) carries the normal and hover rules for the Edit page**: it closes its own
  rule with `}` and *opens new rules* after it:

```
icon 2 = "<pressed.png>");<decls(2)>}
         QPushButton{<decls(0)>qproperty-text:"Stage";image:url("<normal.png>");}
         QPushButton:hover{<decls(1)>image:url("<hover.png>"          <- Resolve adds );}
icons 3..7 = "<face>");<decls(k)>image:url("<face>"
```

`decls(k)` is the size plus that state's text colour. This is `ButtonRowIcons()` in
[`src/ButtonRow.h`](../src/ButtonRow.h). Verified: Fusion shows native buttons, the Edit page
keeps its styled ones, and the log has no parse errors.

## 6. Making tabs out of it

### Defining

- Put the **pinned** controls (anything that must show on every tab) first, then the tab
  row(s), then everything else. **Resolve lays the Inspector out in definition order.**
- Nine 56 px buttons do not fit one row of the Inspector (about 330 px wide; Resolve leaves
  roughly 10 px between buttons). Multi Transform uses **two rows**: the first starts on its first
  tab, the second starts on `""` (nothing selected).
- `DefineTabBar()` in `ButtonRow.h` does all of this, and in other hosts collapses every row into
  one Choice whose value is the section index directly.

### Switching -- `changedParam`

```cpp
if (p_ParamName == kParamTabsMain || p_ParamName == kParamTabsMore)
{
    int section = -1;
    if (!mtx::TabRowIsButtons(this, kParamTabsMain))            // fallback Choice
        section = mtx::GetTabRow(this, kParamTabsMain, kTabsAll);
    else if (p_ParamName == kParamTabsMain)
    {
        section = mtx::GetTabRow(this, kParamTabsMain, kTabsMain);
        if (section >= 0) mtx::ClearTabRow(this, kParamTabsMore);
    }
    else
    {
        const int i = mtx::GetTabRow(this, kParamTabsMore, kTabsMore);
        if (i >= 0) { section = kSecFirstMore + i; mtx::ClearTabRow(this, kParamTabsMain); }
    }
    if (section >= 0 && section != _section) { _section = section; syncVisibility(); }
    return;
}
```

Clearing the other row fires `changedParam` for it **synchronously**, with an empty value;
`GetTabRow` returns -1 for that and the handler ignores it.

### Showing one section

- Hide/show with `setIsSecret` on every parameter. Keep a `_section` member.
- **Describe everything visible.** Fusion builds Inspector controls only for descriptors that are
  not secret at describe time; a parameter described secret can never be shown there later. Hide
  at runtime, from the constructor's first sync onward.
- **Build the per-section lists in the constructor** with `getParam(name)`. The OFX support
  library caches handles in an unlocked map; a first fetch by name during a render can crash
  Fusion (see `handoff-fusion-empty-source.md`, addendum 9). After construction, every lookup is a
  read.
- **Combine with existing visibility rules in one place.** A control with its own rule (e.g. Scale
  Y shown only while Link Scale is off) becomes `hidden = !onThisTab || ownRule`. Two separate
  passes overwrite each other.
- `setIsSecret` is a host round trip each. Re-apply the per-section lists only when the section
  actually changed (`_appliedSection`).
- **Read the current tab in the constructor.** Resolve rebuilds the instance every time a clip is
  selected, and within a session it remembers the row's value even though it is not persistent.
  Starting on the first tab regardless puts the content out of step with the lit button.

### No collapsible groups -- at all

Resolve draws a thin separator line for every `GroupParam` **even while it is hidden**. With
sections hidden per tab, the hidden groups left bands of stray lines above or below whatever tab
was showing. Once a tab is the section, a group header inside it is redundant anyway. Multi
Transform has no groups left; every control is a plain row, ordered by definition.

## 7. The face images

[`scripts/make-tab-faces.py`](../scripts/make-tab-faces.py) (standard library only) writes four
PNGs to `packaging/tab-faces/`: `normal`, `hover`, `checked`, `disabled`. Each is a rounded,
outlined rectangle at the button's size ×3 (168 × 66 for 56 × 22), anti-aliased by supersampling;
`checked` adds a 2 pt orange (`#F28C28`) bar along the bottom. Colours, radius, outline and size
are constants at the top. The PNGs are committed, so the build needs no Python.

CMake copies them into the bundle and relinks when one changes (without the `LINK_DEPENDS`, an
image-only edit never reaches the bundle):

```cmake
file(GLOB MTX_TAB_FACES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/packaging/tab-faces/*.png")
set_property(TARGET MultiTransform APPEND PROPERTY LINK_DEPENDS ${MTX_TAB_FACES})
# in the POST_BUILD bundle step, before signing:
COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/packaging/tab-faces" "${BUNDLE_DIR}/Contents/Resources/tab-faces"
```

At describe time, `TabFacesDir()` turns `kOfxPluginPropFilePath` into
`<bundle>/Contents/Resources/tab-faces`. If the host gives no path, the tab bar falls back to the
Choice rather than draw broken buttons. On macOS, sign *after* copying (the bundle signature
covers Resources).

## 8. Implementation checklist for another plugin

1. Copy [`src/ButtonRow.h`](../src/ButtonRow.h), [`scripts/make-tab-faces.py`](../scripts/make-tab-faces.py)
   and `packaging/tab-faces/` from the `inspector-tabs` branch. They have no Multi Transform dependencies.
   Rename the namespace if you like.
2. Add the CMake snippet above.
3. Decide the sections and their order; list each tab as `{ id, label }`. Ids are stored -- pick
   them once.
4. In `describeInContext`: pinned controls, then `DefineTabBar(...)`, then every other parameter
   **ungrouped**, in display order. Remove `GroupParam`s.
5. Add `_section`, `_appliedSection` and per-section `std::vector<OFX::Param*>` lists; fill the
   lists in the constructor; read the current tab there too.
6. Add the `changedParam` block from section 6 and one visibility function that ANDs the tab with
   any existing per-parameter rules.
7. Build, install, and check section 9 -- both the Edit page **and** a Fusion comp containing the
   node.

## 9. Verifying, and debugging if Resolve changes

**Logs:** `~/Library/Application Support/Blackmagic Design/DaVinci Resolve/logs/ResolveDebug.txt`.
- `grep -c "Could not parse widget stylesheet"` must be **0**. Non-zero means the Edit-page
  strings are unbalanced.
- A crash on project load writes `==========[CRASH DUMP]==========` there, with the failed
  assertion just above it. The icon-count assertion in section 3 is the one to expect.

**What each failure looked like:**

| Symptom | Cause |
|---|---|
| Resolve aborts while loading the project | icon count is not 8 × buttons |
| An empty band one row tall above the controls; clicks still register | strings inside `url()` as bogus file names (no splice) -- buttons are 26 px blank squares |
| Buttons sized right, Resolve's default border, hatched selection, no text | style sheet rejected -- check the parse-error count |
| Text missing but buttons styled | `qproperty-text` not in the no-pseudo-state rule |
| Fusion: bare text, no frame | icons 0/1 not empty |
| Stray thin lines on some tabs | a hidden `GroupParam` somewhere |

**Testing with UI automation:** an accessibility "press" on a tab button flips its highlight but
**does not fire `changedParam`** -- Resolve only notifies on a real mouse click. Use a real
(raw) click, or have a person click. Notifications can also arrive a moment late. A temporary log
line in `changedParam` is the reliable way to see what the plugin received.

**Resolve's scripting** (`resolve.GetProjectManager()`, `SaveProject()`, `resolve.Quit()`,
`LoadProject()`, `OpenPage("fusion")`) makes the quit / install / reopen loop scriptable, and a
Fusion comp's tools can be found with `item.GetFusionCompByIndex(i).GetToolList()` and their
`TOOLS_RegID`.

**If a future Resolve breaks this,** the facts above came from:
- `strings` on `DaVinci Resolve.app/Contents/MacOS/Resolve` for type and property names, the
  style-sheet template and assertion texts;
- locating the template string's address and scanning the arm64 code for the ADRP + ADD pair that
  loads it, then disassembling that function with `lldb -b -o "disassemble -s <addr> -c <n>"`
  (it is the one calling `QString::arg` once per state);
- the same on `Libraries/Fusion/Plugins/openfx.plugin` for the Fusion side;
- AnimizoFX (`/Library/OFX/Plugins/AnimizoFX.ofx.bundle`), whose code builds the same strings --
  its colour table, face images and string layout were the reference.

## 10. Open items

- **Windows is not built or tested.** The code is shared, and `TabFacesDir` converts backslashes,
  but the bundle path will contain spaces (`C:/Program Files/Common Files/OFX/Plugins/...`). It
  is quoted inside `url("...")`, which should be fine; check it.
- **The Edit-page hover state was never seen** -- UI automation could not hover. It comes from the
  rule injected through icon 2.
- **Fusion uses its own highlight colour**, not the orange. It draws its own buttons; giving it
  images (icons 0/1) would replace the labels, and the faces carry no text.
- **Standalone Fusion Studio** is untested; it should report a different host name and get the
  Choice fallback.
