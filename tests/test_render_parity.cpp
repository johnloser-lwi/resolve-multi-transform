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

#include <cmath>
#include <cstdio>
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
    v.data = src.data();
    v.width = w;
    v.height = h;
    v.rowStrideFloats = w * 4;

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
};

static void RunCase(const Case& c, int w, int h)
{
    const std::vector<float> src = MakeTestImage(w, h);
    std::vector<float> cpu(static_cast<size_t>(w) * h * 4, 0.0f);
    std::vector<float> gpu(static_cast<size_t>(w) * h * 4, 0.0f);

    const SampleTransforms st = BuildSampleTransforms(c.anim, c.blur, c.time,
                                                      static_cast<float>(w),
                                                      static_cast<float>(h));

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

int main()
{
    std::printf("=== CPU vs CUDA parity ===\n");

    const int W = 256, H = 192;

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

    for (const Case& c : cases) RunCase(c, W, H);

    std::printf("\n%d failure(s)\n", g_failures);
    if (g_failures == 0) std::printf("PARITY OK\n");
    return g_failures == 0 ? 0 : 1;
}
