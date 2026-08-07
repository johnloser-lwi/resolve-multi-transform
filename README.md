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
    Enabled, Set Start / Set End, Start Frame, End Frame, Duration, Anchor, Link Scale X/Y
—  FROM (START)  —
    Scale, Scale Y, Position, Rotation, Tilt (X axis), Swivel (Y axis), Opacity
—  TO (END)  —
    Scale, Scale Y, Position, Rotation, Tilt (X axis), Swivel (Y axis), Opacity
—  EASING  —
    Easing, Ease In, Ease Out, Anticipation, Overshoot
```

Reading a pose is one block top to bottom, rather than picking every other row out of an
interleaved `Scale From / Scale To / Position From / Position To` list.

**Anchor deliberately sits in the timing section**, not in either pose block — a pivot that
moved between the start and end of a move would make the motion very hard to reason about.

### The base transform — a resting pose

Above the stages sits a **— BASE TRANSFORM —** section: a static pose with the same channels a
stage has, but no timing. It answers "this layer just *sits* here, at this size" without
spending a whole stage on a From that equals its To.

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

### Motion path

By default a stage moves in a straight line from its From position to its To position. The
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

They default to `%LOCALAPPDATA%\MultiTransform\Presets`, and a stage preset loads into whichever
stage is active. The files are readable and diffable, and they work in both Resolve and Fusion.

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

**Stage tabs** (above the timeline) — click `1`–`4` to choose which stage the gizmo and curve
editor act on. **FROM / TO** picks which end of that stage the gizmo poses. **CURVE** shows or
hides the curve editor panel.

Hide the curve editor when it is sitting over something you want to drag. It occupies the
top-right of the image and is hit-tested ahead of the motion path, so it will take clicks
intended for a path handle underneath it. Hiding it removes it from hit-testing entirely, not
just from view. The same switch is **Show Curve Editor** in the Inspector, and it is remembered
with the project.

**Transform gizmo** (on the image) — the outline shows the stage's pose. Drag inside it to
move, drag a corner to scale, drag the arm above the top edge to rotate, drag the circled
crosshair to move the anchor point. The gizmo is cyan for FROM and orange for TO.

The gizmo is drawn **where the image actually is**, not where the stage alone would put it.
Stages compose, so stage 2's own numbers describe an offset from wherever stage 1 and the base
have already moved the picture to — a gizmo drawn from stage 2 in isolation would float somewhere
off the image entirely. The overlay composes the surrounding stages back in for display, and maps
your drag back out of them before writing, so the box lands on the picture while the numbers
written stay the stage's own.

The surrounding stages are sampled at **this stage's own start or end frame** — whichever end
the gizmo is posing — and never at the playhead. That is what keeps the gizmo still while you
scrub: it marks a fixed place in the move. It also means the TO gizmo sits exactly on the
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
