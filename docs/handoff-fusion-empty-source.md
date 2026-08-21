# Handoff: crashes and CPU fallback from Fusion's empty / cropped source images

**For:** whoever maintains **TextAnimator** (or any other OFX plugin of ours that reads
source pixels and runs on the Fusion page).
**From:** the Multi Transform investigation, August 2026. Fixed there in commits
`ad859d4` (the fix) and `cc4e61d` (the diagnostics) of `resolve-multi-transform`.
**Read time:** 10 minutes. **Checklist time against a plugin:** an hour or two.

This document assumes no knowledge of the Multi Transform conversation. Everything
needed is here.

---

## 1. What it looked like

Two symptoms that seemed unrelated and turned out to have one cause:

1. **Resolve crashed** while scrubbing a Fusion composition containing several instances
   of the plugin. Intermittent, more likely with more nodes.
2. **Fusion-page playback was terrible while the Edit page played the same comp in real
   time**, with the Fusion cache cleared — and the GPU sat idle during the bad playback.

If TextAnimator shows *either* of these, it is probably the same bug.

## 2. Root cause

### Fusion hands out images the Edit page never does

Resolve's Edit page always gives a filter plugin a source image the same size and position
as the output. **Fusion crops a node's input to its domain of definition (DoD).** So on the
Fusion page a source can be:

- **smaller than the frame and offset inside it** (an element that only covers part of the
  canvas), or
- **empty — zero width or height — possibly with a null data pointer**, for an element that
  has nothing to show at the current frame. This is *routine while scrubbing*: outside a
  clip's range, before a text element has any characters, a mask that resolves to nothing.

Multi Transform's code never considered either case because the Edit page never produced them.

### The crash: edge handling with a zero-sized image

The sampler's wrap function looked like this:

```cpp
int WrapCoord(int v, int n, EdgeMode mode, bool& outside)
{
    if (v >= 0 && v < n) return v;
    outside = true;
    if (mode == kEdgeClamp)              return v < 0 ? 0 : n - 1;
    if (mode == kEdgeMirror && n > 1)    { /* reflect */ }
    return v < 0 ? 0 : n - 1;             // Black: caller discards via `outside`
}
```

With `n == 0`:

- **Clamp** returns `n - 1` = **`-1`** → reads before the buffer.
- **Mirror** fails `n > 1` and falls through to the same `-1`.
- If `data` is null, even index 0 dereferences null.
- Only **Black** happened to bail out before reading (the caller checks `outside`).

On the CPU path that is an out-of-bounds or null read → **Resolve crashes**.

### The performance problem: the same read on the GPU poisons the CUDA context

The identical header compiles as CUDA device code. The same out-of-bounds read there is a
**device fault**. A device fault does two things:

- Sometimes it escalates and Resolve goes down (symptom 1 again).
- Otherwise it leaves the **CUDA context in a sticky error state**: every later CUDA call on
  that context fails. A host that sees a plugin's GPU render fail does the sensible thing and
  **stops using the GPU for that plugin**, rendering it on the CPU instead.

And Fusion's host offers the CPU path **one thread** (`host threads=1` — measured). So the
fallback was not "slower"; it was *4K on a single core*. That is the horrible playback and
the idle GPU. The Edit page was fine because Resolve's playback pipeline renders through its
**own** CUDA context, which the Fusion viewer's fault had not poisoned.

### Why the log never showed it

Two reasons, both worth knowing for TextAnimator:

- **Device faults are asynchronous.** `cudaGetLastError()` straight after the launch returns
  success; the error surfaces at the host's later synchronise, which the plugin never sees.
  A plugin cannot log a fault it caused.
- The "which render path am I on" log line was a log-once keyed on a **fixed string**, so it
  fired for whichever page rendered first in the process and never again. The Fusion page and
  the Edit page are **different OFX hosts** inside one process; the log had nothing to say
  about the second one.

## 3. Checklist to run against TextAnimator

Work through these in order. Each is a grep-and-read, not a redesign.

### A. Every place that reads source pixels

