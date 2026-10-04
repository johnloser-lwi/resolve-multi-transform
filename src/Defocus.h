#pragma once

// Gaussian-looking blur ("Blur" in the interface, defocus in the code, because
// "blur" already names the motion blur throughout).
//
// Ported from Text Animator's compositor, which arrived at it the hard way:
//
//  * A separable box blur run three times -- the usual stand-in for a Gaussian,
//    indistinguishable from one at these radii.
//  * Cost independent of the radius: a running window, so each output pixel is
//    one add and one subtract however wide the blur is.
//  * Runs on PREMULTIPLIED pixels, which is what makes a plain average right:
//    colour is already weighted by coverage, so a transparent neighbour
//    contributes nothing instead of dragging black into the edge.
//  * The window is REAL, not an integer number of pixels. An integer window
//    quantises the blur, and the quantisation is worst exactly where it shows:
//    the narrowest box is one pixel either side, so an easing blur steps
//    6, 3, 3, 3, sharp and the last step reads as the blur snapping off. The
//    leftover fraction instead weights the two samples just past the window's
//    ends, which makes the result continuous in the radius -- and exactly the
//    identity at zero, so a blur can ease all the way to sharp.
//
// Header-only and device-safe, so the CPU renderer and the CUDA kernels run the
// same additions in the same order. A running window, not prefix sums, for the
// same reason: identical arithmetic is the only way the two paths agree.

#include <cstddef>

#include "TransformMath.h"   // MTX_HD

namespace mtx {

/** Below this radius the window is narrow enough to BE the identity, so the
 *  whole pass is skipped -- not some larger threshold that would reintroduce
 *  the snap the fractional window exists to remove. */
constexpr float kDefocusMinRadius = 0.003f;

MTX_HD inline bool DefocusActive(float radiusPx) { return radiusPx > kDefocusMinRadius; }

/** Each of the three passes spreads radius/3, so the total visible spread is
 *  the radius the user asked for. */
MTX_HD inline float DefocusHalfWidth(float radiusPx) { return radiusPx / 3.0f; }

/** @brief Border, in pixels, to render around the frame before blurring.
 *
 *  A pass reads up to halfWidth+1 pixels away (the fractional samples sit one
 *  past the integer window), so after three passes a pixel can be influenced
 *  from 3 * (r + 1) away. Rendering that much extra around the frame means the
 *  blur at the frame's edge sees the real image beyond it -- or real
 *  transparency, if the image ends there -- instead of an artificial cut-off
 *  that would fade every frame edge. One more pixel of margin on top. */
MTX_HD inline int DefocusPad(float radiusPx)
{
    const int r = static_cast<int>(DefocusHalfWidth(radiusPx));
    return 3 * (r + 1) + 1;
}

/** @brief One box pass along one line of RGBA floats.
 *
 *  @param in,out  first pixel of the line in the source and destination
 *  @param n       pixels in the line
 *  @param step    floats between consecutive pixels along the line (4 for a
 *                 row, 4 * width for a column)
 *
 *  Divided by the FULL window even where it hangs off the end of the line:
 *  outside is nothing, and nothing is transparent, so the end of a line fades
 *  out rather than smearing its last pixel outwards. With the padding from
 *  DefocusPad that end lies beyond the frame, where it cannot be seen.
 */
MTX_HD inline void DefocusBoxLine(const float* in, float* out, int n, std::ptrdiff_t step,
                                  float halfWidth)
{
    const int   r   = static_cast<int>(halfWidth);
    const float f   = halfWidth - static_cast<float>(r);
    const float inv = 1.0f / (2.0f * static_cast<float>(r) + 1.0f + 2.0f * f);

    float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i <= r && i < n; ++i)
        for (int c = 0; c < 4; ++c) sum[c] += in[step * i + c];

    for (int i = 0; i < n; ++i)
    {
        const int lo = i - r - 1, hi = i + r + 1;
        for (int c = 0; c < 4; ++c)
        {
            float edge = 0.0f;
            if (lo >= 0) edge += in[step * lo + c];
            if (hi < n)  edge += in[step * hi + c];
            out[step * i + c] = (sum[c] + f * edge) * inv;
        }
        const int add = i + r + 1, drop = i - r;
        if (add < n)
            for (int c = 0; c < 4; ++c) sum[c] += in[step * add + c];
        if (drop >= 0)
            for (int c = 0; c < 4; ++c) sum[c] -= in[step * drop + c];
    }
}

/** @brief One full pass over an RGBA float image, rows or columns.
 *
 *  Lines [lineBegin, lineEnd) only, so a caller can split the image across
 *  threads. Horizontal reads @p in and writes @p out row by row.
 */
inline void DefocusPassLines(const float* in, float* out, int w, int h, float halfWidth,
                             bool horizontal, int lineBegin, int lineEnd)
{
    const int n = horizontal ? w : h;
    const std::ptrdiff_t step     = horizontal ? 4 : static_cast<std::ptrdiff_t>(w) * 4;
    const std::ptrdiff_t lineStep = horizontal ? static_cast<std::ptrdiff_t>(w) * 4 : 4;

    for (int l = lineBegin; l < lineEnd; ++l)
        DefocusBoxLine(in + lineStep * l, out + lineStep * l, n, step, halfWidth);
}

/** @brief The whole blur, single-threaded: three horizontal + vertical rounds,
 *  leaving the result back in @p image. @p scratch must be as large. */
inline void DefocusImage(float* image, float* scratch, int w, int h, float radiusPx)
{
    if (w <= 0 || h <= 0 || !DefocusActive(radiusPx)) return;
    const float hw = DefocusHalfWidth(radiusPx);
    for (int pass = 0; pass < 3; ++pass)
    {
        DefocusPassLines(image,   scratch, w, h, hw, true,  0, h);
        DefocusPassLines(scratch, image,   w, h, hw, false, 0, w);
    }
}

} // namespace mtx
