# Multi Transform — OFX plugin for DaVinci Resolve

A multi-stage animated transform node with staggered per-stage timing, built-in motion blur,
and an on-screen viewer overlay for editing timing and easing curves directly.

**Status: Phase 0 (scaffold).** The plugin currently builds, loads, and renders a pass-through
image. Its real job right now is the host capability probe. The transform, animation engine,
GPU kernels and overlay UI land in later phases — see the plan in
`~/.claude/plans/https-www-youtube-com-watch-v-zeu9pdfinx-jolly-sloth.md`.

## Requirements

- **DaVinci Resolve Studio** — the free edition does not load third-party OFX plugins.
- Visual Studio 2022 or 2026 with the C++ workload.
- CUDA Toolkit (for Phase 2 onward).

## Build

```powershell
.\scripts\build.ps1 -Config Release
```

This uses the CMake bundled with Visual Studio rather than whatever is on `PATH` — a
standalone CMake older than the installed VS toolset will not recognise its generator.

Output: `build\Release\MultiTransform.ofx.bundle` (the bundle path is per-config, so a Debug
build never silently overwrites a Release one).

## Install

### Normal install — `C:\Program Files\Common Files\OFX\Plugins`

Double-click **`scripts\Install-MultiTransform.cmd`**.

It builds as you, then prompts once via UAC for the copy into Program Files. Building happens
*before* elevation on purpose: building as administrator would leave `build\` owned by the
admin account and break every later non-elevated build.

Equivalent from a shell (elevates itself — no need for an admin prompt first):

```powershell
.\scripts\install.ps1 -Build
```

Useful switches:

| Switch | Effect |
|---|---|
| `-Build` | Build before installing |
| `-Config Debug` | Install the Debug bundle instead of Release |
| `-Force` | Close DaVinci Resolve automatically if it has the plugin locked |
| `-Destination <path>` | Install somewhere else |

To remove it:

```powershell
.\scripts\uninstall.ps1
```

**On Resolve being open:** you do not have to close it for a *first* install — Resolve only
locks plugins it has already loaded. If you are replacing a build Resolve has loaded, the
installer says so and asks you to close it. Either way Resolve must be **fully restarted**
afterwards, since it scans for OFX plugins only at launch.

### Alternative — no admin at all

```powershell
.\scripts\install-dev.ps1
```

Copies to `%LOCALAPPDATA%\OFX\Plugins` and relies on Resolve reading `OFX_PLUGIN_PATH` at
launch. Set it once:

```powershell
[Environment]::SetEnvironmentVariable('OFX_PLUGIN_PATH', "$env:LOCALAPPDATA\OFX\Plugins", 'User')
```

Be aware this route has a failure mode the Program Files route does not: a newly-set user
environment variable is often not inherited by apps launched from an already-running Explorer,
so the plugin can fail to appear even when everything is correct. Prefer the normal install
unless you specifically need to avoid elevation.

## How stages and stagger work

A **stage** is a complete transform (scale, position, rotation, anchor) with **its own start
and end frame** and its own easing curve. Duration is derived from the two and shown greyed
out — you never set it directly.

Stages **combine** — they are multiplied together, not chained. Two stages each animating scale
1.0 → 1.5 produce 2.25x overall, not 1.5x. Because of that, keeping one property per stage is
usually the clearest way to work.

There is no separate "stagger" control, because none is needed: **staggering is just giving
stages different start frames.** Stage 1 running 100 → 120 alongside Stage 2 running 106 → 126
*is* a six-frame stagger. Stages can also differ in length, so a short snap can overlap a long
drift — something a single shared duration could never express.

### One stage at a time

**Only the selected stage's controls appear in the Inspector.** Use the **Active Stage**
dropdown at the top, or click a stage tab or timeline lane in the viewer overlay — all three
drive the same setting. Showing four stages' worth of controls at once was unreadable, and
Stage Count still governs how many stages exist.

Within the visible stage, controls are separated by plain text headings rather than collapsible
groups:

```
—  STAGE 2 : TIMING  —
    Enabled, Set Start / Set End, Start Frame, End Frame, Duration, Anchor
—  FROM (START)  —
    Scale, Position, Rotation, Opacity
—  TO (END)  —
    Scale, Position, Rotation, Opacity