Find them all (`getPixelData`, `getPixelAddress`, `getBounds`, any `ImageView`-like struct).
For each, answer:

- [ ] What happens when the source's **width or height is 0**?
- [ ] What happens when **`getPixelData()` returns null** while bounds are non-empty, or
      bounds are empty but the pointer is stale?
- [ ] What happens when the source's **bounds origin differs from the destination's**
      (`src.x1 != dst.x1`)? Is the source sampled at frame coordinates without subtracting
      that offset? (Wrong picture, not a crash — but wrong in Fusion.)
- [ ] Is the **frame size** for the maths (positions, anchors, normalised units) taken from
      the *source* bounds? In Fusion that makes "the frame" mean the element's cropped box.
      It should come from the **destination** bounds.

### B. Edge / wrap handling

- [ ] Any clamp of the form `n - 1` with no guard on `n <= 0`?
- [ ] Any mirror/reflect arithmetic with `n <= 1` (period `2n - 2` becomes 0 or negative;
      `%` by 0 is UB)?
- [ ] Does the "transparent outside" mode actually **return before** touching memory, or
      does it compute an index first and discard afterwards?

### C. Coordinate conversion

- [ ] `static_cast<int>(floorf(x))` on an **unbounded float**. Out-of-int-range float → int
      is UB on the CPU (x86 gives 0x80000000); CUDA saturates. Either way `x0 + 1` can then
      overflow. Bound the float first (±2^24 is a safe, exact limit).
- [ ] NaN coordinates. `NaN` fails every comparison, so it slips through `< 0` and `>= n`
      tests alike. Route it explicitly.

### D. The CUDA launcher

- [ ] Does it **return early** when there is no source / an empty source **without writing
      the destination**? The host's buffer then keeps its last contents → stale or torn frames
      exactly when an input vanishes mid-scrub. Clear it (`cudaMemset2DAsync` on the host's
      stream — by rows, since pitch ≠ width).
- [ ] Is a null **destination** pointer guarded?
- [ ] Are kernel sizes derived from the destination bounds, and are they guarded for ≤ 0?

### E. If TextAnimator is a *generator* (no source, or the source is a background)

The same questions apply to whatever it reads — a background input, a font atlas, a glyph
cache — and additionally:

- [ ] Can the **destination** be empty (0×0) or have a null pointer? Fusion can ask for that
      too. Every loop over destination pixels must tolerate width/height ≤ 0.
- [ ] Does anything index a lookup texture/atlas with computed coordinates that could be
      out of range under a degenerate transform?

### F. Process-global state (a secondary Fusion concern)

Fusion renders nodes **concurrently**. Any file-scope/static mutable state shared between
instances needs a mutex or must be instance-owned. (Multi Transform's were all guarded; check
yours: caches, scratch buffers, "last result" memos, static device buffers.)

## 4. The fix pattern (reference implementation in `resolve-multi-transform`)

All in commit `ad859d4`. Copy the shape, not necessarily the code.

**`src/Sampler.h` — the view knows when it is empty, and the fetch refuses to read one:**

```cpp
struct ImageView
{
    const float* data;  int width;  int height;  int rowStrideFloats;
    int originX = 0;    // source pixel (0,0) relative to the destination's origin
    int originY = 0;
    MTX_HD bool Empty() const { return data == nullptr || width <= 0 || height <= 0; }
};

MTX_HD inline void FetchTexel(const ImageView& img, int x, int y, EdgeMode mode, float* out)
{
    if (img.Empty()) { out[0] = out[1] = out[2] = out[3] = 0.0f; return; }   // load-bearing
    ...
}
```

**Bound coordinates before integer conversion** (`BoundCoord` in the same file): clamp to
±2^24, send NaN to the far side where every edge mode returns a defined value.

**Sample at the source's real position** (`SampleImage`): subtract `originX/originY` from the
frame coordinates before fetching.

**`src/render/CudaKernel.cu` — the launcher clears instead of skipping:**

