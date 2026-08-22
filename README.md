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

Each stage animates between two poses, **A** (start) and **B** (end). They were called From and
To until the labels proved awkward to say and to read — "copy from to to" — and lopsided as a
pair. The rename is **cosmetic only**: parameter names, preset files and project data still say
`From` and `To` underneath, so nothing saved before the change had to be migrated.

Stages **combine** — they are multiplied together, not chained. Two stages each animating scale
1.0 → 1.5 produce 2.25x overall, not 1.5x. Because of that, keeping one property per stage is
usually the clearest way to work.

There is no separate "stagger" control, because none is needed: **staggering is just giving
stages different start frames.** Stage 1 running 100 → 120 alongside Stage 2 running 106 → 126
*is* a six-frame stagger. Stages can also differ in length, so a short snap can overlap a long
drift — something a single shared duration could never express.

### Staggering channels inside one stage

Each stage's Timing section also carries six **timing offsets** — Position, Scale, Rotation,
Tilt, Swivel and Opacity. An offset slides that one channel's copy of the stage's window:
**shift, not squeeze** — offset +5 runs the channel from start+5 to end+5, same duration, same
easing curve, just later. Negative leads. The channel holds its A value until its own window
opens, exactly as a stage holds before its start frame.

This is the in-stage version of what stages do for whole properties. A move whose fade trails
it by six frames used to cost a second stage of the four; now it is one stage with Opacity
Offset 6. Stages stay for staggering *different curves and paths*; offsets stagger channels
that share one.

The units follow the stage: frames normally, percentages under a Stretch anchor — and Fit to
Clip rescales offsets together with the start and end, so a fade that trailed its move by a
tenth of the clip still trails by a tenth on the new clip. Presets saved before offsets
existed load with all six at zero, which is exactly the old single-clock behaviour.

Two derived things deliberately stay on the stage's own clock: the timeline lane's velocity
shading and red peak tick, and the two Sync Acceleration buttons. With channels staggered
there is no longer a single "fastest moment", so they read the stage's base timing. The
motion path does follow the Position offset — the drawn route pins to the frames the image
really travels.

### One stage at a time

**Only the selected stage's controls appear in the Inspector.** Use the **Active Stage**
dropdown at the top, or click a stage tab or timeline lane in the viewer overlay — all three
drive the same setting. Showing four stages' worth of controls at once was unreadable, and
Stage Count still governs how many stages exist.

Every section is a **collapsible group, collapsed by default**, so the panel opens as a short
list of headers:

```
Stage Count, Active Stage        ← always visible
▸ Viewer Overlay
▸ Base Transform
▸ Stage 2 — Timing               ← only the active stage's five sections appear
▸ Stage 2 — A (start)
▸ Stage 2 — B (end)
▸ Stage 2 — Motion Path
▸ Stage 2 — Easing
▸ Presets
▸ Motion Blur
▸ Sampling
```

Reading a pose is still one block top to bottom, rather than picking every other row out of an
interleaved `A Scale / B Scale / A Position / B Position` list.

An earlier version used plain text headings instead of groups, to avoid a wall of disclosure
arrows. Two things ended that. The panel roughly tripled as base transforms, presets, motion
paths and bounce controls arrived — one active stage reached about **70 rows**. And the text
headings turned out to render in Fusion but *not* in Resolve: the heading text sat in the
parameter's **value** while Resolve draws only the **label**, so the separators were invisible in
the very host this is written for. A group header is drawn by the host itself and cannot go
missing that way.

**There is no tab support to be had.** The only tab mechanism in OFX is multiple `PageParam`s,
and Resolve reports `maxPages = 0` (see `probe.log`) — it advertises none. Selecting a stage
therefore uses the Active Stage dropdown or the overlay's stage tabs, and the other three stages
are hidden entirely, groups and all.

**Anchor deliberately sits in the timing section**, not in either pose block — a pivot that
moved between the start and end of a move would make the motion very hard to reason about.

### Syncing a move to a beat

**Sync Acceleration to Playhead**, in the stage's Timing section, slides the stage so the
**fastest moment of the move** lands on the playhead. Start and End shift by the same amount, so
the duration, the easing and every other value are untouched — only *when* the move happens
changes.

That moment is the red tick on the stage's timeline lane, so the frame it snaps to is one you can
already see before pressing anything.

Syncing to the peak rather than to an end is the point: for anything but linear easing the two
are far apart. An Ease Out peaks almost immediately, so the stage ends up starting near the
playhead; an Ease In peaks at the very end, so it finishes near it. Same button, opposite
placement — which is what you want when matching a hit to a beat, because the beat should land on
the impact, not on the beginning of the wind-up.

Pressing it again does nothing once synced, and under a Stretch anchor the shift stays fractional
rather than rounding to whole frames, since there the values are percentages.

**Sync Acceleration by Easing** reaches the same target the other way: the stage stays exactly
where it is and the *curve* is reshaped instead. Use it when the timing is locked — a title that
must appear on a specific frame — and only the feel is free to move.

It reshapes **Ease In and Ease Out**, which are what decide where the peak sits: weight the curve
towards Ease In and it accelerates late, towards Ease Out and it peaks early.

**Hitting the target is the only objective.** Both amounts are free to go anywhere in 0–100,
including changing how much easing there is overall — a curve that stayed politely as soft as it
was but missed the playhead would be no use. A linear stage is reshaped rather than refused: "no
easing yet" is a starting point, not an obstacle. It never warns; it just lands.

Anticipation, overshoot and bounce are left alone. Those carry the move's character, and none of
them decide where the peak sits.

Two amounts fitted to one number leaves a whole family of answers, so among the settings that hit
the target it picks the one **closest to the curve you already had**. A curve needing a nudge gets
nudged; one needing a rebuild gets rebuilt.

The solver searches a grid rather than converging on a solution. The peak is usually monotonic in
the balance between the two amounts, but a bounce multiplies an oscillation into the curve and can
break that — anything assuming monotonicity would then settle confidently on the wrong side.

