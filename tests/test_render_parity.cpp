// CPU vs CUDA parity.
//
// The design bet of this project is that AnimEngine.h / TransformMath.h /
// Sampler.h are compiled once as host code and once as device code and agree.
// This test is what keeps that bet honest. A divergence here would otherwise
// surface as a barely-visible timing or sub-pixel difference between the
// viewer (GPU) and a CPU-path render -- the sort of bug that gets noticed
// late and is miserable to attribute.

#include "AnimEngine.h"
#include "Sampler.h"
#include "render/CudaRender.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace mtx;

static int g_failures = 0;

/** Reference CPU render, structurally identical to the plugin's CPU path. */
static void RenderCpu(const std::vector<float>& src, int w, int h,
                      std::vector<float>& dst,
                      const SampleTransforms& st,
                      FilterMode filter, EdgeMode edge)
{
    ImageView v;
    v.data = reinterpret_cast<const unsigned char*>(src.data());
    v.width = w;
    v.height = h;
    v.rowStrideBytes = w * 16;
    v.depth = kDepthFloat;

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            RenderPixel(v, st,
                        static_cast<float>(x) + 0.5f,
                        static_cast<float>(y) + 0.5f,
                        filter, edge, &dst[(static_cast<size_t>(y) * w + x) * 4]);
        }
    }
}

/** A deterministic image with plenty of high-frequency detail, so sampling
 *  differences have somewhere to show up rather than hiding in flat colour. */
static std::vector<float> MakeTestImage(int w, int h)
{
    std::vector<float> img(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            float* p = &img[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = ((x / 8 + y / 8) % 2) ? 0.9f : 0.1f;          // checkerboard
            p[1] = static_cast<float>(x) / static_cast<float>(w); // horizontal ramp
            p[2] = static_cast<float>(y) / static_cast<float>(h); // vertical ramp
            p[3] = 1.0f;
        }
    }
    return img;
}

struct Case
{
    const char* name;
    AnimParams  anim;
    float       time;
    FilterMode  filter;
    EdgeMode    edge;
    BlurParams  blur = BlurParams::Default();
    bool        ghost = false;   ///< composite the drag preview as well
};

static void RunCase(const Case& c, int w, int h)
{
    const std::vector<float> src = MakeTestImage(w, h);
    std::vector<float> cpu(static_cast<size_t>(w) * h * 4, 0.0f);
    std::vector<float> gpu(static_cast<size_t>(w) * h * 4, 0.0f);

    SampleTransforms st = BuildSampleTransforms(c.anim, c.blur, c.time,
                                                static_cast<float>(w),
                                                static_cast<float>(h));

    // The drag preview composites a second, faded copy of the source through
    // its own inverse. It runs inside RenderPixel, so both paths execute it and
    // both must agree -- otherwise the preview would look different depending
    // on whether the viewer happened to be on the GPU.
    if (c.ghost)
    {
        st.hasGhost     = true;
        st.ghostInv     = Invert(EvaluateTransform(c.anim, c.time + 6.0f,
                                                   static_cast<float>(w),
                                                   static_cast<float>(h)));
        st.ghostOpacity = 0.4f;
    }

    RenderCpu(src, w, h, cpu, st, c.filter, c.edge);
    RunMultiTransformCudaSync(src.data(), w, h, gpu.data(), w, h,
                              st, static_cast<int>(c.filter), static_cast<int>(c.edge));

    double maxDiff = 0.0;
    double sumDiff = 0.0;
    size_t worstIdx = 0;
    for (size_t i = 0; i < cpu.size(); ++i)
    {
        const double d = std::fabs(static_cast<double>(cpu[i]) - static_cast<double>(gpu[i]));
        sumDiff += d;
        if (d > maxDiff) { maxDiff = d; worstIdx = i; }
    }
    const double meanDiff = sumDiff / static_cast<double>(cpu.size());

    // Tolerance allows for fused-multiply-add and other legal float reassociation
    // between host and device, but is far tighter than any visible difference.
    const double kTol = 1e-4;
    const bool ok = (maxDiff <= kTol);
    if (!ok) ++g_failures;

    std::printf("  %-38s max=%.3e mean=%.3e  %s\n",
                c.name, maxDiff, meanDiff, ok ? "OK" : "FAIL");
    if (!ok)
    {
        const size_t px = worstIdx / 4;
        std::printf("      worst at pixel (%d,%d) ch%d: cpu=%.7f gpu=%.7f\n",
                    static_cast<int>(px % w), static_cast<int>(px / w),
                    static_cast<int>(worstIdx % 4), cpu[worstIdx], gpu[worstIdx]);
    }
}

