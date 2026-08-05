// Multi Transform -- a multi-stage animated transform for DaVinci Resolve.
//
// Phase 1: the animation engine drives a CPU reference renderer. The CUDA path
// (Phase 2) and motion blur (Phase 3) reuse the same header-only maths, so this
// renderer stays as the correctness oracle they are diffed against.
//
// Two host facts shape the design, both measured in Phase 0 and recorded in
// probe.log:
//   1. Resolve supports neither parametric (curve) parameters nor custom
//      parameter-panel interacts, so the plugin owns its own animation.
//   2. Resolve passes timeline-absolute times and reports a meaningless clip
//      frame range, so the plugin cannot discover where the clip starts. The
//      "Set Start to Playhead" button captures it instead.

#include "MultiTransformPlugin.h"

#include "AnimEngine.h"
#include "HostProbe.h"
#include "ParamNames.h"
#include "Sampler.h"
#include "TransformMath.h"
#include "interact/OverlayInteract.h"
#include "render/CudaRender.h"

#include <memory>
#include <sstream>

#include "ofxDrawSuite.h"
#include "ofxsInteract.h"
#include "ofxsLog.h"
#include "ofxsProcessing.h"
#include "ofxsSupportPrivate.h"

using namespace mtx;

#define kSupportsTiles              false
#define kSupportsMultiResolution    false
#define kSupportsMultipleClipPARs   false

////////////////////////////////////////////////////////////////////////////////
// Processor
//
// Deriving from OFX::ImageProcessor buys two things: the host's GPU/CPU
// dispatch, and multi-threading of the CPU path (the base class slices the
// render window across cores). Both paths call the same header-only maths.

class TransformProcessor : public OFX::ImageProcessor
{
public:
    explicit TransformProcessor(OFX::ImageEffect& p_Instance)
        : OFX::ImageProcessor(p_Instance)
    { }

    void setSrcImg(OFX::Image* p_SrcImg) { _srcImg = p_SrcImg; }

    void setParams(const mtx::SampleTransforms& p_Transforms,
                   mtx::FilterMode p_Filter, mtx::EdgeMode p_Edge)
    {
        _transforms = p_Transforms;
        _filter     = p_Filter;
        _edge       = p_Edge;
    }

    virtual void processImagesCUDA() override;
    virtual void multiThreadProcessImages(OfxRectI p_ProcWindow) override;

private:
    /** @brief Describe the source image for the samplers. */
    mtx::ImageView srcView() const
    {
        const OfxRectI b = _srcImg->getBounds();
        mtx::ImageView v;
        v.data            = static_cast<const float*>(_srcImg->getPixelData());
        v.width           = b.x2 - b.x1;
        v.height          = b.y2 - b.y1;
        v.rowStrideFloats = _srcImg->getRowBytes() / static_cast<int>(sizeof(float));
        return v;
    }

    OFX::Image*            _srcImg = nullptr;
    mtx::SampleTransforms  _transforms{};
    mtx::FilterMode        _filter = mtx::kFilterBilinear;
    mtx::EdgeMode          _edge   = mtx::kEdgeBlack;
};

void TransformProcessor::processImagesCUDA()
{
    if (!_srcImg) return;

    const mtx::ImageView sv = srcView();

    const OfxRectI db = _dstImg->getBounds();
    const int dstWidth  = db.x2 - db.x1;
    const int dstHeight = db.y2 - db.y1;
    const int dstRowFloats = _dstImg->getRowBytes() / static_cast<int>(sizeof(float));

    RunMultiTransformCuda(_pCudaStream,
                          sv.data, sv.width, sv.height, sv.rowStrideFloats,
                          static_cast<float*>(_dstImg->getPixelData()),
                          dstWidth, dstHeight, dstRowFloats,
                          _transforms,
                          static_cast<int>(_filter), static_cast<int>(_edge));
}