```cpp
if (!src || srcWidth <= 0 || srcHeight <= 0)
{
    cudaMemset2DAsync(dst, dstRowFloats * sizeof(float), 0,
                      dstWidth * 4 * sizeof(float), dstHeight, stream);
    return nullptr;       // (plus a sync when the host gave no stream)
}
```

**`render()` — the frame is the destination; the source origin is carried through:**

```cpp
const OfxRectI db = dst->getBounds();
const OfxRectI sb = src->getBounds();
const int originX = sb.x1 - db.x1, originY = sb.y1 - db.y1;
const float w = db.x2 - db.x1, h = db.y2 - db.y1;       // NOT the source's size
```

## 5. Tests that pin it (all in `tests/test_render_parity.cpp`, `RunFusionShapeChecks`)

Add equivalents. They run in seconds and would have caught this years earlier.

1. **Empty source** (`data = nullptr, width = 0, height = 0`): every edge mode × every
   filter samples **transparent**, no read.
2. **Zero-width source that still has a data pointer**: the guard fires *before* any read
   (point it at a real buffer so a bad index reads garbage instead of faulting — then check
   the output is transparent, proving the guard path was taken).
3. **GPU, empty source clears the destination**: pre-fill the output with 1.0, run with a
   null/empty source, require all zeros.
4. **Cropped source == embedded source** (the geometric proof): render a 96×64 element at
   offset (40,30) through the cropped-view path, and the *same pixels* pasted into a
   full-frame transparent image through the normal path. With Black edges they must match
   **exactly** (tolerance 1e-6).
5. **Cropped source, CPU vs GPU parity** for all three edge modes (tolerance 1e-4).
6. **Huge (±1e30) and NaN coordinates** resolve to a defined texel.

## 6. Diagnostics that would have found it in one playback (commit `cc4e61d`)

Log the render dispatch **once per kind**, not once per process, with cost:

```cpp
const std::string kind = std::string(args.isEnabledCudaRender ? "cuda" : "no-cuda") + "/"
                       + (args.isEnabledOpenCLRender ? "opencl" : "no-opencl");
// ... render ...
ProbeOnce("render-dispatch-" + kind,
          "render dispatch: " + kind + "  dst=WxH  first render N ms  host threads=T");
```

Reading the line:

```
render dispatch: cuda/no-opencl  dst=3840x2160  blur samples=1  first render 1.01 ms  host threads=1
```

- A `no-cuda/no-opencl` line appearing **after** a `cuda` line in the same session means the
  host stopped offering the GPU — almost certainly a poisoned context. Look for a fault.
- `host threads=1` is what Fusion offers the CPU path. Any CPU fallback there is single-core.
- `first render` is warm-up-inclusive; the order of magnitude is what matters (ms = GPU,
  hundreds of ms = CPU at 4K).

## 7. Verifying in Resolve

1. Build a Fusion comp with **three or more** instances of the plugin on elements that are
   empty at some frames (text that appears late, clips that start mid-comp).
2. **Scrub** across the whole comp, fast, for a minute. No crash.
3. **Play** on the Fusion page with the GPU monitor open: GPU should show load, playback
   should match the Edit page. Then read the log — one `cuda/...` dispatch line, no `no-cuda`.
4. Clear the Fusion cache, switch to the Edit page, play: still fine, still CUDA.

## 8. Things learned the hard way (so you don't)

- **Fusion's OFX host is its own implementation.** It is stricter than Resolve's (it needs
  `paramEditBegin/End` around plugin-initiated writes and `setOutputFrameVarying(true)` to
  re-render at all), offers one CPU thread, and crops inputs to DoD. Measure it; do not
  assume it behaves like the Edit page.
- **A log-once keyed on a constant hides the second host.** Key it on the state you care about.
- **Device faults never reach the plugin's error check.** Design the kernel so it cannot
  fault, rather than trying to detect that it did.
- **"GPU is idle" is the diagnostic.** The user noticed it before any log did. If the GPU is
  chilled on the page that plays badly and busy on the one that plays well, the code path
  differs — find why.