static AnimParams MakeAnim(float scaleTo, float rotTo, float posXTo, float posYTo,
                           int stageCount = 1)
{
    AnimParams a = AnimParams::Default();
    a.stageCount = stageCount;
    for (int i = 0; i < stageCount; ++i)
    {
        a.stages[i].enabled    = true;
        a.stages[i].startFrame = 0.0f;
        a.stages[i].endFrame   = 20.0f;
        a.stages[i].scaleTo    = scaleTo;
        a.stages[i].rotTo      = rotTo;
        a.stages[i].posXTo     = posXTo;
        a.stages[i].posYTo     = posYTo;
    }
    return a;
}

/** Reference CPU render through an arbitrary view into a destination of a
 *  different size -- the shape Fusion produces and the Edit page never does. */
static void RenderCpuView(const ImageView& v, int dstW, int dstH, std::vector<float>& dst,
                          const SampleTransforms& st, FilterMode filter, EdgeMode edge)
{
    for (int y = 0; y < dstH; ++y)
        for (int x = 0; x < dstW; ++x)
            RenderPixel(v, st,
                        static_cast<float>(x) + 0.5f,
                        static_cast<float>(y) + 0.5f,
                        filter, edge, &dst[(static_cast<size_t>(y) * dstW + x) * 4]);
}

static void Report(const char* name, bool ok, double maxDiff = 0.0)
{
    if (!ok) ++g_failures;
    std::printf("  %-38s max=%.3e             %s\n", name, maxDiff, ok ? "OK" : "FAIL");
}

static double MaxDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    double m = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        m = std::max(m, std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
    return m;
}

/** The image shapes Fusion hands a node and the Edit page never does.
 *
 * Fusion crops an input to its domain of definition, so a source can be a
 * small rectangle offset inside the frame, or -- for an element with no DoD at
 * the current frame, which is routine while scrubbing -- empty, with or
 * without a data pointer. The empty case was an invalid read on the CPU and a
 * device fault on CUDA: with width 0, Clamp and Mirror resolved to texel -1.
 * These checks pin both the safety and the geometry of the fix.
 */
