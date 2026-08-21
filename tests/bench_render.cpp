// Render benchmark: what does one instance of this effect actually cost per
// frame at UHD? Used to decide where optimisation effort is worth spending,
// rather than guessing.

#include "AnimEngine.h"
#include "MotionBlur.h"
#include "Sampler.h"
#include "render/CudaRender.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace mtx;

float BenchMultiTransformCuda(const float* hostSrc, int width, int height,
                              const SampleTransforms& st,
                              int filterMode, int edgeMode, int iterations);

namespace {

constexpr int kWidth  = 3840;
constexpr int kHeight = 2160;

std::vector<float> MakeImage(int w, int h)
{
    std::vector<float> img(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            float* p = &img[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = ((x / 8 + y / 8) % 2) ? 0.9f : 0.1f;
            p[1] = static_cast<float>(x) / w;
            p[2] = static_cast<float>(y) / h;
            p[3] = 1.0f;
        }
    return img;
}

AnimParams MovingAnim(float scaleTo, float posXTo)
{
    AnimParams a = AnimParams::Default();
    a.stages[0].enabled    = true;
    a.stages[0].startFrame = 0.0f;
    a.stages[0].endFrame   = 20.0f;
    a.stages[0].scaleTo    = scaleTo;
    a.stages[0].posXTo     = posXTo;
    return a;
}

void BenchGpu(const char* name, const std::vector<float>& src,
              const AnimParams& a, const BlurParams& b, float time)
{
    const SampleTransforms st = BuildSampleTransforms(a, b, time, kWidth, kHeight);
    const float ms = BenchMultiTransformCuda(src.data(), kWidth, kHeight, st,
                                             kFilterBilinear, kEdgeBlack, 50);
    std::printf("  %-40s %2d sample(s)  %7.3f ms   (%5.1f fps, %4.1f instances @ 24fps)\n",
                name, st.count, ms,
                ms > 0.0f ? 1000.0f / ms : 0.0f,
                ms > 0.0f ? (1000.0f / 24.0f) / ms : 0.0f);
}

void BenchCpu(const char* name, const std::vector<float>& src,
              const AnimParams& a, const BlurParams& b, float time)
{
    const SampleTransforms st = BuildSampleTransforms(a, b, time, kWidth, kHeight);

    ImageView v;
    v.data = reinterpret_cast<const unsigned char*>(src.data());
    v.width = kWidth;
    v.height = kHeight;
    v.rowStrideBytes = kWidth * 16;
    v.depth = kDepthFloat;

    std::vector<float> dst(static_cast<size_t>(kWidth) * kHeight * 4);

    const auto t0 = std::chrono::high_resolution_clock::now();
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
            RenderPixel(v, st, x + 0.5f, y + 0.5f, kFilterBilinear, kEdgeBlack,
                        &dst[(static_cast<size_t>(y) * kWidth + x) * 4]);
    const auto t1 = std::chrono::high_resolution_clock::now();

    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("  %-40s %2d sample(s)  %7.3f ms   (single-threaded reference)\n",
                name, st.count, ms);
}

} // namespace

int main()
{
    std::printf("=== MultiTransform render benchmark: %dx%d ===\n\n", kWidth, kHeight);

    const std::vector<float> src = MakeImage(kWidth, kHeight);

    BlurParams off = BlurParams::Default();
    off.enabled = false;

    BlurParams fixed16 = BlurParams::Default();
    fixed16.enabled = true; fixed16.adaptive = false; fixed16.samples = 16;

    BlurParams fixed64 = BlurParams::Default();
    fixed64.enabled = true; fixed64.adaptive = false; fixed64.samples = 64;

    BlurParams adaptive = BlurParams::Default();
    adaptive.enabled = true; adaptive.adaptive = true; adaptive.samples = 64;

    std::printf("GPU (CUDA), kernel time only, per frame per instance:\n");

    // A static pose: no animation in progress, but the image is still scaled,
    // so a full resample happens. This is the "stacked but not moving" case.
    AnimParams staticPose = MovingAnim(1.25f, 0.1f);
    BenchGpu("static pose, no blur",            src, staticPose, off,      100.0f);
    BenchGpu("static pose, blur on (adaptive)", src, staticPose, adaptive, 100.0f);
    BenchGpu("static pose, blur 16 fixed",      src, staticPose, fixed16,  100.0f);
    BenchGpu("static pose, blur 64 fixed",      src, staticPose, fixed64,  100.0f);

    std::printf("\n");

    // Mid-animation: the expensive case, genuinely moving.
    AnimParams moving = MovingAnim(1.6f, 0.4f);
    BenchGpu("mid-move, no blur",               src, moving, off,      10.0f);
    BenchGpu("mid-move, blur on (adaptive)",    src, moving, adaptive, 10.0f);
    BenchGpu("mid-move, blur 16 fixed",         src, moving, fixed16,  10.0f);
    BenchGpu("mid-move, blur 64 fixed",         src, moving, fixed64,  10.0f);

    std::printf("\n");

    // A small scaled-down element, the common motion-graphics case: most of the
    // frame is transparent, yet every pixel is still sampled.
    AnimParams small = MovingAnim(0.25f, 0.0f);
    BenchGpu("scaled to 25% (mostly empty frame)", src, small, off, 100.0f);

    // Four stages inside ONE instance. Stages are composed into a single matrix
    // on the host, so the kernel never sees them individually -- this should
    // cost the same as one stage, unlike stacking four separate instances.
    {
        AnimParams four = AnimParams::Default();
        four.stageCount = 4;
        for (int i = 0; i < 4; ++i)
        {
            four.stages[i].enabled    = true;
            four.stages[i].startFrame = static_cast<float>(i) * 3.0f;
            four.stages[i].endFrame   = four.stages[i].startFrame + 20.0f;
            four.stages[i].scaleTo    = 1.1f;
            four.stages[i].posXTo     = 0.05f;
            four.stages[i].rotTo      = 5.0f;
        }
        std::printf("\n");
        BenchGpu("4 stages in ONE instance, mid-move", src, four, off, 10.0f);
        BenchGpu("4 stages in ONE instance + adaptive blur", src, four, adaptive, 10.0f);
    }

    std::printf("\nCPU reference (the plugin multithreads this across cores):\n");
    BenchCpu("mid-move, no blur", src, moving, off, 10.0f);

    return 0;
}