void TransformProcessor::multiThreadProcessImages(OfxRectI p_ProcWindow)
{
    const OfxRectI db = _dstImg->getBounds();

    if (!_srcImg)
    {
        for (int y = p_ProcWindow.y1; y < p_ProcWindow.y2; ++y)
        {
            float* row = static_cast<float*>(_dstImg->getPixelAddress(p_ProcWindow.x1, y));
            if (!row) continue;
            for (int x = p_ProcWindow.x1; x < p_ProcWindow.x2; ++x, row += 4)
                row[0] = row[1] = row[2] = row[3] = 0.0f;
        }
        return;
    }

    const mtx::ImageView sv = srcView();

    for (int y = p_ProcWindow.y1; y < p_ProcWindow.y2; ++y)
    {
        if (_effect.abort()) break;

        float* dstPix = static_cast<float*>(_dstImg->getPixelAddress(p_ProcWindow.x1, y));
        if (!dstPix) continue;

        for (int x = p_ProcWindow.x1; x < p_ProcWindow.x2; ++x, dstPix += 4)
        {
            mtx::RenderPixel(sv, _transforms,
                             static_cast<float>(x - db.x1) + 0.5f,
                             static_cast<float>(y - db.y1) + 0.5f,
                             _filter, _edge, dstPix);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Effect instance

class MultiTransformPlugin : public OFX::ImageEffect
{
public:
    explicit MultiTransformPlugin(OfxImageEffectHandle p_Handle);

    virtual void render(const OFX::RenderArguments& p_Args) override;
    virtual bool isIdentity(const OFX::IsIdentityArguments& p_Args,
                            OFX::Clip*& p_IdentityClip, double& p_IdentityTime) override;
    virtual void changedParam(const OFX::InstanceChangedArgs& p_Args,
                              const std::string& p_ParamName) override;

    /** Gather every parameter into the plain structs the renderer consumes. */
    AnimParams fetchAnimParams(double p_Time) const;
    BlurParams fetchBlurParams(double p_Time) const;

    /** Source dimensions in pixels, which the transform maths is expressed in. */
    bool sourceSize(double p_Time, float& outW, float& outH) const;

private:
    void syncStageVisibility();
    void updateDuration(int stageIndex);
    void applyEasingPreset(int stageIndex);
    void markEasingCustom(int stageIndex);

    OFX::Clip* _dstClip = nullptr;
    OFX::Clip* _srcClip = nullptr;

    OFX::ChoiceParam*  _stageCount   = nullptr;
    OFX::ChoiceParam*  _activeStage  = nullptr;
    OFX::ChoiceParam*  _filter       = nullptr;
    OFX::ChoiceParam*  _edge         = nullptr;

    OFX::BooleanParam* _blurEnabled  = nullptr;
    OFX::DoubleParam*  _shutterAngle = nullptr;
    OFX::DoubleParam*  _shutterPhase = nullptr;
    OFX::IntParam*     _blurSamples  = nullptr;
    OFX::BooleanParam* _blurAdaptive = nullptr;

    struct StageParamHandles
    {
        OFX::StringParam*   lblTiming;
        OFX::StringParam*   lblFrom;
        OFX::StringParam*   lblTo;
        OFX::StringParam*   lblEasing;
        OFX::StringParam*   lblPath;
        OFX::Double2DParam* pathC1;
        OFX::Double2DParam* pathC2;
        OFX::PushButtonParam* pathReset;
        OFX::BooleanParam*  enabled;
        OFX::DoubleParam*   startFrame;
        OFX::DoubleParam*   endFrame;
        OFX::PushButtonParam* setStart;
        OFX::PushButtonParam* setEnd;
        OFX::DoubleParam*   duration;
        OFX::DoubleParam*   scaleFrom;
        OFX::DoubleParam*   scaleTo;
        OFX::Double2DParam* posFrom;
        OFX::Double2DParam* posTo;
        OFX::DoubleParam*   rotFrom;
        OFX::DoubleParam*   rotTo;
        OFX::DoubleParam*   opacityFrom;
        OFX::DoubleParam*   opacityTo;
        OFX::Double2DParam* anchor;
        OFX::ChoiceParam*   easingPreset;
        OFX::DoubleParam*   easeIn;
        OFX::DoubleParam*   easeOut;
        OFX::DoubleParam*   anticipation;
        OFX::DoubleParam*   overshoot;
        OFX::ChoiceParam*   bounceType;
        OFX::DoubleParam*   bounceAmount;
        OFX::DoubleParam*   bounceCount;
        OFX::DoubleParam*   bounceDamping;
        OFX::DoubleParam*   bounceStart;
    };
    StageParamHandles _stage[kMaxStages];

    /// Guards the preset <-> amounts sync from re-entering itself: stamping a
    /// preset writes the amounts, and writing an amount selects Custom.
    bool _syncingEasing = false;

    /// Diagnostic only: how many frames actually reached the renderer.
    long long _renderCount = 0;
};

MultiTransformPlugin::MultiTransformPlugin(OfxImageEffectHandle p_Handle)
    : OFX::ImageEffect(p_Handle)
{
    _dstClip = fetchClip(kOfxImageEffectOutputClipName);
    _srcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);

    _stageCount  = fetchChoiceParam(kParamStageCount);
    _activeStage = fetchChoiceParam(kParamActiveStage);
    _filter     = fetchChoiceParam(kParamFilter);
    _edge       = fetchChoiceParam(kParamEdge);

    _blurEnabled  = fetchBooleanParam(kParamBlurEnabled);
    _shutterAngle = fetchDoubleParam (kParamShutterAngle);
    _shutterPhase = fetchDoubleParam (kParamShutterPhase);
    _blurSamples  = fetchIntParam    (kParamBlurSamples);
    _blurAdaptive = fetchBooleanParam(kParamBlurAdaptive);

    for (int i = 0; i < kMaxStages; ++i)
    {
        StageParamHandles& s = _stage[i];
        s.lblTiming     = fetchStringParam  (StageParam(kParamLabelTiming,   i));
        s.lblFrom       = fetchStringParam  (StageParam(kParamLabelFrom,     i));
        s.lblTo         = fetchStringParam  (StageParam(kParamLabelTo,       i));
        s.lblEasing     = fetchStringParam  (StageParam(kParamLabelEasing,   i));
        s.lblPath       = fetchStringParam  (StageParam(kParamLabelPath,     i));
        s.pathC1        = fetchDouble2DParam(StageParam(kParamPathC1,        i));
        s.pathC2        = fetchDouble2DParam(StageParam(kParamPathC2,        i));
        s.pathReset     = fetchPushButtonParam(StageParam(kParamPathReset,   i));
        s.enabled       = fetchBooleanParam (StageParam(kParamEnabled,       i));
        s.startFrame    = fetchDoubleParam  (StageParam(kParamStartFrame,    i));
        s.endFrame      = fetchDoubleParam  (StageParam(kParamEndFrame,      i));
        s.setStart      = fetchPushButtonParam(StageParam(kParamSetStart,    i));
        s.setEnd        = fetchPushButtonParam(StageParam(kParamSetEnd,      i));
        s.duration      = fetchDoubleParam  (StageParam(kParamDuration,      i));
        s.scaleFrom     = fetchDoubleParam  (StageParam(kParamScaleFrom,     i));
        s.scaleTo       = fetchDoubleParam  (StageParam(kParamScaleTo,       i));
        s.posFrom       = fetchDouble2DParam(StageParam(kParamPosFrom,       i));
        s.posTo         = fetchDouble2DParam(StageParam(kParamPosTo,         i));
        s.rotFrom       = fetchDoubleParam  (StageParam(kParamRotFrom,       i));
        s.rotTo         = fetchDoubleParam  (StageParam(kParamRotTo,         i));
        s.opacityFrom   = fetchDoubleParam  (StageParam(kParamOpacityFrom,   i));
        s.opacityTo     = fetchDoubleParam  (StageParam(kParamOpacityTo,     i));
        s.anchor        = fetchDouble2DParam(StageParam(kParamAnchor,        i));
        s.easingPreset  = fetchChoiceParam  (StageParam(kParamEasingPreset,  i));
        s.easeIn        = fetchDoubleParam  (StageParam(kParamEaseIn,        i));
        s.easeOut       = fetchDoubleParam  (StageParam(kParamEaseOut,       i));
        s.anticipation  = fetchDoubleParam  (StageParam(kParamAnticipation,  i));
        s.overshoot     = fetchDoubleParam  (StageParam(kParamOvershoot,     i));
        s.bounceType    = fetchChoiceParam  (StageParam(kParamBounceType,    i));
        s.bounceAmount  = fetchDoubleParam  (StageParam(kParamBounceAmount,  i));
        s.bounceCount   = fetchDoubleParam  (StageParam(kParamBounceCount,   i));
        s.bounceDamping = fetchDoubleParam  (StageParam(kParamBounceDamping, i));
        s.bounceStart   = fetchDoubleParam  (StageParam(kParamBounceStart,   i));
    }

    mtx::ProbeHostOnce();

    // Secrecy must be established here rather than in describe(): some OFX hosts
    // permanently lock a parameter that was declared secret at describe time,
    // making it impossible to reveal later.
    syncStageVisibility();
    for (int i = 0; i < kMaxStages; ++i)
    {
        // Duration is derived from start/end, never authored, so it is shown
        // greyed out. Refresh it here so it is right when a saved project is
        // reopened, not only after the next edit.
        _stage[i].duration->setEnabled(false);
        updateDuration(i);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Parameter plumbing

AnimParams MultiTransformPlugin::fetchAnimParams(double p_Time) const
{
    AnimParams a = AnimParams::Default();

    int stageCountIndex = 0;
    _stageCount->getValueAtTime(p_Time, stageCountIndex);
    a.stageCount = stageCountIndex + 1;   // dropdown is 0-based, count is 1-based

    for (int i = 0; i < kMaxStages; ++i)
    {
        const StageParamHandles& h = _stage[i];
        Stage& s = a.stages[i];

        // getValueAtTime rather than getValue: the OFX spec requires it inside
        // render actions, and it costs nothing for our non-animated parameters.
        s.enabled       = h.enabled->getValueAtTime(p_Time);

        // Each read is a suite call into the host. Reading all four stages
        // unconditionally meant ~100 calls per frame per instance even when
        // only one stage was in use -- and with several layered clips each
        // running this effect, that overhead stops being free. A stage that
        // cannot contribute is left at its (identity) defaults instead.
        if (!s.enabled || i >= a.stageCount)
        {
            s.enabled = false;
            continue;
        }

        s.startFrame    = static_cast<float>(h.startFrame->getValueAtTime(p_Time));
        s.endFrame      = static_cast<float>(h.endFrame->getValueAtTime(p_Time));
        s.scaleFrom     = static_cast<float>(h.scaleFrom->getValueAtTime(p_Time));
        s.scaleTo       = static_cast<float>(h.scaleTo->getValueAtTime(p_Time));
        s.rotFrom       = static_cast<float>(h.rotFrom->getValueAtTime(p_Time));
        s.rotTo         = static_cast<float>(h.rotTo->getValueAtTime(p_Time));

        // Parameters are percentages for familiarity; the engine works in 0..1.
        s.opacityFrom   = static_cast<float>(h.opacityFrom->getValueAtTime(p_Time) * 0.01);
        s.opacityTo     = static_cast<float>(h.opacityTo->getValueAtTime(p_Time) * 0.01);

        double x = 0.0, y = 0.0;
        h.posFrom->getValueAtTime(p_Time, x, y);
        s.posXFrom = static_cast<float>(x); s.posYFrom = static_cast<float>(y);
        h.posTo->getValueAtTime(p_Time, x, y);
        s.posXTo   = static_cast<float>(x); s.posYTo   = static_cast<float>(y);
        h.anchor->getValueAtTime(p_Time, x, y);
        s.anchorX  = static_cast<float>(x); s.anchorY  = static_cast<float>(y);
        h.pathC1->getValueAtTime(p_Time, x, y);
        s.pathC1X  = static_cast<float>(x); s.pathC1Y  = static_cast<float>(y);
        h.pathC2->getValueAtTime(p_Time, x, y);
        s.pathC2X  = static_cast<float>(x); s.pathC2Y  = static_cast<float>(y);

        // The amounts are the single source of truth; the preset dropdown only
        // ever stamps values into them.
        int bounceType = kBounceNone;
        h.bounceType->getValueAtTime(p_Time, bounceType);

        s.easing = MakeEasing(static_cast<float>(h.easeIn->getValueAtTime(p_Time)),
                              static_cast<float>(h.easeOut->getValueAtTime(p_Time)),
                              static_cast<float>(h.anticipation->getValueAtTime(p_Time)),
                              static_cast<float>(h.overshoot->getValueAtTime(p_Time)),
                              bounceType,
                              static_cast<float>(h.bounceAmount->getValueAtTime(p_Time)),
                              static_cast<float>(h.bounceCount->getValueAtTime(p_Time)),
                              static_cast<float>(h.bounceDamping->getValueAtTime(p_Time)),
                              static_cast<float>(h.bounceStart->getValueAtTime(p_Time)));
    }

    return a;
}

BlurParams MultiTransformPlugin::fetchBlurParams(double p_Time) const
{
    BlurParams b   = BlurParams::Default();
    b.enabled      = _blurEnabled->getValueAtTime(p_Time);
    b.shutterAngle = static_cast<float>(_shutterAngle->getValueAtTime(p_Time));
    b.shutterPhase = static_cast<float>(_shutterPhase->getValueAtTime(p_Time));
    b.samples      = _blurSamples->getValueAtTime(p_Time);
    b.adaptive     = _blurAdaptive->getValueAtTime(p_Time);
    return b;
}

bool MultiTransformPlugin::sourceSize(double p_Time, float& outW, float& outH) const
{
    if (!_srcClip) return false;
    const OfxRectD rod = _srcClip->getRegionOfDefinition(p_Time);
    outW = static_cast<float>(rod.x2 - rod.x1);
    outH = static_cast<float>(rod.y2 - rod.y1);
    return outW > 0.0f && outH > 0.0f;
}

void MultiTransformPlugin::syncStageVisibility()
{
    int countIdx = 0;
    _stageCount->getValue(countIdx);
    const int count = countIdx + 1;

    int active = 0;
    _activeStage->getValue(active);

    // Keep the selection inside the stages that actually exist, otherwise
    // reducing Stage Count would leave the Inspector showing nothing.
    if (active >= count)
    {
        active = count - 1;
        _activeStage->setValue(active);
    }
    if (active < 0) active = 0;

    // Only the selected stage is shown. Four stages' worth of controls at once
    // is unreadable, and the Active Stage dropdown (or a click on a timeline
    // lane in the overlay) is a cheaper way to move between them.
    for (int i = 0; i < kMaxStages; ++i)
    {
        const bool hidden = (i != active);
        StageParamHandles& s = _stage[i];

        s.lblTiming->setIsSecret(hidden);
        s.lblFrom->setIsSecret(hidden);
        s.lblTo->setIsSecret(hidden);
        s.lblEasing->setIsSecret(hidden);
        s.lblPath->setIsSecret(hidden);
        s.pathC1->setIsSecret(hidden);
        s.pathC2->setIsSecret(hidden);
        s.pathReset->setIsSecret(hidden);

        s.enabled->setIsSecret(hidden);
        s.setStart->setIsSecret(hidden);
        s.setEnd->setIsSecret(hidden);
        s.startFrame->setIsSecret(hidden);
        s.endFrame->setIsSecret(hidden);
        s.duration->setIsSecret(hidden);
        s.anchor->setIsSecret(hidden);

        s.scaleFrom->setIsSecret(hidden);
        s.posFrom->setIsSecret(hidden);
        s.rotFrom->setIsSecret(hidden);
        s.opacityFrom->setIsSecret(hidden);

        s.scaleTo->setIsSecret(hidden);
        s.posTo->setIsSecret(hidden);
        s.rotTo->setIsSecret(hidden);
        s.opacityTo->setIsSecret(hidden);

        s.easingPreset->setIsSecret(hidden);
        s.easeIn->setIsSecret(hidden);
        s.easeOut->setIsSecret(hidden);
        s.anticipation->setIsSecret(hidden);
        s.overshoot->setIsSecret(hidden);
        s.bounceType->setIsSecret(hidden);

        // Two rules decide these: the stage must be the active one, AND a bounce
        // type must be selected. Combined in one place deliberately -- as two
        // independent passes they would overwrite each other and leave a control
        // stranded hidden.
        int bounceType = kBounceNone;
        s.bounceType->getValue(bounceType);
        const bool bounceHidden = hidden || (bounceType == kBounceNone);

        s.bounceAmount->setIsSecret(bounceHidden);
        s.bounceCount->setIsSecret(bounceHidden);
        s.bounceDamping->setIsSecret(bounceHidden);
        s.bounceStart->setIsSecret(bounceHidden);
    }
}

void MultiTransformPlugin::applyEasingPreset(int stageIndex)
{
    int preset = 0;
    _stage[stageIndex].easingPreset->getValue(preset);
    if (preset == kEasingCustom) return;   // Custom means "leave my values alone"

    const EasingPresetValues v = PresetValues(preset);

    _syncingEasing = true;
    _stage[stageIndex].easeIn->setValue(v.easeIn);
    _stage[stageIndex].easeOut->setValue(v.easeOut);
    _stage[stageIndex].anticipation->setValue(v.anticipation);
    _stage[stageIndex].overshoot->setValue(v.overshoot);
    _stage[stageIndex].bounceType->setValue(v.bounceType);
    _stage[stageIndex].bounceAmount->setValue(v.bounceAmount);
    _stage[stageIndex].bounceCount->setValue(v.bounceCount);
    _stage[stageIndex].bounceDamping->setValue(v.bounceDamping);
    _stage[stageIndex].bounceStart->setValue(v.bounceStart);
    _syncingEasing = false;

    // The preset may have switched the bounce type on or off, which changes
    // which bounce controls are relevant.
    syncStageVisibility();
}

void MultiTransformPlugin::markEasingCustom(int stageIndex)
{
    // Hand-adjusting any amount means the curve no longer matches the named
    // preset, so the dropdown must stop claiming that it does.
    if (_syncingEasing) return;

    int preset = 0;
    _stage[stageIndex].easingPreset->getValue(preset);
    if (preset == kEasingCustom) return;

    _syncingEasing = true;
    _stage[stageIndex].easingPreset->setValue(kEasingCustom);
    _syncingEasing = false;
}

void MultiTransformPlugin::updateDuration(int stageIndex)
{
    const double start = _stage[stageIndex].startFrame->getValue();
    const double end   = _stage[stageIndex].endFrame->getValue();
    _stage[stageIndex].duration->setValue(end - start);
}

void MultiTransformPlugin::changedParam(const OFX::InstanceChangedArgs& p_Args,
                                        const std::string& p_ParamName)
{
    // Both the count and the selection change which stage's controls are shown.
    // Active Stage is also written by the overlay's stage tabs and timeline
    // lanes, so this keeps the Inspector following what is clicked in the viewer.
    if (p_ParamName == kParamStageCount || p_ParamName == kParamActiveStage)
    {
        syncStageVisibility();
        return;
    }

    for (int i = 0; i < kMaxStages; ++i)
    {
        // Capturing the playhead is the only way to learn a timeline position:
        // Resolve hands the plugin timeline-absolute times and no usable clip
        // range, so park-and-click beats typing frame numbers.
        if (p_ParamName == StageParam(kParamSetStart, i))
        {
            _stage[i].startFrame->setValue(p_Args.time);
            updateDuration(i);
            return;
        }
        if (p_ParamName == StageParam(kParamSetEnd, i))
        {
            _stage[i].endFrame->setValue(p_Args.time);
            updateDuration(i);
            return;
        }
        if (p_ParamName == StageParam(kParamStartFrame, i) ||
            p_ParamName == StageParam(kParamEndFrame, i))
        {
            updateDuration(i);
            return;
        }
        if (p_ParamName == StageParam(kParamEasingPreset, i))
        {
            applyEasingPreset(i);
            return;
        }
        if (p_ParamName == StageParam(kParamPathReset, i))
        {
            // Zero offsets restore an exactly straight line, not merely a
            // nearly straight one -- see PathControlPoints.
            _stage[i].pathC1->setValue(0.0, 0.0);
            _stage[i].pathC2->setValue(0.0, 0.0);
            return;
        }
        if (p_ParamName == StageParam(kParamBounceType, i))
        {
            markEasingCustom(i);
            syncStageVisibility();   // reveals or hides the bounce amounts
            return;
        }
        if (p_ParamName == StageParam(kParamEaseIn, i)        ||
            p_ParamName == StageParam(kParamEaseOut, i)       ||
            p_ParamName == StageParam(kParamAnticipation, i)  ||
            p_ParamName == StageParam(kParamOvershoot, i)     ||
            p_ParamName == StageParam(kParamBounceAmount, i)  ||
            p_ParamName == StageParam(kParamBounceCount, i)   ||
            p_ParamName == StageParam(kParamBounceDamping, i))
        {
            markEasingCustom(i);
            return;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Render

bool MultiTransformPlugin::isIdentity(const OFX::IsIdentityArguments& p_Args,
                                      OFX::Clip*& p_IdentityClip, double& p_IdentityTime)
{
    // If the composed transform is the identity there is nothing to do, and
    // saying so lets Resolve skip the effect entirely.
    float w = 0.0f, h = 0.0f;
    if (!sourceSize(p_Args.time, w, h)) return false;

    // Every shutter sample must be the identity, not just the one at frame
    // centre: a move that is momentarily at rest still blurs if it is moving
    // either side of the shutter.
    const SampleTransforms st = BuildSampleTransforms(fetchAnimParams(p_Args.time),
                                                      fetchBlurParams(p_Args.time),
                                                      static_cast<float>(p_Args.time), w, h);

    const Mat3 id = Mat3::Identity();
    for (int k = 0; k < st.count; ++k)
    {
        // A fade is a real change even when nothing moves, so opacity has to be
        // part of the identity test or fades would be silently optimised away.
        if (std::fabs(st.opacity[k] - 1.0f) > 1e-4f) return false;

        for (int i = 0; i < 6; ++i)
        {
            // Tolerance is in pixels for the translation terms, so keep it tight.
            if (std::fabs(st.inv[k].m[i] - id.m[i]) > 1e-4f) return false;
        }
    }

    // Worth knowing whether the host honours this at all: if identity frames
    // are still being rendered, no amount of kernel tuning will help.
    mtx::ProbeOnce("first-identity-skip",
                   "isIdentity: returned true -- host may skip this render entirely");

    p_IdentityClip = _srcClip;
    p_IdentityTime = p_Args.time;
    return true;
}

void MultiTransformPlugin::render(const OFX::RenderArguments& p_Args)
{
    std::unique_ptr<OFX::Image> dst(_dstClip ? _dstClip->fetchImage(p_Args.time) : nullptr);
    std::unique_ptr<OFX::Image> src(_srcClip ? _srcClip->fetchImage(p_Args.time) : nullptr);

    if (!dst)
    {
        OFX::throwSuiteStatusException(kOfxStatFailed);
        return;
    }

    mtx::ProbeOnce("first-gpu-state",
                   std::string("render dispatch: cuda=") +
                   (p_Args.isEnabledCudaRender ? "yes" : "no") +
                   " opencl=" + (p_Args.isEnabledOpenCLRender ? "yes" : "no"));

    // Count renders that actually did work, so the log distinguishes "the host
    // skipped it" from "we rendered a frame that produced nothing new".
    _renderCount++;

    int filterIdx = kFilterBilinear;
    int edgeIdx   = kEdgeBlack;
    _filter->getValueAtTime(p_Args.time, filterIdx);
    _edge->getValueAtTime(p_Args.time, edgeIdx);

    // Built once per render, not per pixel: the shutter matrices are identical
    // for every pixel, and re-running the easing solver millions of times would
    // dominate the cost of the blur.
    SampleTransforms st;
    st.count      = 1;
    st.inv[0]     = Mat3::Identity();
    st.opacity[0] = 1.0f;

    if (src)
    {
        const OfxRectI sb = src->getBounds();
        st = BuildSampleTransforms(fetchAnimParams(p_Args.time),
                                   fetchBlurParams(p_Args.time),
                                   static_cast<float>(p_Args.time),
                                   static_cast<float>(sb.x2 - sb.x1),
                                   static_cast<float>(sb.y2 - sb.y1));
    }

    TransformProcessor processor(*this);
    processor.setDstImg(dst.get());
    processor.setSrcImg(src.get());
    processor.setGPURenderArgs(p_Args);
    processor.setRenderWindow(p_Args.renderWindow);
    processor.setParams(st,
                        static_cast<FilterMode>(filterIdx),
                        static_cast<EdgeMode>(edgeIdx));

    processor.process();
}

////////////////////////////////////////////////////////////////////////////////
// Factory

using namespace OFX;

MultiTransformPluginFactory::MultiTransformPluginFactory()
    : OFX::PluginFactoryHelper<MultiTransformPluginFactory>(kPluginIdentifier,
                                                            kPluginVersionMajor,
                                                            kPluginVersionMinor)
{
}

void MultiTransformPluginFactory::describe(OFX::ImageEffectDescriptor& p_Desc)
{
    p_Desc.setLabels(kPluginName, kPluginName, kPluginName);
    p_Desc.setPluginGrouping(kPluginGrouping);
    p_Desc.setPluginDescription(kPluginDescription);

    p_Desc.addSupportedContext(eContextFilter);
    p_Desc.addSupportedContext(eContextGeneral);
    p_Desc.addSupportedBitDepth(eBitDepthFloat);

    p_Desc.setSingleInstance(false);
    p_Desc.setHostFrameThreading(false);
    p_Desc.setSupportsMultiResolution(kSupportsMultiResolution);
    p_Desc.setSupportsTiles(kSupportsTiles);
    p_Desc.setTemporalClipAccess(false);
    p_Desc.setRenderTwiceAlways(false);
    p_Desc.setSupportsMultipleClipPARs(kSupportsMultipleClipPARs);

    // A transform samples from arbitrary source locations, so it is spatially
    // aware and must not be run during LUT generation.
    p_Desc.setNoSpatialAwareness(false);

    // CUDA only. Phase 0's probe measured supportsOpenCLRender = NO on this
    // host (an NVIDIA GPU makes Resolve choose CUDA), so an OpenCL path could
    // not be tested and was dropped rather than shipped unverified.
    p_Desc.setSupportsCudaRender(true);
    p_Desc.setSupportsCudaStream(true);

    p_Desc.setOverlayInteractDescriptor(new mtx::MultiTransformOverlayDescriptor());
}

namespace {

DoubleParamDescriptor* DefineDouble(OFX::ImageEffectDescriptor& desc, PageParamDescriptor* page,
                                    GroupParamDescriptor* parent, const std::string& name,
                                    const std::string& label, const std::string& hint,
                                    double def, double lo, double hi,
                                    double dispLo, double dispHi, double inc)
{
    DoubleParamDescriptor* p = desc.defineDoubleParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    p->setDefault(def);
    p->setRange(lo, hi);
    p->setDisplayRange(dispLo, dispHi);
    p->setIncrement(inc);
    if (parent) p->setParent(*parent);
    page->addChild(*p);
    return p;
}

Double2DParamDescriptor* DefineDouble2D(OFX::ImageEffectDescriptor& desc, PageParamDescriptor* page,
                                        GroupParamDescriptor* parent, const std::string& name,
                                        const std::string& label, const std::string& hint,
                                        double defX, double defY)
{
    Double2DParamDescriptor* p = desc.defineDouble2DParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    p->setDefault(defX, defY);
    p->setRange(-100.0, -100.0, 100.0, 100.0);
    p->setDisplayRange(-2.0, -2.0, 2.0, 2.0);
    p->setIncrement(0.001);
    if (parent) p->setParent(*parent);
    page->addChild(*p);
    return p;
}

/** @brief A section heading: a static label used purely as a visual divider.
 *
 * Deliberately not a group parameter. Groups add a collapsible section with a
 * disclosure arrow, and four stages' worth of them turned the Inspector into a
 * wall of things to click open. A label just separates the controls beneath it.
 */
StringParamDescriptor* DefineDivider(OFX::ImageEffectDescriptor& desc, PageParamDescriptor* page,
                                     const std::string& name, const std::string& text)
{
    StringParamDescriptor* p = desc.defineStringParam(name);
    p->setStringType(eStringTypeLabel);
    p->setDefault(text);
    p->setLabels("", "", "");
    p->setEnabled(false);
    p->setIsPersistant(false);   // derived decoration, nothing to save
    page->addChild(*p);
    return p;
}

void DefineStage(OFX::ImageEffectDescriptor& desc, PageParamDescriptor* page, int i)
{
    const std::string idx = std::to_string(i + 1);

    // Only one stage is ever visible at a time, so the controls sit directly on
    // the page divided by headings rather than inside nested collapsible groups.

    // --- Timing ---
    DefineDivider(desc, page, StageParam(kParamLabelTiming, i),
                  "\xE2\x80\x94  STAGE " + idx + " : TIMING  \xE2\x80\x94");

    BooleanParamDescriptor* en = desc.defineBooleanParam(StageParam(kParamEnabled, i));
    en->setLabels("Enabled", "Enabled", "Enabled");
    en->setHint("Include this stage in the combined transform. Stages COMBINE rather than "
                "replace: two stages each scaling 1.0 to 1.5 give 2.25x overall.");
    en->setDefault(i == 0);
    page->addChild(*en);

    PushButtonParamDescriptor* setStart = desc.definePushButtonParam(StageParam(kParamSetStart, i));
    setStart->setLabels("Set Start to Playhead", "Set Start", "Set Start to Playhead");
    setStart->setHint("Park the playhead where this stage should begin and click.");
    page->addChild(*setStart);

    PushButtonParamDescriptor* setEnd = desc.definePushButtonParam(StageParam(kParamSetEnd, i));
    setEnd->setLabels("Set End to Playhead", "Set End", "Set End to Playhead");
    setEnd->setHint("Park the playhead where this stage should finish and click.");
    page->addChild(*setEnd);

    DefineDouble(desc, page, nullptr, StageParam(kParamStartFrame, i), "Start Frame",
                 "Timeline frame where this stage begins. Normally set with the button "
                 "above rather than typed. Staggering is just giving stages different "
                 "start frames: 100 here and 106 on the next stage is a six-frame stagger.",
                 0.0, 0.0, 1e9, 0.0, 100000.0, 1.0);

    DefineDouble(desc, page, nullptr, StageParam(kParamEndFrame, i), "End Frame",
                 "Timeline frame where this stage finishes. Stages may differ in length as "
                 "well as in start time.",
                 24.0, 0.0, 1e9, 0.0, 100000.0, 1.0);

    DefineDouble(desc, page, nullptr, StageParam(kParamDuration, i), "Duration (frames)",
                 "End Frame minus Start Frame. Calculated automatically -- shown for "
                 "reference, not editable.",
                 24.0, -1e9, 1e9, 0.0, 240.0, 1.0);

    DefineDouble2D(desc, page, nullptr, StageParam(kParamAnchor, i), "Anchor",
                   "Point that scale and rotation pivot around. 0.5, 0.5 is the image centre. "
                   "Shared by both ends -- a pivot that moved mid-move would make the motion "
                   "impossible to reason about.",
                   0.5, 0.5);

    // --- From / To ---
    // Split so a pose reads top to bottom in one block, instead of picking every
    // other row out of an interleaved "Scale From / Scale To / Position From..."
    // list.
    struct EndSpec { const char* labelName; const char* heading; bool isTo; };
    const EndSpec ends[2] = {
        { kParamLabelFrom, "FROM (START)", false },
        { kParamLabelTo,   "TO (END)",     true  }
    };

    for (const EndSpec& end : ends)
    {
        DefineDivider(desc, page, StageParam(end.labelName, i),
                      std::string("\xE2\x80\x94  ") + end.heading + "  \xE2\x80\x94");

        DefineDouble(desc, page, nullptr,
                     StageParam(end.isTo ? kParamScaleTo : kParamScaleFrom, i), "Scale",
                     "Uniform scale multiplier.", 1.0, -100.0, 100.0, 0.0, 4.0, 0.01);

        DefineDouble2D(desc, page, nullptr,
                       StageParam(end.isTo ? kParamPosTo : kParamPosFrom, i), "Position",
                       "Offset, normalised: 1.0 is one full image width/height.", 0.0, 0.0);

        DefineDouble(desc, page, nullptr,
                     StageParam(end.isTo ? kParamRotTo : kParamRotFrom, i), "Rotation",
                     "Rotation in degrees.", 0.0, -100000.0, 100000.0, -360.0, 360.0, 1.0);

        DefineDouble(desc, page, nullptr,
                     StageParam(end.isTo ? kParamOpacityTo : kParamOpacityFrom, i), "Opacity",
                     "Fade level as a percentage. Set From to 0 and To to 100 for a fade in, "
                     "or the reverse for a fade out. Opacity animates on this stage's own "
                     "timing and easing, so a fade can be staggered against the movement.",
                     100.0, 0.0, 100.0, 0.0, 100.0, 1.0);
    }

    // --- Motion path ---
    // The path belongs to the stage as a whole rather than to either end, since
    // it is the route between them.
    DefineDivider(desc, page, StageParam(kParamLabelPath, i),
                  "\xE2\x80\x94  MOTION PATH  \xE2\x80\x94");

    DefineDouble2D(desc, page, nullptr, StageParam(kParamPathC1, i), "Path Handle 1",
                   "Bends the first part of the trajectory away from a straight line. Easier "
                   "to drag on screen than to type: enable Open FX Overlay and pull the "
                   "handles on the dotted path. 0,0 is a straight line.",
                   0.0, 0.0);

    DefineDouble2D(desc, page, nullptr, StageParam(kParamPathC2, i), "Path Handle 2",
                   "Bends the second part of the trajectory. 0,0 is a straight line.",
                   0.0, 0.0);

    PushButtonParamDescriptor* pathReset = desc.definePushButtonParam(StageParam(kParamPathReset, i));
    pathReset->setLabels("Straighten Path", "Straighten", "Straighten Path");
    pathReset->setHint("Reset both handles, returning the motion to a straight line.");
    page->addChild(*pathReset);

    // --- Easing ---
    DefineDivider(desc, page, StageParam(kParamLabelEasing, i),
                  "\xE2\x80\x94  EASING  \xE2\x80\x94");

    ChoiceParamDescriptor* ease = desc.defineChoiceParam(StageParam(kParamEasingPreset, i));
    ease->setLabels("Easing", "Easing", "Easing");
    ease->setHint("A starting point. Picking one fills in the four amounts below, which you "
                  "are then free to adjust -- doing so switches this to Custom.");
    ease->appendOption("Linear");
    ease->appendOption("Smooth (Ease In-Out)");
    ease->appendOption("Ease In");
    ease->appendOption("Ease Out");
    ease->appendOption("Overshoot (Back)");
    ease->appendOption("Spring");
    ease->appendOption("Bounce");
    ease->appendOption("Custom");
    ease->setDefault(kEasingSmooth);
    page->addChild(*ease);

    // These four are the actual curve, and are always editable -- a preset is a
    // starting point, never a locked choice. Raw bezier handles (X1/Y1/X2/Y2)
    // were unusable as a UI: nobody thinks in terms of "X1 = 0.42".
    DefineDouble(desc, page, nullptr, StageParam(kParamEaseIn, i), "Ease In",
                 "Damping at the start. 0 leaves at full speed; 100 creeps away very "
                 "gradually. Raise this to soften the beginning of the move. Setting both "
                 "Ease In and Ease Out to 100 gives the steepest S-curve available.",
                 42.0, 0.0, 100.0, 0.0, 100.0, 1.0);

    DefineDouble(desc, page, nullptr, StageParam(kParamEaseOut, i), "Ease Out",
                 "Damping at the end. 0 stops dead; 100 glides to a halt. This is the one "
                 "to reach for when a move feels like it arrives too abruptly. Still applies "
                 "when a Bounce is active: it shapes the approach into the landing.",
                 42.0, 0.0, 100.0, 0.0, 100.0, 1.0);

    // Negative values are meaningful and deliberately allowed: they push the
    // curve handle past the opposite rail, which is how the steep, snappy
    // curves are made. Clamping these to positive-only left half the curve
    // space unreachable.
    DefineDouble(desc, page, nullptr, StageParam(kParamAnticipation, i), "Anticipation",
                 "Pulls back before moving, the way a character crouches before jumping. "
                 "0 is off. Negative values do the opposite: the move leaves hard and fast, "
                 "which steepens the start.",
                 0.0, -200.0, 200.0, -100.0, 100.0, 1.0);

    DefineDouble(desc, page, nullptr, StageParam(kParamOvershoot, i), "Overshoot",
                 "Travels past the target and settles back; around 80 gives the classic "
                 "springy 'back' ease. 0 is off. Negative values undershoot instead, "
                 "creeping up to the target from below. This is a single smooth overshoot -- "
                 "for repeated rebounds use Bounce below, which supersedes this.",
                 0.0, -200.0, 200.0, -100.0, 100.0, 1.0);

    // Bounce. A bezier curve is a cubic, so it has at most one overshoot and one
    // undershoot in it -- repeated rebounds are mathematically out of reach for
    // any handle position, and need this separate oscillation instead.
    ChoiceParamDescriptor* bounce = desc.defineChoiceParam(StageParam(kParamBounceType, i));
    bounce->setLabels("Bounce", "Bounce", "Bounce");
    bounce->setHint("Adds repeated rebounds after the move, which the Overshoot handle alone "
                    "cannot do. Spring settles through the target, above then below. Bounce "
                    "rebounds off the target like a ball off a floor, never passing it.");
    bounce->appendOption("None");
    bounce->appendOption("Spring (settles through target)");
    bounce->appendOption("Ball (rebounds off target)");
    bounce->setDefault(kBounceNone);
    page->addChild(*bounce);

    DefineDouble(desc, page, nullptr, StageParam(kParamBounceAmount, i), "Bounce Amount",
                 "How far the first rebound travels, as a fraction of the whole move. 0 is no "
                 "bounce at all. Negative values flip which way it bounces: a spring "
                 "undershoots before it overshoots, and a ball rebounds off the near side of "
                 "the target rather than the far side. Reach for a negative value when the "
                 "bounce is going the wrong way for the move. Either way the move still lands "
                 "exactly on its target value.",
                 35.0, -100.0, 100.0, -100.0, 100.0, 1.0);

    DefineDouble(desc, page, nullptr, StageParam(kParamBounceCount, i), "Bounces",
                 "How many rebounds happen before the move settles. Fractional values are "
                 "allowed and are useful for landing mid-rebound.",
                 3.0, 0.0, 12.0, 1.0, 8.0, 0.5);

    DefineDouble(desc, page, nullptr, StageParam(kParamBounceDamping, i), "Bounce Damping",
                 "How quickly the rebounds shrink. 0 keeps them all the same size, which "
                 "reads as mechanical; higher values decay them away for a natural settle.",
                 45.0, 0.0, 100.0, 0.0, 100.0, 1.0);

    DefineDouble(desc, page, nullptr, StageParam(kParamBounceStart, i), "Bounce Start",
                 "Where in the stage the move lands and the bouncing begins, as a percentage "
                 "of its duration. The easing curve is compressed into the part before this, "
                 "and everything after it is the bounce -- so lower values give a quicker "
                 "arrival and a longer bounce.",
                 55.0, 5.0, 95.0, 5.0, 95.0, 1.0);
}

} // namespace

void MultiTransformPluginFactory::describeInContext(OFX::ImageEffectDescriptor& p_Desc,
                                                    OFX::ContextEnum /*p_Context*/)
{
    ClipDescriptor* srcClip = p_Desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    ClipDescriptor* dstClip = p_Desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->setSupportsTiles(kSupportsTiles);

    PageParamDescriptor* page = p_Desc.definePageParam("Controls");

    ChoiceParamDescriptor* count = p_Desc.defineChoiceParam(kParamStageCount);
    count->setLabels("Stage Count", "Stage Count", "Stage Count");
    count->setHint("How many transform stages to use. Each stage is a full transform with "
                   "its own start and end frame; surplus stages are hidden.");
    for (int i = 1; i <= kMaxStages; ++i) count->appendOption(std::to_string(i));
    count->setDefault(0);   // one stage
    page->addChild(*count);

    // Overlay state, kept as parameters so it survives save/reload and so the
    // Inspector and the viewer overlay can never disagree about which stage is
    // being edited.
    ChoiceParamDescriptor* active = p_Desc.defineChoiceParam(kParamActiveStage);
    active->setLabels("Active Stage", "Active Stage", "Active Stage");
    active->setHint("Which stage the on-screen gizmo and curve editor act on. Also set by "
                    "clicking a stage tab or timeline lane in the viewer overlay.");
    for (int i = 1; i <= kMaxStages; ++i) active->appendOption("Stage " + std::to_string(i));
    active->setDefault(0);
    page->addChild(*active);

    ChoiceParamDescriptor* target = p_Desc.defineChoiceParam(kParamEditTarget);
    target->setLabels("Gizmo Edits", "Gizmo Edits", "Gizmo Edits");
    target->setHint("Whether the on-screen gizmo poses the start or the end of the active "
                    "stage.");
    target->appendOption("From (start)");
    target->appendOption("To (end)");
    target->setDefault(1);
    page->addChild(*target);

    BooleanParamDescriptor* showCurve = p_Desc.defineBooleanParam(kParamShowCurve);
    showCurve->setLabels("Show Curve Editor", "Curve Editor", "Show Curve Editor");
    showCurve->setHint("Draw the easing curve panel in the viewer overlay. Turn it off when it "
                       "sits over something you are trying to drag -- it covers the top-right "
                       "of the image and takes clicks before the motion path does. Also "
                       "toggled by the CURVE button in the overlay.");
    showCurve->setDefault(true);
    page->addChild(*showCurve);

    // --- Stages ---
    for (int i = 0; i < kMaxStages; ++i) DefineStage(p_Desc, page, i);

    // --- Motion blur ---
    GroupParamDescriptor* blur = p_Desc.defineGroupParam("motionBlurGroup");
    blur->setLabels("Motion Blur", "Motion Blur", "Motion Blur");
    blur->setHint("Blur generated from the animation itself. Because the plugin owns its "
                  "animation it can evaluate the transform between frames, so this is a true "
                  "analytic blur rather than a blend of neighbouring frames.");
    page->addChild(*blur);

    BooleanParamDescriptor* blurOn = p_Desc.defineBooleanParam(kParamBlurEnabled);
    blurOn->setLabels("Enable Motion Blur", "Motion Blur", "Enable Motion Blur");
    blurOn->setDefault(false);
    blurOn->setParent(*blur);
    page->addChild(*blurOn);

    DefineDouble(p_Desc, page, blur, kParamShutterAngle, "Shutter Angle",
                 "How much of each frame the shutter is open for. 180 is the film "
                 "convention; 360 blurs across a whole frame; 0 disables the blur.",
                 180.0, 0.0, 720.0, 0.0, 360.0, 5.0);

    DefineDouble(p_Desc, page, blur, kParamShutterPhase, "Shutter Phase",
                 "Shifts the shutter interval relative to the frame. 0 centres it; "
                 "negative values bias the blur towards where the image came from.",
                 0.0, -360.0, 360.0, -180.0, 180.0, 5.0);

    IntParamDescriptor* samples = p_Desc.defineIntParam(kParamBlurSamples);
    samples->setLabels("Samples", "Samples", "Samples");
    samples->setHint("Number of shutter samples. More is smoother but slower; with Adaptive "
                     "Samples on this acts as an upper bound rather than a fixed cost.");
    samples->setDefault(16);
    samples->setRange(1, mtx::kMaxBlurSamples);
    samples->setDisplayRange(1, mtx::kMaxBlurSamples);
    samples->setParent(*blur);
    page->addChild(*samples);

    BooleanParamDescriptor* adaptive = p_Desc.defineBooleanParam(kParamBlurAdaptive);
    adaptive->setLabels("Adaptive Samples", "Adaptive", "Adaptive Samples");
    adaptive->setHint("Scale the sample count with how far the image actually moves, so "
                      "slow or static frames cost almost nothing while fast ones stay "
                      "smooth. Turn off only if you need a fixed, predictable cost.");
    adaptive->setDefault(true);
    adaptive->setParent(*blur);
    page->addChild(*adaptive);

    // --- Sampling ---
    GroupParamDescriptor* sampling = p_Desc.defineGroupParam("sampling");
    sampling->setLabels("Sampling", "Sampling", "Sampling");
    page->addChild(*sampling);

    ChoiceParamDescriptor* filter = p_Desc.defineChoiceParam(kParamFilter);
    filter->setLabels("Filtering", "Filtering", "Filtering");
    filter->appendOption("Nearest");
    filter->appendOption("Bilinear");
    filter->setDefault(kFilterBilinear);
    filter->setParent(*sampling);
    page->addChild(*filter);

    ChoiceParamDescriptor* edge = p_Desc.defineChoiceParam(kParamEdge);
    edge->setLabels("Edges", "Edges", "Edges");
    edge->setHint("What to show where the transformed image no longer covers the frame.");
    edge->appendOption("Transparent");
    edge->appendOption("Clamp");
    edge->appendOption("Mirror");
    edge->setDefault(kEdgeBlack);
    edge->setParent(*sampling);
    page->addChild(*edge);
}

ImageEffect* MultiTransformPluginFactory::createInstance(OfxImageEffectHandle p_Handle,
                                                         ContextEnum /*p_Context*/)
{
    return new MultiTransformPlugin(p_Handle);
}

void OFX::Plugin::getPluginIDs(PluginFactoryArray& p_FactoryArray)
{
    static MultiTransformPluginFactory multiTransformPlugin;
    p_FactoryArray.push_back(&multiTransformPlugin);
}