static void RunFusionShapeChecks(int W, int H)
{
    std::printf("--- Fusion image shapes ---\n");

    const SampleTransforms st = BuildSampleTransforms(MakeAnim(1.3f, 12.0f, 0.1f, -0.05f),
                                                      BlurParams::Default(), 10.0f,
                                                      static_cast<float>(W),
                                                      static_cast<float>(H));
    const EdgeMode modes[3] = { kEdgeBlack, kEdgeClamp, kEdgeMirror };

    // 1. Empty source, every edge mode and filter: transparent, and -- the part
    //    that matters -- no read through a null pointer or at index -1.
    {
        ImageView empty;
        empty.data = nullptr; empty.width = 0; empty.height = 0; empty.rowStrideBytes = 0;

        bool ok = true;
        for (EdgeMode m : modes)
            for (int f = 0; f < 2; ++f)
            {
                float out[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                RenderPixel(empty, st, 37.5f, 21.5f, static_cast<FilterMode>(f), m, out);
                ok = ok && out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f && out[3] == 0.0f;
            }
        Report("empty source samples transparent", ok);

        // Zero-sized but with a (stale) data pointer, which Fusion can also do.
        // Pointing it at a real buffer means a bad index would read garbage
        // rather than fault, so this checks the guard fires before any read.
        const std::vector<float> scratch(64, 0.75f);
        ImageView stale;
        stale.data = reinterpret_cast<const unsigned char*>(scratch.data());
        stale.width = 0; stale.height = 3; stale.rowStrideBytes = 32; stale.depth = kDepthFloat;
        float out[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        RenderPixel(stale, st, 2.5f, 1.5f, kFilterBilinear, kEdgeClamp, out);
        Report("zero-width source ignores its data", out[3] == 0.0f && out[0] == 0.0f);
    }

    // 2. GPU: an empty source must *clear* the destination, not leave it. The
    //    buffer is pre-filled so a skipped write is detected.
    {
        std::vector<float> gpu(static_cast<size_t>(W) * H * 4, 1.0f);
        RunMultiTransformCudaSync(nullptr, 0, 0, gpu.data(), W, H, st,
                                  static_cast<int>(kFilterBilinear), static_cast<int>(kEdgeClamp));
        bool ok = true;
        for (float v : gpu) ok = ok && v == 0.0f;
        Report("GPU: empty source clears the destination", ok);
    }

    // 3. Cropped, offset source. A 96x64 element placed at (40,30) in the frame
    //    must render *identically* to the same pixels embedded in a full-frame
    //    image with transparency around them. That is the geometric claim of
    //    the origin offset, and it holds exactly for Black edges, where "beyond
    //    the crop" and "transparent frame" mean the same thing.
    {
        const int cw = 96, ch = 64, ox = 40, oy = 30;
        const std::vector<float> crop = MakeTestImage(cw, ch);

        std::vector<float> full(static_cast<size_t>(W) * H * 4, 0.0f);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x)
                for (int c = 0; c < 4; ++c)
                    full[((static_cast<size_t>(y + oy) * W) + (x + ox)) * 4 + c]
                        = crop[(static_cast<size_t>(y) * cw + x) * 4 + c];

        ImageView cropped;
        cropped.data = reinterpret_cast<const unsigned char*>(crop.data());
        cropped.width = cw; cropped.height = ch; cropped.rowStrideBytes = cw * 16;
        cropped.depth = kDepthFloat; cropped.originX = ox; cropped.originY = oy;

        ImageView whole;
        whole.data = reinterpret_cast<const unsigned char*>(full.data());
        whole.width = W; whole.height = H; whole.rowStrideBytes = W * 16; whole.depth = kDepthFloat;

        std::vector<float> viaCrop(full.size(), 0.0f), viaFull(full.size(), 0.0f);
        RenderCpuView(cropped, W, H, viaCrop, st, kFilterBilinear, kEdgeBlack);
        RenderCpuView(whole,   W, H, viaFull, st, kFilterBilinear, kEdgeBlack);
        const double d = MaxDiff(viaCrop, viaFull);
        Report("cropped source == embedded source", d <= 1e-6, d);

        // And the GPU agrees with the CPU on the cropped shape for every edge
        // mode, including the two whose behaviour at the crop boundary differs
        // from the embedded case.
        for (EdgeMode m : modes)
        {
            std::vector<float> cpu(full.size(), 0.0f), gpu(full.size(), 0.0f);
            RenderCpuView(cropped, W, H, cpu, st, kFilterBilinear, m);
            RunMultiTransformCudaSync(crop.data(), cw, ch, gpu.data(), W, H, st,
                                      static_cast<int>(kFilterBilinear), static_cast<int>(m),
                                      ox, oy);
            const double dd = MaxDiff(cpu, gpu);
            const char* name = m == kEdgeBlack  ? "cropped source, GPU parity (black)"
                             : m == kEdgeClamp  ? "cropped source, GPU parity (clamp)"
                                                : "cropped source, GPU parity (mirror)";
            Report(name, dd <= 1e-4, dd);
        }
    }

    // 4. A coordinate far outside any image, and a NaN: both must resolve to a
    //    defined texel rather than an undefined int conversion.
    {
        const std::vector<float> px(16, 0.5f);
        ImageView one;
        one.data = reinterpret_cast<const unsigned char*>(px.data());
        one.width = 2; one.height = 2; one.rowStrideBytes = 32; one.depth = kDepthFloat;

        float out[4];
        SampleImage(one, 1.0e30f, -1.0e30f, kFilterBilinear, kEdgeClamp, out);
        const bool farOk = out[0] == 0.5f;
        SampleImage(one, std::nanf(""), 0.5f, kFilterBilinear, kEdgeBlack, out);
        const bool nanOk = out[3] == 0.0f;
        Report("huge and NaN coordinates are defined", farOk && nanOk);
    }
}

/** An image encoded at a given depth, tightly packed, from float RGBA. */
static std::vector<unsigned char> Encode(const std::vector<float>& img, int depth)
{
    const int b = BytesPerChannel(depth);
    std::vector<unsigned char> out(img.size() * b);
    for (size_t i = 0; i < img.size(); ++i) StoreChannel(&out[i * b], depth, img[i]);
    return out;
}

static std::vector<float> Decode(const std::vector<unsigned char>& buf, int depth)
{
    const int b = BytesPerChannel(depth);
    std::vector<float> out(buf.size() / b);
    for (size_t i = 0; i < out.size(); ++i) out[i] = LoadChannel(&buf[i * b], depth);
    return out;
}

/** CPU reference render reading one depth and writing another, tightly packed. */
static void RenderCpuDepth(const std::vector<unsigned char>& src, int w, int h, int srcDepth,
                           std::vector<unsigned char>& dst, int dstDepth,
                           const SampleTransforms& st, FilterMode filter, EdgeMode edge)
{
    ImageView v;
    v.data = src.data(); v.width = w; v.height = h;
    v.rowStrideBytes = w * 4 * BytesPerChannel(srcDepth); v.depth = srcDepth;

    const int pixelBytes = 4 * BytesPerChannel(dstDepth);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            float out[4];
            RenderPixel(v, st, x + 0.5f, y + 0.5f, filter, edge, out);
            StorePixel(&dst[(static_cast<size_t>(y) * w + x) * pixelBytes], dstDepth, out);
        }
}