### Moving poses between the two ends

Three of the [Quick Control](#quick-control) actions, because most moves are built by matching one
end to the other and then changing only what should differ:

- **Copy A to B** — the stage holds still until you change something.
- **Copy B to A** — handy after posing the end state on screen: copy it back, then pull the
  start away from it.
- **Swap A and B** — reverse the move. Turns a fade-in into a fade-out, an intro into an
  outro.

They act on the **active stage**, the one the gizmo and the timeline lane are already editing.

All seven animated channels move together: Scale, Scale Y, Position, Rotation, Tilt, Swivel and
Opacity. Swap also trades the **motion path's two handles**, so a bent route keeps its exact
shape and simply runs the other way — the first handle is an offset from one third along the
straight line and the second from two thirds, so reversing the line maps each precisely onto the
other. Swapping twice returns you exactly where you started.

The anchor is not touched: it is shared by both ends by design. Easing is not touched either —
reversing *what* moves is a separate decision from reversing *how* it accelerates.

### The base transform — a resting pose

Above the stages sits a **— BASE TRANSFORM —** section: a static pose with the same channels a
stage has, but no timing. It answers "this layer just *sits* here, at this size" without
spending a whole stage on an A that equals its B.

It composes **innermost**, underneath everything the stages do. That ordering matters: set a
base scale of 50% and a stage that moves by `0.2` still moves by 0.2 of the *frame*, not 0.1.
The other order would silently scale down every animation you had already built.

Base opacity multiplies with the stages', so a base of 50 under a stage fading 0 → 100 ends at
50. **Reset Base Transform** returns the pose to neutral and leaves the animation alone.

### Split scale, and pseudo-3D rotation

**Link Scale X/Y** is on by default and scale behaves as it always did. Turn it off and a
**Scale Y** field appears at each end, so one axis can squash or stretch independently. On
unlinking, the current X value is copied into Y at both ends, so nothing moves until you change
something. With the axes unlinked, dragging a corner of the on-screen gizmo stretches the box
rather than only resizing it.

**Tilt (X axis)** and **Swivel (Y axis)** are pseudo-3D rotations, named the way Premiere's
Basic 3D names them. Animate `Swivel −90 → 0` for a card swinging in to face the camera.

They are **orthographic, not perspective**: an axis rotation is exactly a cosine scale, so the
image squashes rather than foreshortening, and parallel edges stay parallel. It reads as a
convincing flip in motion and is essentially free — no change to the transform type, no change
to the CUDA kernel.

Two consequences worth knowing:

- **Past 90°** the cosine goes negative and the image mirrors. That is correct — you are looking
  at the back of the card — and it is left in rather than clamped.
- **At exactly 90°** the transform collapses to zero width and the frame is simply invisible,
  which is what edge-on looks like. Scrubbing slowly through the flip should show no one-frame
  pop.

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
2. **Stage 1** — A Scale `1.0`, B Scale `1.15`, Easing *Ease Out*. Park the playhead at the
   start of the move, *Set Start*; move forward 20 frames, *Set End*.
3. **Stage 2** — B Position `0.04, 0.0`, Easing *Smooth*. Park the playhead 6 frames after
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

### Motion path

By default a stage moves in a straight line from its A position to its B position. The
**Motion Path** handles bend that route into a curve.

The practical way to use it is on screen: turn on **Open FX Overlay**, and the trajectory is
drawn over the image with two draggable handles, one tethered to each end. Drag them to shape
the arc. **Straighten Path** resets both. The two `Path Handle` values in the Inspector are the
same thing numerically, if you need precision.

Small white dots along the path mark equal *time* intervals, so they show pacing: bunched dots
mean the object is moving slowly there, spread-out dots mean fast.

**Easing controls speed along the path; the handles control its shape.** They are independent,
which is what makes them compose — a bounce on a curved path runs the object back and forth
*along the curve* rather than distorting it.

Handles are stored as offsets from the straight line, so moving From or To carries the bend
with them instead of stranding the handles. And a zero offset reproduces the straight line
**exactly**, not approximately: the control points sit at one and two thirds along the segment,
which makes the cubic the degree-elevated form of a linear interpolation. Existing animations
are bit-for-bit unaffected, and there is a test pinning that.

### Bounce

**Overshoot** gives a single smooth overshoot. It cannot give you more than that: it is the `y2`
handle of a cubic bezier, and a cubic has at most two turning points, so one overshoot and one
undershoot is the mathematical ceiling regardless of where the handle goes. Repeated rebounds
need a separate oscillation, which is what **Bounce** adds.

| Control | Effect |
|---|---|
| **Bounce** | *None*, *Spring* (settles through the target, above then below), or *Ball* (rebounds off the target, never passing it) |
| **Bounce Amount** | How far the first rebound travels, as a fraction of the whole move. **Negative flips the direction.** |
| **Bounces** | How many rebounds before it settles. Fractional values are allowed. |
| **Bounce Damping** | How fast rebounds shrink. 0 keeps them equal — mechanical; higher decays them naturally. |
| **Bounce Start** | Where in the stage the move **lands** and bouncing begins, as a % of its duration. |

**Your easing curve still shapes the approach.** Ease In, Ease Out and Anticipation all apply
normally to the part of the move before the landing — turning a bounce on does not redraw the
curve you set, it only compresses it into the time before Bounce Start.

The single exception is **Overshoot, which is suppressed while bouncing.** If the bezier also
overshot, the curve would pass the target during the approach, get pulled back down to the
target at the landing, and only then let the bounce push past a second time. That downward leg
in the middle makes the first rebound look like it goes the wrong way. Bounce owns the
overshoot behaviour; the bezier must not compete with it.

**If the bounce goes the wrong way, use a negative Bounce Amount.** The sign chooses which side
of the target the rebound leaves from — a spring undershoots before it overshoots, and a ball
rebounds off the near side rather than the far side. It is an exact mirror: nothing else about
the curve changes, including the approach.

**Bounce Start is the control to reach for when the timing feels wrong.** The easing curve is
compressed into the part of the stage *before* it, and everything after it is the bounce. So a
lower value lands the move sooner and leaves a longer bounce; a higher value keeps the move
going longer and bounces briefly at the end. The bounce always happens *after* the move arrives,
never during it.

Two presets set these up: **Spring** and **Bounce**. The amount/count/damping controls only
appear once a bounce type is selected.

Whatever you set, **the move always lands exactly on its target value.** The oscillation is
scaled by the distance still to travel, so it is mathematically zero at the end — a stage
animating scale to 1.5 finishes at exactly 1.5, never 1.497. There is a test sweeping the whole
parameter space for this.

The *Ball* model can never pass the target, by construction rather than by tuning: its
oscillator is `|cos|`, which is never negative, and it is clamped at the target so the rebound
touches exactly. *Spring* is deliberately left unclamped so it can cross.

Bounce is free at render time — easing is evaluated per shutter sample on the host, never per
pixel. Note though that a bounce genuinely moves the image faster, so adaptive motion blur will
correctly spend more samples during the rebounds.

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

## Presets

Resolve has no usable preset mechanism for third-party OFX plugins, so presets are plain JSON
files. Four buttons under **— PRESETS —**:

| Button | Does |
|---|---|
| **Save Preset to File…** | Every stage plus motion blur and sampling |
| **Save Active Stage to File…** | Just the active stage, for a library of reusable pieces |
| **Load Preset from File…** | Applies exactly as saved |
| **Load from File (Fit to Clip)…** | Same, but rescales frame timings to this clip's length |

A stage preset loads into whichever stage is active. The files are readable and diffable, and
they work in both Resolve and Fusion.

### Quick Control

Eight one-shot actions behind a single dropdown and an **Apply** button at the top of the panel:

| Action | Acts on |
|---|---|
| Copy A to B / Copy B to A / Swap A and B | the active stage's two ends |
| Copy Stage / Paste Stage | the active stage |
| Flatten to Stage 1 | the whole animation |
| Copy All Settings / Paste All Settings | the whole effect |

They were eight separate push buttons once, and that was eight full-width rows pushing the
controls actually being adjusted most of a screen down the Inspector. **There is no two-column
layout to spread them across**: the only column mechanism in OFX is page-based
(`kOfxParamPageSkipColumn`), Resolve reports `maxPages = 0`, and OFX 1.4 has no
`SameLine`/`NoNewLine` property at all. Every parameter is one full-width row, so the only way to
take less height is to have fewer parameters.

Apply is separate from the dropdown on purpose — several of these overwrite work, and picking
Flatten by accident while scrolling a list should not destroy four stages.

The same eight are on the overlay behind the **QUICK** button, so none of this needs the Inspector
at all. See [the overlay](#viewer-overlay).

### Copying a whole effect between clips

**Copy All Settings** / **Paste All Settings**, under Quick Control, move the entire node in
one press — every stage, the base transform, motion blur and sampling.

This exists because Resolve cannot copy a *single* OFX effect between clips: it is all of a
clip's effects or none of them. Without it the only route was saving a preset to a file and
loading it on the other clip.

The copy is written to `%LOCALAPPDATA%\MultiTransform\clipboard.json` rather than held in memory,
so it survives closing Resolve, works across projects, and outlives the plugin being unloaded and
reloaded. It uses the same serialisation as presets, so the payload is already covered by the
preset tests and the file is readable if it ever needs inspecting.

Paste applies the timing **exactly as copied**, without rescaling. The anchors already do the
sensible thing on a different clip — Clip Start and Clip End are relative, Stretch is
proportional, and a Timeline-anchored stage is converted to clip-relative on the way out. When
the pacing *should* be rescaled to a different length, use **Load from File (Fit to Clip)**
instead.

`Copy Stage` / `Paste Stage` sit alongside and do the same for one stage rather than the whole
effect.

### Where presets live

`Documents\MultiTransform\Presets` by default — somewhere you can browse to, copy from and back
up, which `%LOCALAPPDATA%` is not. **Set Preset Folder…** changes it and **Use Default Folder**
puts it back; neither moves or deletes anything, they only change where the dialogs open. The
current folder is shown above the buttons.

That choice is a **preference, not a parameter**. OFX parameters are stored inside the project
file, so anything kept as a parameter travels with the timeline and belongs to a single instance
of the effect. "Where my presets live" belongs to you and this machine, and has to apply to
every instance you ever create — including ones in projects that do not exist yet. So it goes in
`%LOCALAPPDATA%\MultiTransform\settings.json` instead, read fresh whenever a dialog opens. Send a
project to someone else and they keep their folder, not yours.

A corrupt preferences file falls back to defaults rather than failing, and a configured folder
that has since been deleted or unplugged falls back too rather than opening nowhere.

#### A bug worth recording

The dialogs used to open in unpredictable places, which is what prompted all of this. The cause
was not the folder setting but how it was passed: `lpstrInitialDir` has only been **advisory**
since Vista. Windows prefers the calling *executable's* last-visited-folder MRU — and that
executable is Resolve, whose MRU is shared with every other file dialog Resolve opens. So after
using any unrelated dialog in the host, ours would open there instead. Seeding the path into
`lpstrFile` takes priority over that MRU, which is the one place the folder can be stated and
actually respected. The folder picker uses `IFileDialog::SetFolder` for the same reason —
`SetDefaultFolder` is a suggestion the last-visited folder overrides.

The names say "to/from File" because a host may have preset controls of its own elsewhere in the
Inspector; these four are always the plugin's own, writing JSON you can move between machines.

**Both Load buttons load everything** — all four stages, motion blur and sampling. They run
identical code and differ only in whether frame-based Start/End values are rescaled on the way
in. The only settings never stored are the viewer ones (Active Stage, Gizmo Edits, Show Curve
Editor), which describe where you are looking rather than the look itself.

### How timing survives moving to another clip

Three of the four anchors are portable by construction, so nothing special is needed:

| Anchor | Stored as | On another clip |
|---|---|---|
| Clip Start | `0 → 20`, frames from the head | correct as-is |
| Clip End | `-18 → 0`, frames back from the tail | correct as-is |
| Stretch | `0 → 100`, percentages | correct at any length |
| Timeline | absolute frames | **meaningless elsewhere** |

Only Timeline needs handling, and it gets it at *save* time: those stages are converted to their
Clip Start equivalent. The timing shape is kept; only the "pinned to this timeline position"
intent is lost, and that could not have survived the move anyway.

**Fit to Clip** rescales frame-based timings by the ratio of clip lengths — an intro of `0 → 20`
authored on a 155-frame clip becomes `0 → 5` on a 40-frame one, preserving its pacing. It
deliberately leaves **Stretch** stages alone: those are already proportional, and rescaling them
would apply the ratio twice and break the one anchor built for exactly this. If either clip
length is unknown it says so and applies the preset unchanged, rather than scaling by a guess.

Short stages clamp to a minimum of one frame, so a six-frame punch does not collapse into an
instant cut on a much shorter clip.

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

### Timing anchors

Each stage has an **Anchor** deciding what its Start and End frames are measured from:

| Anchor | Start / End mean | Use it for |
|---|---|---|
| **Clip Start** (default) | frames from the clip's first frame | intros — survives moving the clip and trimming its head |
| **Clip End** | frames back from the **last** frame | outros — `-20 → 0` always finishes exactly on the final frame |
| **Stretch** | **percentages of the clip** | moves that should fill the same proportion of any clip |
| **Timeline** | absolute timeline frames | pinning to a timeline position, ignoring the clip |

**Stretch** scales rather than shifts: `0 → 100` always fills the whole clip, and `0 → 50` always
the first half, whatever the clip's length. Trim the clip and the move compresses to match
instead of running off the end. Under Stretch the fields relabel themselves to *Start (% of
clip)* and so on, so 50 is never mistaken for frame 50.

Anchors are **per stage**, so one effect can hold an intro and an outro at once:

```
Stage 1   Anchor: Clip Start    0 → 20     fade and push in as the clip opens
Stage 2   Anchor: Clip End    -18 → 0      fade out onto the final frame
Stage 3   Anchor: Stretch       0 → 100    slow drift across the whole clip
```

Trim either end of the clip and each stage follows what it is anchored to — the intro stays on
the head, the outro on the tail, and the drift rescales to the new length.

The end anchor references frame `length - 1`, not `length`. The clip's last visible frame *is*
`length - 1`, so anchoring to the length itself would place zero one frame past the end and an
outro would finish a few percent short on the final visible frame instead of landing exactly.

**Frame 0 is the clip's first frame** under the default anchor. Move the clip along the
timeline, or trim its head, and the animation goes with it — the timing means "this far into the
clip", not "at this timeline position".

Finding the clip's extent took measuring, because most of the obvious routes report nothing
usable in Resolve:

| Source | Reports |
|---|---|
| `getFrameRange` (src and dst) | `[0, 1798200]` — a sentinel, exactly 1000 minutes |
| `getUnmappedFrameRange` | `[0, 0]` — not populated |
| `timeLineGetTime` | `0` — not populated |
| **`timeLineGetBounds`** | **`[107961, 108116]` — the clip, 155 frames** |

Only the last is usable, so the plugin validates what it gets and falls back to treating times
as absolute if a host reports nothing sensible.

**Existing projects convert themselves, losslessly.** Timing used to be stored as absolute
timeline frames. Subtracting the clip's *current* start reproduces exactly where each animation
plays today — nothing shifts — and only then does it start travelling with the clip. Old and new
values are told apart by magnitude, with roughly half an hour of margin between the two, so it
cannot misfire.

The conversion is applied **on every read**, not as a one-time rewrite, and that distinction
matters. Rewriting needs the clip's start, which is not dependable at every point in an effect's
life: asked during construction, before the clip is attached, the host returns a placeholder
(`[0, 1999]`). An earlier version took that at face value, converted nothing against a start of
zero, and recorded the migration as complete — locking the effect out of ever converting, which
is worse than not having tried. Normalising on read is idempotent, because an already-relative
value is bounded by the clip length and can never look absolute, so correctness no longer depends
on when the rewrite happens. The stored values are still tidied up when the clip is attached
(`changedClip`) or any parameter changes, but that is now cosmetic rather than load-bearing.

## The viewer overlay

**Resolve does not show OFX overlays by default.** After applying the effect, use the
on-screen-control dropdown underneath the Viewer and choose **Open FX Overlay**. Works on the
Edit, Fusion and Color pages.

The overlay has four parts:

The toolbar is **two rows, grouped by kind** — *what am I looking at* above, *what am I editing*
below:

```
TIME  PATH  OPAC  CURVE  LIB               LOAD   ← panels
[1][2]⟨3⟩⟨4⟩ [-] ON   QUICK GHOST BASE  A  B      ← stage, target and actions
```

**QUICK** raises the [Quick Control](#quick-control) panel over the middle of the image: the same
eight actions as the Inspector's dropdown, one per row, grouped by what each overwrites — the two
ends of a stage, a whole stage, the whole effect. Picking one runs it and closes the panel.

Behind a button rather than laid out along the toolbar, because eight more boxes permanently over
the picture is exactly the crowding the two-row toolbar exists to avoid — and it keeps Paste and
Flatten from sitting one stray click away from the controls in constant use.

**Stage count is set from the tabs.** All four slots are always drawn: the ones past the current
count are hollow, and clicking one raises the count to it and selects it. `-` drops the last
stage. Neither goes near the Inspector's Stage Count dropdown, though that still works and stays
in step.

Two deliberate details. The slots never reflow — a row that resized as stages were added would
slide the buttons out from under the cursor exactly when they are being clicked repeatedly. And
adding a stage does **not** switch it on: Stage Count and Enabled are separate gates, enabling
changes what renders, so that stays an explicit press of **ON** right beside it. Dropping a stage
keeps its values too, so raising the count again brings the work back rather than a blank stage.

Every panel switches independently, so the overlay can be pared back to what a given edit
actually needs. A fresh effect opens as **gizmo + timeline + toolbar**; the motion path, opacity
slider, curve editor and library start hidden. The same five switches are in the Inspector's
**Viewer Overlay** group, and the state saves with the project.

A hidden panel is skipped for **clicks as well as drawing** — an invisible panel that still
swallowed clicks was a real bug once, when the curve editor ate the motion path's handles.

**Stage tabs** (above the timeline) — click `1`–`4` to choose which stage the gizmo and curve
editor act on. A stage that is switched off is dimmed, so the tabs say which stages are actually
contributing without clicking through them. **ON / OFF**, immediately right
of the tabs, enables the selected stage — raising Stage Count alone leaves the new stage
switched off, and this saves opening its Timing section every time. **A / B / BASE** picks what the gizmo poses: either end of the active stage, or the Base Transform, so the resting pose can be dragged on the picture instead of typed in the Inspector. **CURVE** shows or
hides the curve editor panel.

Hide the curve editor when it is sitting over something you want to drag. It occupies the
top-right of the image and is hit-tested ahead of the motion path, so it will take clicks
intended for a path handle underneath it. Hiding it removes it from hit-testing entirely, not
just from view. The same switch is **Show Curve Editor** in the Inspector, and it is remembered
with the project.

**Transform gizmo** (on the image) — the outline shows the stage's pose. Drag inside it to
move, drag a corner to scale, drag the arm above the top edge to rotate, drag the circled
crosshair to move the anchor point. The gizmo is cyan for A and orange for B.

The gizmo is drawn **where the image actually is**, not where the stage alone would put it.
Stages compose, so stage 2's own numbers describe an offset from wherever stage 1 and the base
have already moved the picture to — a gizmo drawn from stage 2 in isolation would float somewhere
off the image entirely. The overlay composes the surrounding stages back in for display, and maps
your drag back out of them before writing, so the box lands on the picture while the numbers
written stay the stage's own.

**While you drag, the picture itself is previewed.** The plugin renders the image at the pose
being dragged — the real frame, not a rectangle standing in for it — **instead of** the frame the
playhead is parked on, tinted towards the gizmo's colour. Around it, the box being dragged is
shaded, the *other* end of the move is outlined in its own colour, and thin lines join matching
corners.

**GHOST** in the toolbar turns it off. It sits on the gizmo's row rather than with the panel
toggles above, because it is not a panel — it is how the gizmo behaves while being dragged. On a
straight zoom the outline says most of what there is to say, and the preview is just motion.

That exists because the gizmo shows where the image *will* be while the viewer shows whatever
frame the playhead sits on — usually the start of the clip. Without it you are moving an outline
around a picture that will not look like that, with nothing to judge the result against.

It **replaces** the frame rather than sitting over it. Compositing the two meant the picture
underneath was whatever the playhead happened to be parked on — a second, unrelated copy of the
object competing with the one being posed. Showing only the dragged pose leaves nothing to
confuse it with, and the tint is what says "preview" instead.

The preview is a **render**, not an overlay drawing — the overlay can only draw lines, polygons
and text, so it could never show the picture. It happens inside `RenderPixel`, which is shared by
the CPU and CUDA paths, so there is no second implementation to keep in step and the parity tests
cover it.

It is strictly a preview. The parameter that switches it on is hidden, is set only while a gizmo
drag is in progress, is cleared on pen-up and on every drag-recovery path, and is **not saved with
the project** — so it cannot survive a reload or leak into an export.

The Base Transform has no opposite end to ghost, being a single resting pose, so it gets the
shading alone.

The base gizmo is the one exception to the rule below: it follows the playhead, because it marks no place in any move and is most useful sitting on the picture as it looks right now. For a stage, the surrounding stages are sampled at **this stage's own start or end frame** — whichever end
the gizmo is posing — and never at the playhead. That is what keeps the gizmo still while you
scrub: it marks a fixed place in the move. It also means the B gizmo sits exactly on the
rendered image at that stage's end frame, without having to park the playhead there.

**Motion path** (on the image) — the route the stage travels, in the stage's lane colour, with
a cyan dot at the start and an orange one at the end. Drag either handle to bend it; the white
dots along it mark equal time intervals, so they show where the move is fast or slow.

The path is **swept over the stage's own span of frames**, so it is fixed in place and does not
move as you scrub. Each end and its tangent handle are pinned to that end's frame, which is what
makes consecutive stages meet: where stage 1 leaves the image is where stage 2's route begins.
Because it follows time rather than the bezier's parameter, it also shows the route bending with
any other stage that is moving during the same window, and it shows an overshoot genuinely
carrying the image past its end point and back.

**Stage timing lanes** (bottom) — one lane per stage, drawn against a shared frame ruler with
the playhead marked. Drag a bar's left or right edge to change its start or end frame; drag its
middle to slide the whole stage without changing its length. Clicking a lane also selects that
stage. This is where staggering becomes obvious: the offsets between stages are visible as
offsets between bars.

Each bar carries three numbers: its **start** and **end** frame just outside either edge, and its
**duration** in the middle, signed (`+12`). Duration is the number you actually think in when
pacing a move, and it was the one the lane made you work out by subtracting the two it was already
showing. The sign is there because a stage whose end precedes its start plays backwards — worth
seeing plainly rather than hiding behind an absolute value. On a bar too narrow to hold it the
duration is dropped rather than drawn over its neighbours.

**SYNC TIME** and **SYNC EASE** sit in the panel's header and act on the active stage — the same
two operations as the Inspector's *Sync Acceleration to Playhead* and *Sync Acceleration by
Easing*. They are here because both are about placing the red peak tick against the playhead, and
both are visible on the lanes directly underneath.

Each bar is **shaded by how fast the move is going** at that point, brightest where the motion
is quickest, and a red tick marks the **peak** — the single fastest moment. That is usually
where a cut, a sound hit or an impact should land, and for anything but linear easing it is
*not* the middle of the bar: an Ease Out peaks almost immediately, an Ease In peaks at the very
end. The shading is measured with the same evaluator that renders the frames, including the
bounce oscillation, so it does not drift from what actually happens.

Stages that hold a pose rather than change it are left unshaded — the easing curve still has a
steepest point, but nothing happens there, and marking it would be misleading.

**Curve editor** (top right, toggled by **CURVE**) — the actual easing curve for the active stage, plotted with the
same evaluator the renderer uses, so what you see is what it does. Drag the two control handles
to shape it. The faint diagonal is linear for reference, the horizontal lines mark 0 and 1
(anything beyond them is anticipation or overshoot), and the yellow dot shows where the current
frame sits on the curve.

Everything written by the overlay lands in the ordinary parameters, so the Inspector and the
overlay always agree, and every edit is undoable in the normal way.

**Opacity slider** (left edge) — the one animated channel with no other on-screen control.
It follows the same A / B / BASE target the gizmo is on and takes that target's colour, so
the two always describe the same thing. Click anywhere on the track to jump to a value; the grab
area is much wider than the drawn track, which is deliberately thin to stay out of the way.

Deliberately not attached to the gizmo: the gizmo rotates and scales with the pose it represents,
and a slider that tilted with it would be unusable at exactly the moments it matters.

### Click a handle twice to reset it

Each control resets exactly what it drives — repeat-clicking the rotation arm does not move the
position too.

| Repeat-click | Resets |
|---|---|
| Inside the gizmo box | Position |
| A corner | Scale, both axes |
| The rotation arm | Rotation |
| The anchor crosshair | Anchor, back to centre |
| A motion path handle | That half of the route, back to straight |
| The opacity slider | Full opacity |

Works on A, B and BASE — it resets whichever target the gizmo is on.

**In Resolve this takes three clicks, not two.** OFX has no double-click action and its pen
events carry no click count, so the gesture is inferred from the interval and distance between
presses. Widening the window from 400 ms to 900 ms changed nothing, which rules out the host
merely being slow; what fits is that Resolve consumes the *first* press to give the on-screen
controls focus, so the plugin only ever sees presses two and three. A duplicated press would
cause the opposite symptom — a reset on a single click — so that is ruled out too. Nothing in a
plugin can recover a press that never arrives, so three clicks it is.

Scale resets both axes even when linked, because the corner *is* the size handle and a stale
Scale Y waiting to reappear when you unlink would be a trap. Tilt and Swivel are left alone —
separate channels, not part of the size.

### Hold Shift to constrain a drag

| Drag | With Shift |
|---|---|
| Move the gizmo | Locks to one axis |
| Gizmo corner, scale **unlinked** | Changes only the axis you are pulling |
| Rotate | Snaps to 15° |
| Anchor point | Locks to one axis |
| Motion path handle | Locks to one axis |
| Curve editor handle | Locks to timing *or* to overshoot, not both |
| Timeline lane — either end | Snaps to 5-frame steps |
| Timeline lane — whole bar | Slides in 5-frame steps, keeping its duration |

On the timeline Shift means *coarser* rather than *constrained* — there is no second axis to lock
— which is the same thing it already means on the rotate handle. Dragging an **end** snaps the
resulting frame number to a multiple of five, so stages line up with each other and with a beat.
Dragging the **whole bar** snaps the movement instead, so a stage keeps whatever length it was
given: snapping both ends to the grid would quietly change the duration of any stage that was not
already a multiple of five frames long.

Under a **Stretch** anchor the values are percentages, not frames, so the step is five percent of
the clip — the same coarse round increment in the unit that stage is authored in.

**Hold Ctrl to drive both handles of a pair at once.** On the curve editor it
mirrors the far handle, so one drag shapes a symmetric ease — Ease Out follows Ease In, and
Overshoot follows Anticipation. On the motion path it gives both handles the same offset, bowing
the route symmetrically instead of bending it into an S. Ctrl and Shift combine: an axis-locked
symmetric edit.

The axis is chosen from the drag as it goes, not fixed when Shift goes down, so you can press and
release Shift mid-drag. The gizmo locks to the **stage's own** axes rather than the screen's — on
a rotated stage that is the axis the number being written actually refers to.

The curve editor's two axes are two separate questions: horizontal is the timing (Ease In / Ease
Out), vertical is the amount of anticipation or overshoot. Locking one is the only practical way
to adjust either without disturbing the other.

**This one depends on Resolve.** OFX pen events carry no modifier state at all — `PenArgs` has a
position and a pressure and nothing else — so Shift has to be tracked from separate key events.
If Resolve does not deliver those to plugin overlays, Shift simply does nothing and dragging
behaves exactly as it did before.

### Labels and why they sometimes shorten

`OfxDrawSuite` has **no text-metrics call** — `drawText` is the only text entry point and nothing
reports a width, so a plugin cannot know how wide its own label is. Overlay buttons therefore
estimate it and trim anything that would not fit, rather than letting text run outside its box.

Boxes are sized to their labels wherever the layout allows, so trimming stays a fallback. The one
place it can still bite is the toolbar at extreme zoom-out, where the buttons shrink to keep clear
of the stage tabs and `CURVE` becomes `CURV`. That is the deliberate trade: a clipped character
is a better failure than a row of buttons colliding with the tabs.

### Staying visible on white footage

Everything the overlay draws over the picture is drawn **twice**: a wide near-black pass first,
then the real colour thinner on top. Outlines, the anchor cross, the rotation arm, the motion path
and its ticks, and every label on the image.

The obvious fix — pick a colour that always shows, red being the usual suggestion — does not
actually work. Any single ink is a point in the same colour space as the footage, so some footage
sits right on top of it: red disappears into skin tones, sunsets, tail lights and a Resolve scope.
Choosing a colour only moves *which* shots the overlay vanishes on.

Drawing each mark twice sidesteps that, because it stops depending on the background's **colour**
and starts depending only on its **brightness**. On white footage the black rim carries the shape;
on black footage the bright core does; on mid greys both edges read. The marks also keep their
meanings — cyan is still A, orange still B, violet still the base, and the four stage colours
still tell the lanes apart — which recolouring everything red would have thrown away.

The rim is 72% black rather than opaque, so the picture is still readable through it instead of
being blocked out by a heavy outline. Panels do not use any of this: a panel is already a dark
backdrop of known brightness, so it solves the problem for its own contents.

Labels get the same treatment as four offset copies rather than a drop shadow — a shadow only
protects the two edges it falls on, and a label over white footage needs all four. There is no
outline or shadow call in `OfxDrawSuite` to do it properly.

### Known limitation

The panels are positioned relative to the **image**, not the viewport, so if you zoom deep into
the picture they can drift off screen. OFX 1.4 removed `kOfxInteractPropViewportSize` and
provides no other way for a plugin to learn the viewport bounds, so anchoring to the image is
the only option available. Zoom to fit to bring them back.

**The plugin cannot move the playhead.** Resolve silently ignores the timeline suite's
`gotoTime`, and OFX offers no alternative, so there is no way to add a "jump to this stage's
start" button — it was tried and removed. Timing therefore goes the other way round: park the
playhead and click **Set Start / Set End to Playhead**. Reading the playhead works fine; only
writing it is unavailable.

## Performance

Measured on an RTX 5070 Ti at 3840x2160, CUDA kernel time per frame **per instance**
(`bench_render` in the build output re-runs this):

| Case | Samples | Time | Four layered clips |
|---|---|---|---|
| No movement (any blur setting) | 1 | **0.31 ms** | 1.2 ms |
| Mid-move, no blur | 1 | 0.30 ms | 1.2 ms |
| Mid-move, adaptive blur | 30 | 2.54 ms | 10.2 ms |
| Mid-move, 64 fixed samples | 64 | 5.31 ms | 21.2 ms |
| CPU fallback path | 1 | ~122 ms | — |

Three things follow from this.

**A frame that is not moving costs one sample, whatever the blur settings say.** If nothing
has moved across the shutter interval and opacity is not mid-fade, every sample would produce
an identical image, so they collapse to one. A static frame with 64 fixed samples used to cost
17x what it needed to.

**Motion blur is the cost driver, and only while something is actually moving.** It is roughly
8x the cost of no blur. Leave **Adaptive Samples** on — it spends samples in proportion to real
on-screen movement instead of paying a fixed worst case every frame.

**Check that the GPU path is actually being used.** The CPU fallback is ~400x slower, and four
layers of that will not play back. `probe.log` records `render dispatch: cuda=yes` on the first
render; if it says `no`, that dwarfs every other consideration.

### Prefer stages over stacked instances

Four stages inside **one** instance measure 0.35 ms — the same as one stage, because stages are
composed into a single matrix before the kernel runs. Four *separate* instances cost four times
the kernel time plus four full-frame intermediate buffers (~127 MB each at UHD RGBA float), and
resample the image four times, which softens it. Layered clips each needing their own transform
are unavoidable; multiple transforms on the *same* clip are not.

An effect that does nothing **at any frame** reports `isIdentity`, and the host may skip it
entirely. That test is deliberately time-independent. Judging it per frame looks correct and is
not: outside a stage's range the progress pins to 0 or 1, so the transform collapses to the From
or To pose — normally the identity — and the effect declares itself a pass-through on precisely
the frames outside its own animation. Hosts cache that verdict per frame, so moving the stage's
start or end over one of those frames left the host still convinced nothing happened there, and
edits made with the playhead outside the range appeared to do nothing until the cache was
purged. Giving the same answer at every frame removes the failure mode; the cost is losing the
skip on merely-neutral frames, worth about 0.3 ms each.

## A note for other OFX hosts

### Pixel depth follows the comp

The plugin accepts **8-bit, 16-bit integer, 16-bit float (half) and 32-bit float** images, and
renders at whatever depth the host supplies. It used to declare float only, which made Fusion
convert an 8-bit comp to 32-bit float from this node onward — four times the memory traffic for
a transform that gains nothing from it. Resolve's Edit page always supplies float regardless;
Fusion supplies the comp's own depth.

The maths is float throughout. Depth exists only at the two edges: a texel is converted to
float when loaded and the result converted back when stored, on the CPU and in the CUDA kernel
alike. Half is converted at the bit level in the shared header rather than through the CUDA
intrinsics, which exist only on the device — the CPU reference has to produce the *same bits*,
or the parity test could not tell a rounding difference from a bug. Integer depths clamp to
0..1 and round to nearest on store; float keeps overshoot exactly as before.

If detail matters — a long motion blur averages many resamples, and 8-bit input can band in
smooth gradients — keep the comp at 16-bit float. The plugin will follow it.

### Fusion hands out images the Edit page never does

Fusion crops a node's input to its **domain of definition**. So while the Edit page always
supplies a source the same size as the frame, in Fusion the source can be a small rectangle
offset inside the frame, or — for an element with nothing to show at the current frame, which
is routine while scrubbing — **empty**, sometimes with a null data pointer.

The empty case used to crash. With a zero-width image the sampler's edge handling had no valid
texel to fall back on: Clamp and Mirror both resolved to index **−1**, and only Black happened
to bail out before reading. That was an out-of-bounds read on the CPU path and a device fault
on CUDA — and a composition with several Multi Transform nodes being scrubbed is the surest way
to hit it, because at any given frame *some* node's input is likely to be empty.

Now:

- An empty or data-less source samples as transparent, in every edge mode, without touching
  memory. On CUDA it also **clears** the output rather than leaving the host's buffer holding a
  stale frame.
- The transform is evaluated in the **destination's** frame, and a cropped source is sampled
  at its real offset inside it — so `position 1.0` still means one frame width and `anchor 0.5`
  still means the frame centre, not the element's own little box. The parity test renders a
  cropped source and the same pixels embedded full-frame and requires them to match exactly.
- Sampling coordinates are bounded before integer conversion, so a near-singular transform can
  no longer produce the undefined float-to-int behaviour that `x0 + 1` then overflowed.

When a source's bounds differ from the destination's, `probe.log` records it once per session
with both rectangles — so the next Fusion report can be read against facts.

### Fusion builds Inspector controls only for parameters that start visible

The three inactive stages are hidden at **runtime**, by the instance, never on the descriptor.
A version that marked stages 2–4 secret at describe time — to spare the constructor some
`setIsSecret` calls — worked on the Edit page and broke the Fusion page: selecting Stage 2
there showed its group headers, and none of them would expand. Fusion had never built the
controls, because the descriptors said they were secret; the runtime un-hide had nothing to
act on. Resolve's Edit-page host builds every control and toggles visibility, which is why the
same build looked fine there.

The general rule this leaves behind: on Fusion, **a parameter that must ever be shown must be
described visible**. Hide it afterwards if you like; do not describe it hidden.

### Fusion renders one instance on several threads, and the SDK's handle cache has no lock

`OFX::ParamSet::fetchParam(name)` in the support library caches handles in a `std::map`: a
fetch finds, and on a miss **inserts**, unguarded. Fusion renders a single instance on three
or more threads concurrently while scrubbing. Any parameter first fetched by name *from inside
a render* is therefore a data race — two threads miss the map in the same moment, both insert,
and the tree is corrupted. That crashed Resolve twice in an evening, each time seconds into a
fresh session (cold map) while a gizmo drag overlapped a Fusion scrub. The Edit page has one
render thread, so the same code worked there all day.

The rule: **every parameter the plugin ever fetches by name is fetched once in the
constructor.** After that every lookup is a read-only find, which concurrent threads may share.
The constructor carries the list, and anything fetched by name anywhere — the overlay, the
triggers, changedParam — must be on it. Render-side code goes further and reads through
stored handles only.

### Fusion can fault inside `clipGetRegionOfDefinition` — so the overlay never asks

The overlay used to ask the host for the source clip's region of definition on every draw, to
know the frame it was drawing on. On the Fusion page that query runs Fusion's graph evaluation,
and at a frame where the node's input cannot be resolved — `cannot get Parameter for Source`
in Fusion's own log, which is routine while scrubbing — the host returned an error on a good
day and **faulted with a null write inside ntdll** on a bad one. A crash dump, read against a
linker map of the crashed binary, showed exactly that chain on the UI thread: overlay draw →
`Clip::getRegionOfDefinition` → Fusion's OFX host → ntdll. A status-checked call would still
have made the call.

So the overlay does not ask. `render()` sees the destination's bounds on every frame and
publishes them (canonical coordinates, render scale divided out); the overlay reads the last
published rectangle. Before the first render it draws on the **project frame** (size and
offset, read as plain properties of the instance) — and that fallback is not a corner case: a
freshly added effect is the identity, `isIdentity` says so, and the host never calls `render()`
until something changes, so an overlay that waited for a render would not appear until the
user had already edited the thing they needed it to edit. The only host calls left on the
overlay's hot path are raw, status-checked property reads, so nothing on the UI thread can
start an exception at all.

Two habits this left behind: the linker writes a `.map` beside the binary on every build (an
address out of a Resolve crash dump becomes a function name with a grep), and `buildContext`'s
catch now logs *what* was thrown and with which status, because "threw" alone was the last
line before two crashes and said nothing about why.

The animation is driven by the render time, not by host keyframes, so **no parameter is ever
animated**. A host that assumes "no animated parameters means a static output" will render one
frame and reuse it for the whole clip — playback freezes while the controls still update the
image live.

Two OFX properties have to be declared because of this, and **both default to the wrong value
for an effect that animates internally**:

- **`kOfxImageEffectFrameVarying`**, set in `getClipPreferences`. It means "generates a
  different image from frame to frame, even if no parameters or input image changes". Without
  it a host may render one frame and reuse it, so playback freezes while the controls still
  update the image live.
- **`kOfxParamPropCacheInvalidation` = `kOfxParamInvalidateAll`**, on every parameter that
  affects the render. The default, `kOfxParamInvalidateValueChange`, means "invalidate only the
  range of frames this parameter's *keyframe* affects" — correct for keyframed parameters, and
  meaningless here, because these parameters have no keyframes and each affects every frame.
  Without it a caching host keeps replaying stale frames after an edit until the cache is
  purged by hand.

  The parameters that only drive the viewer overlay — Active Stage, Gizmo Edits, Show Curve
  Editor — deliberately keep the default, since they change nothing about the rendered image
  and purging the cache when a stage tab is clicked would just stall playback.

- **`paramEditBegin` / `paramEditEnd`** around every parameter the *plugin itself* writes. The
  spec requires these whenever a plugin calls `paramSetValue` "either from custom GUI
  interaction or some analysis of imagery", from an interact action or `changedParam`. That is
  every write this plugin makes: the overlay's gizmo, timeline, path and curve handles, the Set
  Start/End buttons, the easing presets, Straighten Path, and the derived Duration.

  Without the brackets the host is never told the edit happened. The symptom is distinctive:
  edits made through the host's *own* Inspector work, because the host performed them and knows
  about them, while anything dragged in the overlay or set by a button appears to do nothing
  until the cache is purged by hand. Bracketing also collapses a drag into one undo step
  instead of one per mouse-move event.

Resolve is lenient about all three and never showed any of the problems; Fusion honours all
three, and is the one behaving correctly.

`probe.log` records the host's capabilities on load, which render path was chosen, and any
error thrown in the overlay or the CUDA kernel. It is quiet during normal playback.

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