—  EASING  —
    Easing, Ease In, Ease Out, Anticipation, Overshoot
```

Reading a pose is one block top to bottom, rather than picking every other row out of an
interleaved `Scale From / Scale To / Position From / Position To` list.

**Anchor deliberately sits in the timing section**, not in either pose block — a pivot that
moved between the start and end of a move would make the motion very hard to reason about.

### Fades

Each panel has an **Opacity** percentage, so a fade is just another animated property:

- **Fade in** — From Opacity `0`, To Opacity `100`
- **Fade out** — From Opacity `100`, To Opacity `0`

Opacity runs on the stage's own start/end frames and its own easing curve, so it can be
staggered against the movement exactly like anything else. A common trick: put the fade on one
stage and the push-in on another, starting the fade a few frames earlier so the image is already
partly visible as it begins to move.

Opacity composes the same way transforms do — two stages each at 50% leave 25%. It also blurs
correctly: with motion blur on, a fade mid-shutter is sampled per shutter step rather than
frozen at the frame-centre value.

### Setting timing

Each stage has two buttons:

- **Set Start to Playhead** — park the playhead where the stage should begin, click.
- **Set End to Playhead** — park the playhead where it should finish, click.

Start Frame and End Frame can also be typed, and **Duration** updates automatically.

### Worked example — a push-in where the scale leads the drift

1. Stage Count `2`.
2. **Stage 1** — Scale From `1.0`, Scale To `1.15`, Easing *Ease Out*. Park the playhead at the
   start of the move, *Set Start*; move forward 20 frames, *Set End*.
3. **Stage 2** — Position To `0.04, 0.0`, Easing *Smooth*. Park the playhead 6 frames after
   Stage 1's start, *Set Start*; then 20 frames further, *Set End*.

The scale starts immediately and eases out; the sideways drift starts six frames later and
overlaps the tail of the scale. Slide Stage 2's start between 3 and 10 frames after Stage 1's
to feel the difference — that overlap is the whole effect.

## Easing

Each stage's curve is controlled by four amounts, all of which stay editable at all times:

| Control | Effect |
|---|---|
| **Ease In** | Damping at the start. 0 leaves at full speed, 100 creeps away gradually. |
| **Ease Out** | Damping at the end. 0 stops dead, 100 glides to a halt. |
| **Anticipation** | Pulls back before moving, like a crouch before a jump. **Negative steepens the start.** |
| **Overshoot** | Travels past the target and settles back (~80 = classic springy). **Negative undershoots.** |

### Getting a steeper curve

Two ways, and both are reachable by dragging the handles in the overlay:

- **Ease In and Ease Out both at 100** gives the steepest S-curve possible — very slow at each
  end, very fast through the middle.
- **Negative Anticipation or Overshoot** pushes a handle past the opposite rail, which steepens
  that end of the curve rather than adding a bounce.

Horizontally the handles are clamped to the span between the two keyframes. That is not a
limitation of this plugin: outside that range the bezier folds back on itself and a single time
would map to two values, so there would be no well-defined answer. Every curve editor,
Resolve's included, constrains this. Vertically there is no such restriction.

The **Easing** dropdown is a starting point, not a lock: picking *Smooth* or *Ease Out* fills in
these four values, and adjusting any of them switches the dropdown to *Custom*. So a preset is
always something you can then tune.

The amounts map exactly onto a CSS `cubic-bezier`, so *Smooth* is precisely `ease-in-out`,
and Ease In / Ease Out are the x positions of the two control handles.

> A visual, draggable curve editor is coming in Phase 5. It has to live in the **viewer
> overlay** rather than the Inspector, because Resolve's OFX host supports neither parametric
> (curve) parameters nor custom parameter-panel widgets — both measured, see `probe.log`.

## Motion blur

Enable under **Motion Blur**. The blur is *analytic*: every shutter sample is a different
transform of the same source frame, so no neighbouring frames are fetched. This is only
possible because the plugin owns its animation and can evaluate the transform at fractional
times — the payoff for not depending on host keyframes.

| Control | Effect |
|---|---|
| **Shutter Angle** | How long the shutter is open. 180 is the film convention; 0 disables. |
| **Shutter Phase** | Shifts the shutter relative to the frame. 0 centres it. |
| **Samples** | Shutter samples. With Adaptive on, an upper bound rather than a fixed cost. |
| **Adaptive Samples** | Scales sample count with actual on-screen motion. |

Leave **Adaptive Samples** on. It measures how far the image corners travel across the shutter
and allocates roughly one sample per two pixels, so a static frame costs one sample regardless
of the slider while a whip pan still gets the full count.

Turning motion blur off, setting Shutter Angle to 0, or setting Samples to 1 all collapse to
*exactly* the un-blurred render path — bit-identical, not merely similar. There is a test for it.

### Timing is on the timeline, not the clip

Resolve passes plugins timeline-absolute frame numbers and reports a useless clip frame range,
so the plugin genuinely cannot tell where a clip starts. That is why the timing is captured
from the playhead. If nothing appears to animate, this is almost always why: the stage's frames
are still at their defaults (0 and 24) while your timeline starts at hour one, so every visible
frame is past the end and sits at the final value. Click *Set Start* / *Set End* and it will
come to life.

One consequence worth knowing: because frames are timeline-absolute, **moving the clip along
the timeline does not carry the animation with it** — you would re-click the buttons. If that
becomes annoying in practice, the timing can be made relative to a single anchor instead.

## The viewer overlay

**Resolve does not show OFX overlays by default.** After applying the effect, use the
on-screen-control dropdown underneath the Viewer and choose **Open FX Overlay**. Works on the
Edit, Fusion and Color pages.

The overlay has four parts:

**Stage tabs** (above the timeline) — click `1`–`4` to choose which stage the gizmo and curve
editor act on. **FROM / TO** picks which end of that stage the gizmo poses.

**Transform gizmo** (on the image) — the outline shows the stage's pose. Drag inside it to
move, drag a corner to scale, drag the arm above the top edge to rotate, drag the circled
crosshair to move the anchor point. The gizmo is cyan for FROM and orange for TO.

**Stage timing lanes** (bottom) — one lane per stage, drawn against a shared frame ruler with
the playhead marked. Drag a bar's left or right edge to change its start or end frame; drag its
middle to slide the whole stage without changing its length. Clicking a lane also selects that
stage. This is where staggering becomes obvious: the offsets between stages are visible as
offsets between bars.

**Curve editor** (top right) — the actual easing curve for the active stage, plotted with the
same evaluator the renderer uses, so what you see is what it does. Drag the two control handles
to shape it. The faint diagonal is linear for reference, the horizontal lines mark 0 and 1
(anything beyond them is anticipation or overshoot), and the yellow dot shows where the current
frame sits on the curve.

Everything written by the overlay lands in the ordinary parameters, so the Inspector and the
overlay always agree, and every edit is undoable in the normal way.

### Known limitation

The panels are positioned relative to the **image**, not the viewport, so if you zoom deep into
the picture they can drift off screen. OFX 1.4 removed `kOfxInteractPropViewportSize` and
provides no other way for a plugin to learn the viewport bounds, so anchoring to the image is
the only option available. Zoom to fit to bring them back.

## Host capability probe

The plugin writes what the host actually supports to:

```
%LOCALAPPDATA%\MultiTransform\probe.log
```

This exists because published documentation on Resolve's OFX host is thin and partly
contradictory. The log records the host version, whether the DrawSuite is available, whether
custom interacts and parametric parameters are supported, which GPU render paths are offered,
and — on first render — the clip frame range and the time value the host passes, which settles
how the animation engine maps parameter frames onto real time.

## Layout

```
src/                 plugin source
  render/            CPU / CUDA / OpenCL backends      (Phase 2)
  interact/          overlay widgets                   (Phase 4-5)
tests/               unit tests for the animation engine
third_party/openfx/  vendored Blackmagic OFX SDK
packaging/           bundle Info.plist
scripts/             build and install
```

### On the vendored SDK

`third_party/openfx` is copied from Resolve's own SDK at
`C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\OpenFX` (note: under
`ProgramData`, not `Program Files`). It is vendored so builds are reproducible and do not
depend on a Resolve install path.

`Support/Library/ofxsHWNDInteract.cpp` is **excluded from the build**. It references
`gHWNDInteractSuite` and `kOfxHWndInteract*` symbols that are declared nowhere in the SDK, so
it does not compile. Blackmagic's own Makefile omits it too. It is a vestigial Foundry
extension, not a Resolve feature — do not try to revive it.