/** Pixel depths other than float.
 *
 * The maths is float throughout; depth exists only at the load and the store.
 * Three things are pinned here: the half conversions are bit-correct, the
 * integer depths round-trip exactly, and a render at every depth agrees
 * between CPU and CUDA -- and with the float render of the same data, to
 * within the depth's own quantum. That last one is what proves the conversions
 * are in the right place and the right way round, not merely self-consistent.
 */
static void RunDepthChecks(int W, int H)
{
    std::printf("--- Pixel depths ---\n");

    // Half: exact on representable values, round-to-nearest-even elsewhere,
    // and the specials survive.
    {
        const float exact[] = { 0.0f, 1.0f, -1.0f, 0.5f, 0.25f, 2.0f, -2.0f, 65504.0f,
                                1024.0f, 0.0009765625f /* 2^-10 */ };
        bool ok = true;
        for (float v : exact) ok = ok && HalfToFloat(FloatToHalf(v)) == v;
        Report("half: representable values are exact", ok);

        const float inexact[] = { 1.0f / 3.0f, 0.1f, 3.14159f, 123.456f, -0.7071f, 1e-6f };
        bool close = true;
        for (float v : inexact)
        {
            const float r   = HalfToFloat(FloatToHalf(v));
            const float tol = std::max(std::fabs(v) * (1.0f / 2048.0f) * 1.01f, 6.0e-8f);
            close = close && std::fabs(r - v) <= tol;
        }
        Report("half: others within half precision", close);

        const float inf = std::numeric_limits<float>::infinity();
        const bool specials =
            HalfToFloat(FloatToHalf(inf))  ==  inf &&
            HalfToFloat(FloatToHalf(-inf)) == -inf &&
            HalfToFloat(FloatToHalf(std::nanf(""))) != HalfToFloat(FloatToHalf(std::nanf(""))) &&
            HalfToFloat(FloatToHalf(1.0e6f)) == inf &&            // overflow -> inf
            HalfToFloat(FloatToHalf(1.0e-9f)) == 0.0f &&          // underflow -> zero
            FloatToHalf(-0.0f) == 0x8000u;                        // signed zero kept
        Report("half: inf, nan, overflow, underflow, -0", specials);
    }

    // Integer depths: every 8-bit code round-trips, and 16-bit spot values do.
    {
        bool ok = true;
        for (int i = 0; i < 256; ++i)
        {
            unsigned char c;
            StoreChannel(&c, kDepthByte, static_cast<float>(i) / 255.0f);
            ok = ok && c == i && LoadChannel(&c, kDepthByte) == static_cast<float>(i) / 255.0f;
        }
        unsigned char s[2];
        StoreChannel(s, kDepthShort, 1.0f);   ok = ok && LoadChannel(s, kDepthShort) == 1.0f;
        StoreChannel(s, kDepthShort, 0.0f);   ok = ok && LoadChannel(s, kDepthShort) == 0.0f;
        StoreChannel(s, kDepthShort, 2.0f);   ok = ok && LoadChannel(s, kDepthShort) == 1.0f;   // clamped
        StoreChannel(s, kDepthShort, -1.0f);  ok = ok && LoadChannel(s, kDepthShort) == 0.0f;
        Report("byte and short round-trip and clamp", ok);
    }

    // Renders at every depth: CPU == CUDA, and both == the float render of
    // the same (already quantised) source, within one output quantum.
    {
        const SampleTransforms st = BuildSampleTransforms(MakeAnim(1.4f, 17.0f, 0.08f, -0.04f),
                                                          BlurParams::Default(), 11.0f,
                                                          static_cast<float>(W),
                                                          static_cast<float>(H));
        const std::vector<float> srcF = MakeTestImage(W, H);

        struct DepthCase { int depth; const char* name; double quantum; };
        const DepthCase depths[3] = {
            { kDepthByte,  "byte",  1.0 / 255.0   },
            { kDepthShort, "short", 1.0 / 65535.0 },
            { kDepthHalf,  "half",  1.0 / 1024.0  },   // worst case near 1.0
        };

        for (const DepthCase& d : depths)
        {
            const std::vector<unsigned char> src = Encode(srcF, d.depth);
            const std::vector<float>         srcQ = Decode(src, d.depth);   // what the render sees

            std::vector<unsigned char> cpu(src.size(), 0), gpu(src.size(), 0);
            RenderCpuDepth(src, W, H, d.depth, cpu, d.depth, st, kFilterBilinear, kEdgeBlack);
            RunMultiTransformCudaSync(src.data(), W, H, gpu.data(), W, H, st,
                                      static_cast<int>(kFilterBilinear), static_cast<int>(kEdgeBlack),
                                      0, 0, d.depth, d.depth);

            // The two paths do not quantise the *same* float: the float render
            // itself already differs between host and device by up to the
            // parity tolerance (legal FMA reassociation), and at 16-bit that is
            // two or three codes. So the bound is that float noise plus one
            // code of rounding, not one code alone.
            const double parity = MaxDiff(Decode(cpu, d.depth), Decode(gpu, d.depth));
            char name[64];
            std::snprintf(name, sizeof(name), "%s render, CPU == CUDA", d.name);
            Report(name, parity <= 1e-4 + d.quantum, parity);

            // Against the float render of the quantised source: the only
            // difference allowed is the output rounding.
            std::vector<float> ref(srcF.size(), 0.0f);
            RenderCpu(srcQ, W, H, ref, st, kFilterBilinear, kEdgeBlack);
            for (float& v : ref) v = Clamp01(v);              // integer depths clamp on store
            const double vsFloat = MaxDiff(Decode(cpu, d.depth), ref);
            std::snprintf(name, sizeof(name), "%s render == float render, quantised", d.name);
            Report(name, vsFloat <= d.quantum * 0.51 + 1e-6, vsFloat);
        }
    }
}

int main()
{
    std::printf("=== CPU vs CUDA parity ===\n");

    const int W = 256, H = 192;

    RunFusionShapeChecks(W, H);
    RunDepthChecks(W, H);

    std::vector<Case> cases;
    cases.push_back({ "identity",              MakeAnim(1.0f, 0.0f, 0.0f, 0.0f),  0.0f,  kFilterBilinear, kEdgeBlack });
    cases.push_back({ "scale 2x mid-anim",     MakeAnim(2.0f, 0.0f, 0.0f, 0.0f),  10.0f, kFilterBilinear, kEdgeBlack });
    cases.push_back({ "scale 0.5x complete",   MakeAnim(0.5f, 0.0f, 0.0f, 0.0f),  20.0f, kFilterBilinear, kEdgeBlack });
    cases.push_back({ "rotate 37deg",          MakeAnim(1.0f, 37.0f, 0.0f, 0.0f), 20.0f, kFilterBilinear, kEdgeBlack });
    cases.push_back({ "translate",             MakeAnim(1.0f, 0.0f, 0.25f, -0.1f),20.0f, kFilterBilinear, kEdgeBlack });
    cases.push_back({ "scale+rot+move",        MakeAnim(1.7f, -22.0f, 0.15f, 0.2f),13.0f, kFilterBilinear, kEdgeBlack });
    cases.push_back({ "nearest filter",        MakeAnim(1.7f, -22.0f, 0.15f, 0.2f),13.0f, kFilterNearest,  kEdgeBlack });
    cases.push_back({ "edge clamp",            MakeAnim(0.6f, 15.0f, 0.0f, 0.0f), 20.0f, kFilterBilinear, kEdgeClamp });
    cases.push_back({ "edge mirror",           MakeAnim(0.6f, 15.0f, 0.0f, 0.0f), 20.0f, kFilterBilinear, kEdgeMirror });
    cases.push_back({ "two composed stages",   MakeAnim(1.4f, 10.0f, 0.05f, 0.05f, 2), 12.0f, kFilterBilinear, kEdgeBlack });

    // Sub-frame times: the motion blur path in Phase 3 depends on these agreeing.
    cases.push_back({ "sub-frame t=7.37",      MakeAnim(1.8f, 30.0f, 0.1f, 0.1f), 7.37f, kFilterBilinear, kEdgeBlack });

    // Staggered stages exercise the easing solver at differing progress values
    // on the same frame, which is where host/device divergence would bite.
    {
        AnimParams a = MakeAnim(1.6f, 20.0f, 0.1f, 0.0f, 4);
        for (int i = 0; i < 4; ++i)
        {
            // Staggered starts and differing lengths, so every stage sits at a
            // different progress on the same frame.
            a.stages[i].startFrame = static_cast<float>(i) * 3.0f;
            a.stages[i].endFrame   = a.stages[i].startFrame + 20.0f + static_cast<float>(i) * 5.0f;
            a.stages[i].easing     = (i % 2) ? Easing::EaseIn() : Easing::EaseOut();
        }
        cases.push_back({ "four staggered stages", a, 9.0f, kFilterBilinear, kEdgeBlack });
    }

    // A custom overshoot curve: y outside [0,1] must behave identically on both.
    {
        AnimParams a = MakeAnim(1.5f, 0.0f, 0.0f, 0.0f, 1);
        a.stages[0].easing = Easing{ 0.68f, -0.55f, 0.27f, 1.55f };  // "back" easing
        cases.push_back({ "overshoot (back) easing", a, 6.0f, kFilterBilinear, kEdgeBlack });
    }

    // --- Motion blur ---
    {
        BlurParams b = BlurParams::Default();
        b.enabled = true;
        b.adaptive = false;          // fixed counts keep the comparison deterministic
        b.samples = 16;
        cases.push_back({ "blur 16 samples, 180deg",
                          MakeAnim(1.0f, 0.0f, 0.5f, 0.0f), 10.0f,
                          kFilterBilinear, kEdgeBlack, b });

        b.samples = 64;
        cases.push_back({ "blur 64 samples (max)",
                          MakeAnim(2.0f, 45.0f, 0.3f, 0.2f), 10.0f,
                          kFilterBilinear, kEdgeBlack, b });

        b.samples = 16;
        b.shutterAngle = 360.0f;
        cases.push_back({ "blur 360deg shutter",
                          MakeAnim(1.0f, 0.0f, 0.5f, 0.0f), 10.0f,
                          kFilterBilinear, kEdgeBlack, b });

        b.shutterAngle = 180.0f;
        b.shutterPhase = -90.0f;
        cases.push_back({ "blur shutter phase -90",
                          MakeAnim(1.0f, 0.0f, 0.5f, 0.0f), 10.0f,
                          kFilterBilinear, kEdgeBlack, b });

        b.shutterPhase = 0.0f;
        b.adaptive = true;
        cases.push_back({ "blur adaptive samples",
                          MakeAnim(1.0f, 0.0f, 0.5f, 0.0f), 10.0f,
                          kFilterBilinear, kEdgeBlack, b });

        b.adaptive = false;
        cases.push_back({ "blur + edge mirror",
                          MakeAnim(0.7f, 20.0f, 0.2f, 0.0f), 10.0f,
                          kFilterBilinear, kEdgeMirror, b });
    }

    // --- Motion path ---
    {
        AnimParams curved = MakeAnim(1.0f, 0.0f, 0.4f, 0.0f);
        curved.stages[0].pathC1Y =  0.35f;
        curved.stages[0].pathC2Y = -0.25f;
        cases.push_back({ "curved motion path", curved, 10.0f,
                          kFilterBilinear, kEdgeBlack });

        // A curved path plus blur moves the image along an arc within a single
        // shutter, which is where a host/device disagreement would show.
        BlurParams b = BlurParams::Default();
        b.enabled = true; b.adaptive = false; b.samples = 16;
        cases.push_back({ "curved path + motion blur", curved, 10.0f,
                          kFilterBilinear, kEdgeBlack, b });
    }

    // --- Bounce ---
    // The oscillator uses cosf/expf, which are separate device implementations
    // on the GPU; these confirm they agree with the host build.
    {
        AnimParams spring = MakeAnim(1.6f, 0.0f, 0.25f, 0.0f);
        spring.stages[0].easing = MakeEasing(0.0f, 20.0f, 0.0f, 0.0f,
                                             kBounceSpring, 60.0f, 3.0f, 40.0f);
        cases.push_back({ "spring bounce, mid-settle", spring, 12.0f,
                          kFilterBilinear, kEdgeBlack });

        AnimParams ball = MakeAnim(1.4f, 0.0f, 0.2f, 0.0f);
        ball.stages[0].easing = MakeEasing(30.0f, 0.0f, 0.0f, 0.0f,
                                           kBounceBall, 80.0f, 4.0f, 50.0f);
        cases.push_back({ "ball bounce, mid-rebound", ball, 14.0f,
                          kFilterBilinear, kEdgeBlack });

        // Bounce plus motion blur: the transform changes fast during a rebound,
        // so this is where host/device drift would show up first.
        BlurParams b = BlurParams::Default();
        b.enabled = true; b.adaptive = false; b.samples = 16;
        cases.push_back({ "spring bounce + motion blur", spring, 12.0f,
                          kFilterBilinear, kEdgeBlack, b });
    }

    // --- Opacity ---
    {
        AnimParams a = MakeAnim(1.2f, 0.0f, 0.1f, 0.0f);
        a.stages[0].opacityFrom = 0.0f;
        a.stages[0].opacityTo   = 1.0f;
        cases.push_back({ "fade in, mid-fade", a, 10.0f, kFilterBilinear, kEdgeBlack });

        BlurParams b = BlurParams::Default();
        b.enabled = true;
        b.adaptive = false;
        b.samples = 16;
        cases.push_back({ "fade in + motion blur", a, 10.0f, kFilterBilinear, kEdgeBlack, b });

        AnimParams out = MakeAnim(1.0f, 0.0f, 0.0f, 0.0f);
        out.stages[0].opacityFrom = 1.0f;
        out.stages[0].opacityTo   = 0.0f;
        cases.push_back({ "fade out, fully transparent", out, 20.0f,
                          kFilterBilinear, kEdgeBlack });
    }

    // --- Per-channel timing offsets ---
    // A frame where the staggered channels are at *different* points of their
    // curves: scale done, position mid-move, fade barely started. Any host/device
    // disagreement in the per-channel progress evaluation shows up as a real
    // pixel difference here, and the blurred variant sweeps the offsets across
    // every shutter sample.
    {
        AnimParams a = MakeAnim(1.6f, 20.0f, 0.2f, 0.1f);
        a.stages[0].opacityFrom   = 0.2f;
        a.stages[0].opacityTo     = 1.0f;
        a.stages[0].offsetPos     = 6.0f;
        a.stages[0].offsetRot     = -4.0f;
        a.stages[0].offsetOpacity = 12.0f;
        cases.push_back({ "channel offsets, mid-stagger", a, 14.0f,
                          kFilterBilinear, kEdgeBlack });

        BlurParams b = BlurParams::Default();
        b.enabled  = true;
        b.adaptive = false;
        b.samples  = 16;
        cases.push_back({ "channel offsets + motion blur", a, 14.0f,
                          kFilterBilinear, kEdgeBlack, b });
    }

    // --- Split scale, orthographic rotation, and the base pose ---
    {
        AnimParams split = MakeAnim(1.0f, 0.0f, 0.0f, 0.0f);
        split.stages[0].linkScale  = false;
        split.stages[0].scaleFrom  = 1.0f;  split.stages[0].scaleTo  = 1.8f;
        split.stages[0].scaleYFrom = 1.0f;  split.stages[0].scaleYTo = 0.55f;
        cases.push_back({ "split scale, mid-stretch", split, 10.0f,
                          kFilterBilinear, kEdgeBlack });

        // Mid-flip, where the cosine is doing real work rather than sitting at
        // one of its exact endpoints.
        AnimParams flip = MakeAnim(1.0f, 0.0f, 0.0f, 0.0f);
        flip.stages[0].swivelYFrom = -90.0f;
        flip.stages[0].swivelYTo   =   0.0f;
        flip.stages[0].tiltXFrom   =  25.0f;
        flip.stages[0].tiltXTo     = -10.0f;
        cases.push_back({ "orthographic flip, mid-swivel", flip, 11.0f,
                          kFilterBilinear, kEdgeBlack });

        // Exactly edge-on: the transform collapses and the frame must come back
        // transparent on both paths, not full-size on one of them.
        //
        // Deliberately NOT the last case. A collapsed transform used to fault on
        // the device and poison the CUDA context, which showed up as every
        // *subsequent* case rendering black -- invisible if this ran last, since
        // its own expected output is black either way.
        AnimParams edgeOn = MakeAnim(1.0f, 0.0f, 0.0f, 0.0f);
        edgeOn.stages[0].swivelYFrom = 90.0f;
        edgeOn.stages[0].swivelYTo   = 90.0f;
        cases.push_back({ "edge-on, exactly 90 degrees", edgeOn, 10.0f,
                          kFilterBilinear, kEdgeBlack });

        // The base pose is composed innermost, so this exercises a code path the
        // stages alone never reach. It also stands guard behind the edge-on case
        // above: if that one poisons the context again, this goes black.
        AnimParams based = MakeAnim(1.3f, 0.0f, 0.2f, 0.0f);
        based.base.scaleX    = 0.6f;
        based.base.scaleY    = 0.85f;
        based.base.linkScale = false;
        based.base.posX      = -0.15f;
        based.base.rot       = 12.0f;
        based.base.swivelY   = 30.0f;
        based.base.opacity   = 0.8f;
        cases.push_back({ "base pose under a moving stage", based, 12.0f,
                          kFilterBilinear, kEdgeBlack });
        // The drag preview, on top of a pose that is itself mid-move: the ghost
        // is sampled and composited per pixel, so any disagreement between the
        // two paths would show as a differently-faded copy.
        Case ghosted{ "drag preview ghost", based, 12.0f, kFilterBilinear, kEdgeBlack };
        ghosted.ghost = true;
        cases.push_back(ghosted);

        BlurParams gb = BlurParams::Default();
        gb.enabled = true; gb.adaptive = false; gb.samples = 8;
        Case ghostBlur{ "drag preview ghost + motion blur", based, 12.0f,
                        kFilterBilinear, kEdgeBlack, gb };
        ghostBlur.ghost = true;
        cases.push_back(ghostBlur);
    }

    for (const Case& c : cases) RunCase(c, W, H);

    std::printf("\n%d failure(s)\n", g_failures);
    if (g_failures == 0) std::printf("PARITY OK\n");
    return g_failures == 0 ? 0 : 1;
}
