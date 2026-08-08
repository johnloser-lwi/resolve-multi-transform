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
#include "ClipTime.h"
#include "EditBlock.h"
#include "HostProbe.h"
#include "Preset.h"
#include "CurvePreset.h"
#include "PresetIO.h"
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

    // Whether the host supplies a stream decides who synchronises, so it is
    // worth knowing which world we are in when a render looks wrong.
    mtx::ProbeOnce("cuda-stream",
                   std::string("CUDA render: host stream = ") +
                   (_pCudaStream ? "provided (host synchronises)"
                                 : "NULL (plugin synchronises)"));

    const char* err =
        RunMultiTransformCuda(_pCudaStream,
                              sv.data, sv.width, sv.height, sv.rowStrideFloats,
                              static_cast<float*>(_dstImg->getPixelData()),
                              dstWidth, dstHeight, dstRowFloats,
                              _transforms,
                              static_cast<int>(_filter), static_cast<int>(_edge));

    if (err) mtx::ProbeOnce("cuda-error", std::string("CUDA error: ") + err);
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
    virtual void getClipPreferences(OFX::ClipPreferencesSetter& p_ClipPreferences) override;
    virtual void changedClip(const OFX::InstanceChangedArgs& p_Args,
                             const std::string& p_ClipName) override;

    /** Gather every parameter into the plain structs the renderer consumes. */
    AnimParams fetchAnimParams(double p_Time) const;
    BlurParams fetchBlurParams(double p_Time) const;

    /** Source dimensions in pixels, which the transform maths is expressed in. */
    bool sourceSize(double p_Time, float& outW, float& outH) const;

private:
    void syncStageVisibility();
    void migrateTimingIfNeeded();

    // --- Presets ---
    mtx::PresetStage captureStage(int stageIndex) const;
    mtx::PresetData  capturePreset(bool wholeEffect) const;
    void             applyStage(int stageIndex, const mtx::PresetStage& s);
    void             applyPreset(const mtx::PresetData& d);

    /// Refresh the preset-folder read-out from preferences.
    void             syncPresetFolderLabel();

    /// Write stage @p i's easing into the curve library.
    void             saveCurve(int stage);

    /** @brief Move poses between a stage's two ends.
     *  @param mode 0 copies From onto To, 1 copies To onto From, 2 swaps them. */
    void             transferEnds(int stage, int mode);
    void             savePreset(bool wholeEffect);
    void             loadPreset(bool fitToClip);
    /** The playhead expressed in the units a given stage's frames use. */
    double playheadInStageFrames(int stageIndex, double absoluteTime) const;
    void updateDuration(int stageIndex);
    void applyEasingPreset(int stageIndex);
    void markEasingCustom(int stageIndex);

    OFX::Clip* _dstClip = nullptr;
    OFX::Clip* _srcClip = nullptr;

    OFX::ChoiceParam*  _stageCount   = nullptr;
    OFX::ChoiceParam*  _activeStage  = nullptr;
    OFX::ChoiceParam*  _filter       = nullptr;
    OFX::ChoiceParam*  _edge         = nullptr;

    // Base transform: a static pose composed under the animation.
    OFX::DoubleParam*   _baseScale     = nullptr;
    OFX::DoubleParam*   _baseScaleY    = nullptr;
    OFX::BooleanParam*  _baseLinkScale = nullptr;
    OFX::Double2DParam* _basePos       = nullptr;
    OFX::DoubleParam*   _baseRot       = nullptr;
    OFX::DoubleParam*   _baseTiltX     = nullptr;
    OFX::DoubleParam*   _baseSwivelY   = nullptr;
    OFX::DoubleParam*   _baseOpacity   = nullptr;
    OFX::Double2DParam* _baseAnchor    = nullptr;

    /// Read-out of the preference, refreshed from settings.json rather than
    /// stored in the project.
    OFX::StringParam* _presetFolder = nullptr;

    OFX::BooleanParam* _blurEnabled  = nullptr;
    OFX::DoubleParam*  _shutterAngle = nullptr;
    OFX::DoubleParam*  _shutterPhase = nullptr;
    OFX::IntParam*     _blurSamples  = nullptr;
    OFX::BooleanParam* _blurAdaptive = nullptr;

    struct StageParamHandles
    {
        // Section groups. Hidden alongside their contents, so an inactive stage
        // leaves no empty headers behind.
        OFX::GroupParam*    grpTiming;
        OFX::GroupParam*    grpFrom;
        OFX::GroupParam*    grpTo;
        OFX::GroupParam*    grpEasing;
        OFX::GroupParam*    grpPath;
        OFX::StringParam*     curveName;
        OFX::PushButtonParam* saveCurve;
        OFX::Double2DParam* pathC1;
        OFX::Double2DParam* pathC2;
        OFX::PushButtonParam* pathReset;
        OFX::BooleanParam*  enabled;
        OFX::ChoiceParam*   timingAnchor;
        OFX::DoubleParam*   startFrame;
        OFX::DoubleParam*   endFrame;
        OFX::PushButtonParam* setStart;
        OFX::PushButtonParam* setEnd;
        OFX::DoubleParam*   duration;
        OFX::PushButtonParam* copyFromTo;
        OFX::PushButtonParam* copyToFrom;
        OFX::PushButtonParam* swapEnds;
        OFX::BooleanParam*  linkScale;
        OFX::DoubleParam*   scaleFrom;
        OFX::DoubleParam*   scaleTo;
        OFX::DoubleParam*   scaleYFrom;
        OFX::DoubleParam*   scaleYTo;
        OFX::DoubleParam*   tiltXFrom;
        OFX::DoubleParam*   tiltXTo;
        OFX::DoubleParam*   swivelYFrom;
        OFX::DoubleParam*   swivelYTo;
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

    /// Suppresses changedParam while a preset is being written in bulk.
    ///
    /// Applying a preset sets around a hundred parameters, and several of them
    /// have handlers that fight each other: writing easingPreset calls
    /// applyEasingPreset(), which overwrites the preset's own easing amounts
    /// with the named preset's canned values, while writing any amount calls
    /// markEasingCustom(), which flips the dropdown back to Custom. Left
    /// unguarded, every stage's easing comes out of a load corrupted.
    bool _applyingPreset = false;

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

    _presetFolder = fetchStringParam(kParamPresetFolder);

    _baseScale     = fetchDoubleParam  (kParamBaseScale);
    _baseScaleY    = fetchDoubleParam  (kParamBaseScaleY);
    _baseLinkScale = fetchBooleanParam (kParamBaseLinkScale);
    _basePos       = fetchDouble2DParam(kParamBasePos);
    _baseRot       = fetchDoubleParam  (kParamBaseRot);
    _baseTiltX     = fetchDoubleParam  (kParamBaseTiltX);
    _baseSwivelY   = fetchDoubleParam  (kParamBaseSwivelY);
    _baseOpacity   = fetchDoubleParam  (kParamBaseOpacity);
    _baseAnchor    = fetchDouble2DParam(kParamBaseAnchor);

    _blurEnabled  = fetchBooleanParam(kParamBlurEnabled);
    _shutterAngle = fetchDoubleParam (kParamShutterAngle);
    _shutterPhase = fetchDoubleParam (kParamShutterPhase);
    _blurSamples  = fetchIntParam    (kParamBlurSamples);
    _blurAdaptive = fetchBooleanParam(kParamBlurAdaptive);

    for (int i = 0; i < kMaxStages; ++i)
    {
        StageParamHandles& s = _stage[i];
        s.grpTiming     = fetchGroupParam   (StageParam(kParamGroupTiming,   i));
        s.grpFrom       = fetchGroupParam   (StageParam(kParamGroupFrom,     i));
        s.grpTo         = fetchGroupParam   (StageParam(kParamGroupTo,       i));
        s.grpEasing     = fetchGroupParam   (StageParam(kParamGroupEasing,   i));
        s.grpPath       = fetchGroupParam   (StageParam(kParamGroupPath,     i));
        s.curveName     = fetchStringParam  (StageParam(kParamCurveName,     i));
        s.saveCurve     = fetchPushButtonParam(StageParam(kParamSaveCurve,  i));
        s.pathC1        = fetchDouble2DParam(StageParam(kParamPathC1,        i));
        s.pathC2        = fetchDouble2DParam(StageParam(kParamPathC2,        i));
        s.pathReset     = fetchPushButtonParam(StageParam(kParamPathReset,   i));
        s.enabled       = fetchBooleanParam (StageParam(kParamEnabled,       i));
        s.timingAnchor  = fetchChoiceParam  (StageParam(kParamAnchor2,       i));
        s.timingAnchor  = fetchChoiceParam  (StageParam(kParamAnchor2,       i));
        s.startFrame    = fetchDoubleParam  (StageParam(kParamStartFrame,    i));
        s.endFrame      = fetchDoubleParam  (StageParam(kParamEndFrame,      i));
        s.setStart      = fetchPushButtonParam(StageParam(kParamSetStart,    i));
        s.setEnd        = fetchPushButtonParam(StageParam(kParamSetEnd,      i));
        s.duration      = fetchDoubleParam  (StageParam(kParamDuration,      i));
        s.copyFromTo    = fetchPushButtonParam(StageParam(kParamCopyFromTo,  i));
        s.copyToFrom    = fetchPushButtonParam(StageParam(kParamCopyToFrom,  i));
        s.swapEnds      = fetchPushButtonParam(StageParam(kParamSwapEnds,    i));
        s.linkScale     = fetchBooleanParam (StageParam(kParamLinkScale,     i));
        s.scaleYFrom    = fetchDoubleParam  (StageParam(kParamScaleYFrom,    i));
        s.scaleYTo      = fetchDoubleParam  (StageParam(kParamScaleYTo,      i));
        s.tiltXFrom     = fetchDoubleParam  (StageParam(kParamTiltXFrom,     i));
        s.tiltXTo       = fetchDoubleParam  (StageParam(kParamTiltXTo,       i));
        s.swivelYFrom   = fetchDoubleParam  (StageParam(kParamSwivelYFrom,   i));
        s.swivelYTo     = fetchDoubleParam  (StageParam(kParamSwivelYTo,     i));
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

    // Deliberately not migrating here. At construction the clip is not
    // necessarily connected, so the clip start reads back as a placeholder, and
    // the OFX spec restricts parameter edit blocks to instance-changed and
    // interact actions in any case. changedClip is the correct hook: it fires
    // once the clip is attached, which is exactly when its extent is knowable.

    syncStageVisibility();
    for (int i = 0; i < kMaxStages; ++i)
    {
        // Duration is derived from start/end, never authored, so it is shown
        // greyed out. Refresh it here so it is right when a saved project is
        // reopened, not only after the next edit.
        _stage[i].duration->setEnabled(false);
        updateDuration(i);
    }

    // The folder read-out is not persisted with the project, so it starts blank
    // on every open and has to be filled in from preferences here.
    syncPresetFolderLabel();
}

////////////////////////////////////////////////////////////////////////////////
// Parameter plumbing

AnimParams MultiTransformPlugin::fetchAnimParams(double p_Time) const
{
    AnimParams a = AnimParams::Default();

    // Legacy timeline-absolute values are converted here, on read, rather than
    // depending on a one-time rewrite having succeeded. The rewrite needs the
    // clip's start, which is not dependable at every point in an effect's life,
    // so making the render path self-correcting removes the timing question
    // from the correctness path entirely.
    double clipStart = 0.0, clipLength = 0.0;
    if (!mtx::GetClipRange(const_cast<MultiTransformPlugin*>(this), clipStart, clipLength))
    {
        clipStart  = 0.0;
        clipLength = 0.0;
    }
    a.clipStart  = static_cast<float>(clipStart);
    a.clipLength = static_cast<float>(clipLength);

    {
        BasePose& b = a.base;
        b.scaleX    = static_cast<float>(_baseScale->getValueAtTime(p_Time));
        b.scaleY    = static_cast<float>(_baseScaleY->getValueAtTime(p_Time));
        b.linkScale = _baseLinkScale->getValueAtTime(p_Time);
        b.rot       = static_cast<float>(_baseRot->getValueAtTime(p_Time));
        b.tiltX     = static_cast<float>(_baseTiltX->getValueAtTime(p_Time));
        b.swivelY   = static_cast<float>(_baseSwivelY->getValueAtTime(p_Time));
        b.opacity   = static_cast<float>(_baseOpacity->getValueAtTime(p_Time) * 0.01);

        double bx = 0.0, by = 0.0;
        _basePos->getValueAtTime(p_Time, bx, by);
        b.posX = static_cast<float>(bx); b.posY = static_cast<float>(by);
        _baseAnchor->getValueAtTime(p_Time, bx, by);
        b.anchorX = static_cast<float>(bx); b.anchorY = static_cast<float>(by);
    }

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

        int anchor = kAnchorClipStart;
        h.timingAnchor->getValueAtTime(p_Time, anchor);
        s.anchor = anchor;

        // The legacy-absolute conversion only applies to values that are meant
        // to be clip-relative. A stage explicitly anchored to the timeline
        // stores absolute frames on purpose, so converting them would be wrong.
        const double rawStart = h.startFrame->getValueAtTime(p_Time);
        const double rawEnd   = h.endFrame->getValueAtTime(p_Time);
        const bool   relative = (anchor != kAnchorTimeline);

        s.startFrame = static_cast<float>(
            relative ? mtx::NormaliseStageFrame(rawStart, clipStart) : rawStart);
        s.endFrame   = static_cast<float>(
            relative ? mtx::NormaliseStageFrame(rawEnd, clipStart) : rawEnd);
        s.scaleFrom     = static_cast<float>(h.scaleFrom->getValueAtTime(p_Time));
        s.scaleTo       = static_cast<float>(h.scaleTo->getValueAtTime(p_Time));
        s.rotFrom       = static_cast<float>(h.rotFrom->getValueAtTime(p_Time));
        s.rotTo         = static_cast<float>(h.rotTo->getValueAtTime(p_Time));
        s.linkScale     = h.linkScale->getValueAtTime(p_Time);
        s.scaleYFrom    = static_cast<float>(h.scaleYFrom->getValueAtTime(p_Time));
        s.scaleYTo      = static_cast<float>(h.scaleYTo->getValueAtTime(p_Time));
        s.tiltXFrom     = static_cast<float>(h.tiltXFrom->getValueAtTime(p_Time));
        s.tiltXTo       = static_cast<float>(h.tiltXTo->getValueAtTime(p_Time));
        s.swivelYFrom   = static_cast<float>(h.swivelYFrom->getValueAtTime(p_Time));
        s.swivelYTo     = static_cast<float>(h.swivelYTo->getValueAtTime(p_Time));

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

void MultiTransformPlugin::getClipPreferences(OFX::ClipPreferencesSetter& p_ClipPreferences)
{
    // Declare that the output changes over time even though no parameter is
    // animated.
    //
    // The animation here is internal: it is driven by the render time, not by
    // host keyframes. Without this, a host is entirely within its rights to
    // render one frame and reuse it for the whole clip, because as far as it
    // can tell nothing about the effect varies -- and Fusion does exactly that,
    // so playback showed a frozen image while the controls still updated it
    // live. Resolve happened to re-render regardless, which is why this went
    // unnoticed there.
    p_ClipPreferences.setOutputFrameVarying(true);
}

////////////////////////////////////////////////////////////////////////////////
// Presets

mtx::PresetStage MultiTransformPlugin::captureStage(int i) const
{
    const StageParamHandles& h = _stage[i];
    mtx::PresetStage s = mtx::PresetStage::Default();

    s.enabled = h.enabled->getValue();
    h.timingAnchor->getValue(s.anchor);
    s.startFrame = static_cast<float>(h.startFrame->getValue());
    s.endFrame   = static_cast<float>(h.endFrame->getValue());

    // A timeline-anchored stage stores absolute frames, which mean nothing on
    // any other clip. Converting it to its clip-relative equivalent keeps the
    // timing shape and discards only the "pinned to this timeline position"
    // intent, which could not have survived the move anyway.
    if (s.anchor == kAnchorTimeline)
    {
        double clipStart = 0.0, clipLength = 0.0;
        if (mtx::GetClipRange(const_cast<MultiTransformPlugin*>(this), clipStart, clipLength))
        {
            s.startFrame -= static_cast<float>(clipStart);
            s.endFrame   -= static_cast<float>(clipStart);
            s.anchor      = kAnchorClipStart;
        }
    }

    s.scaleFrom = static_cast<float>(h.scaleFrom->getValue());
    s.scaleTo   = static_cast<float>(h.scaleTo->getValue());
    s.rotFrom   = static_cast<float>(h.rotFrom->getValue());
    s.rotTo     = static_cast<float>(h.rotTo->getValue());
    s.linkScale  = h.linkScale->getValue();
    s.scaleYFrom = static_cast<float>(h.scaleYFrom->getValue());
    s.scaleYTo   = static_cast<float>(h.scaleYTo->getValue());
    s.tiltXFrom  = static_cast<float>(h.tiltXFrom->getValue());
    s.tiltXTo    = static_cast<float>(h.tiltXTo->getValue());
    s.swivelYFrom = static_cast<float>(h.swivelYFrom->getValue());
    s.swivelYTo   = static_cast<float>(h.swivelYTo->getValue());
    s.opacityFrom = static_cast<float>(h.opacityFrom->getValue());
    s.opacityTo   = static_cast<float>(h.opacityTo->getValue());

    double x = 0.0, y = 0.0;
    h.posFrom->getValue(x, y); s.posXFrom = static_cast<float>(x); s.posYFrom = static_cast<float>(y);
    h.posTo->getValue(x, y);   s.posXTo   = static_cast<float>(x); s.posYTo   = static_cast<float>(y);
    h.anchor->getValue(x, y);  s.anchorX  = static_cast<float>(x); s.anchorY  = static_cast<float>(y);
    h.pathC1->getValue(x, y);  s.pathC1X  = static_cast<float>(x); s.pathC1Y  = static_cast<float>(y);
    h.pathC2->getValue(x, y);  s.pathC2X  = static_cast<float>(x); s.pathC2Y  = static_cast<float>(y);

    h.easingPreset->getValue(s.easingPreset);
    s.easeIn        = static_cast<float>(h.easeIn->getValue());
    s.easeOut       = static_cast<float>(h.easeOut->getValue());
    s.anticipation  = static_cast<float>(h.anticipation->getValue());
    s.overshoot     = static_cast<float>(h.overshoot->getValue());
    h.bounceType->getValue(s.bounceType);
    s.bounceAmount  = static_cast<float>(h.bounceAmount->getValue());
    s.bounceCount   = static_cast<float>(h.bounceCount->getValue());
    s.bounceDamping = static_cast<float>(h.bounceDamping->getValue());
    s.bounceStart   = static_cast<float>(h.bounceStart->getValue());
    return s;
}

mtx::PresetData MultiTransformPlugin::capturePreset(bool wholeEffect) const
{
    mtx::PresetData d = mtx::PresetData::Default();
    d.wholeEffect = wholeEffect;

    double clipStart = 0.0, clipLength = 0.0;
    if (mtx::GetClipRange(const_cast<MultiTransformPlugin*>(this), clipStart, clipLength))
        d.sourceClipLength = static_cast<float>(clipLength);

    if (wholeEffect)
    {
        int v = 0;
        _stageCount->getValue(v); d.stageCount = v + 1;
        _filter->getValue(d.filterMode);
        _edge->getValue(d.edgeMode);

        d.blurEnabled  = _blurEnabled->getValue();
        d.shutterAngle = static_cast<float>(_shutterAngle->getValue());
        d.shutterPhase = static_cast<float>(_shutterPhase->getValue());
        d.blurSamples  = _blurSamples->getValue();
        d.blurAdaptive = _blurAdaptive->getValue();

        mtx::BasePose& b = d.base;
        b.scaleX    = static_cast<float>(_baseScale->getValue());
        b.scaleY    = static_cast<float>(_baseScaleY->getValue());
        b.linkScale = _baseLinkScale->getValue();
        b.rot       = static_cast<float>(_baseRot->getValue());
        b.tiltX     = static_cast<float>(_baseTiltX->getValue());
        b.swivelY   = static_cast<float>(_baseSwivelY->getValue());
        b.opacity   = static_cast<float>(_baseOpacity->getValue() * 0.01);

        double bx = 0.0, by = 0.0;
        _basePos->getValue(bx, by);    b.posX    = static_cast<float>(bx); b.posY    = static_cast<float>(by);
        _baseAnchor->getValue(bx, by); b.anchorX = static_cast<float>(bx); b.anchorY = static_cast<float>(by);

        for (int i = 0; i < kMaxStages; ++i) d.stages[i] = captureStage(i);
    }
    else
    {
        int active = 0;
        _activeStage->getValue(active);
        if (active < 0) active = 0;
        if (active >= kMaxStages) active = kMaxStages - 1;
        d.stages[0] = captureStage(active);
    }
    return d;
}

void MultiTransformPlugin::applyStage(int i, const mtx::PresetStage& s)
{
    StageParamHandles& h = _stage[i];

    h.enabled->setValue(s.enabled);
    h.timingAnchor->setValue(s.anchor);
    h.startFrame->setValue(s.startFrame);
    h.endFrame->setValue(s.endFrame);

    h.scaleFrom->setValue(s.scaleFrom);
    h.scaleTo->setValue(s.scaleTo);
    h.rotFrom->setValue(s.rotFrom);
    h.rotTo->setValue(s.rotTo);
    h.linkScale->setValue(s.linkScale);
    h.scaleYFrom->setValue(s.scaleYFrom);
    h.scaleYTo->setValue(s.scaleYTo);
    h.tiltXFrom->setValue(s.tiltXFrom);
    h.tiltXTo->setValue(s.tiltXTo);
    h.swivelYFrom->setValue(s.swivelYFrom);
    h.swivelYTo->setValue(s.swivelYTo);
    h.opacityFrom->setValue(s.opacityFrom);
    h.opacityTo->setValue(s.opacityTo);

    h.posFrom->setValue(s.posXFrom, s.posYFrom);
    h.posTo->setValue(s.posXTo, s.posYTo);
    h.anchor->setValue(s.anchorX, s.anchorY);
    h.pathC1->setValue(s.pathC1X, s.pathC1Y);
    h.pathC2->setValue(s.pathC2X, s.pathC2Y);

    // The amounts are the source of truth for the curve; the dropdown is only a
    // label for them. Both are restored verbatim, which is safe only because
    // changedParam is suppressed -- otherwise setting the dropdown would stamp
    // its canned values straight over the amounts written next.
    h.easingPreset->setValue(s.easingPreset);
    h.easeIn->setValue(s.easeIn);
    h.easeOut->setValue(s.easeOut);
    h.anticipation->setValue(s.anticipation);
    h.overshoot->setValue(s.overshoot);
    h.bounceType->setValue(s.bounceType);
    h.bounceAmount->setValue(s.bounceAmount);
    h.bounceCount->setValue(s.bounceCount);
    h.bounceDamping->setValue(s.bounceDamping);
    h.bounceStart->setValue(s.bounceStart);
}

void MultiTransformPlugin::syncPresetFolderLabel()
{
    if (!_presetFolder) return;

    // Shown resolved rather than as "(default)": the point of the read-out is to
    // answer "where will the dialog open", and a label that says "default" only
    // moves the question along.
    _presetFolder->setValue(mtx::PresetFolder());
}

void MultiTransformPlugin::saveCurve(int stage)
{
    StageParamHandles& h = _stage[stage];

    std::string raw;
    h.curveName->getValue(raw);

    const std::string name = mtx::SanitiseFileName(raw);
    if (name.empty())
    {
        sendMessage(OFX::Message::eMessageError, "",
                    "Type a name for the curve first.\n\n"
                    "It becomes the file name, so it cannot contain \\ / : * ? \" < > |");
        return;
    }

    mtx::CurvePreset c = mtx::CurvePreset::Default();
    c.name          = name;
    c.easeIn        = static_cast<float>(h.easeIn->getValue());
    c.easeOut       = static_cast<float>(h.easeOut->getValue());
    c.anticipation  = static_cast<float>(h.anticipation->getValue());
    c.overshoot     = static_cast<float>(h.overshoot->getValue());
    h.bounceType->getValue(c.bounceType);
    c.bounceAmount  = static_cast<float>(h.bounceAmount->getValue());
    c.bounceCount   = static_cast<float>(h.bounceCount->getValue());
    c.bounceDamping = static_cast<float>(h.bounceDamping->getValue());
    c.bounceStart   = static_cast<float>(h.bounceStart->getValue());

    const std::string folder = mtx::CurveFolder();
    if (folder.empty())
    {
        sendMessage(OFX::Message::eMessageError, "", "Could not locate the curve folder.");
        return;
    }

    std::string error;
    if (!mtx::WriteTextFile(folder + "\\" + name + ".json", mtx::CurveToJson(c), error))
    {
        sendMessage(OFX::Message::eMessageError, "", "Could not save the curve:\n" + error);
        return;
    }
    mtx::ProbeLog("curve saved: " + name);
}

void MultiTransformPlugin::transferEnds(int stage, int mode)
{
    StageParamHandles& h = _stage[stage];

    // Every channel that has two ends, listed once. Adding a new animated
    // property means adding it here too, and forgetting shows up immediately as
    // a swap that leaves one channel behind.
    OFX::DoubleParam* pairs[][2] = {
        { h.scaleFrom,   h.scaleTo   },
        { h.scaleYFrom,  h.scaleYTo  },
        { h.rotFrom,     h.rotTo     },
        { h.tiltXFrom,   h.tiltXTo   },
        { h.swivelYFrom, h.swivelYTo },
        { h.opacityFrom, h.opacityTo },
    };

    const char* label = (mode == 0) ? "Copy FROM to TO"
                      : (mode == 1) ? "Copy TO to FROM"
                                    : "Swap FROM and TO";
    mtx::EditBlock block(this, label);

    for (auto& p : pairs)
    {
        const double from = p[0]->getValue();
        const double to   = p[1]->getValue();

        if      (mode == 0) p[1]->setValue(from);
        else if (mode == 1) p[0]->setValue(to);
        else                { p[0]->setValue(to); p[1]->setValue(from); }
    }

    // Position is a 2D parameter and so cannot join the list above.
    {
        double fx = 0.0, fy = 0.0, tx = 0.0, ty = 0.0;
        h.posFrom->getValue(fx, fy);
        h.posTo->getValue(tx, ty);

        if      (mode == 0) h.posTo->setValue(fx, fy);
        else if (mode == 1) h.posFrom->setValue(tx, ty);
        else                { h.posFrom->setValue(tx, ty); h.posTo->setValue(fx, fy); }
    }

    // Swapping the ends reverses the route, so the two path handles have to
    // trade places or a bent path would turn inside out. The first handle is an
    // offset from one third along the straight line and the second from two
    // thirds; reversing the line maps each onto the other exactly, so trading
    // them preserves the drawn shape rather than approximating it.
    //
    // A copy is left alone deliberately: it is a change of pose, not of route.
    if (mode == 2)
    {
        double c1x = 0.0, c1y = 0.0, c2x = 0.0, c2y = 0.0;
        h.pathC1->getValue(c1x, c1y);
        h.pathC2->getValue(c2x, c2y);
        h.pathC1->setValue(c2x, c2y);
        h.pathC2->setValue(c1x, c1y);
    }
}

void MultiTransformPlugin::applyPreset(const mtx::PresetData& d)
{
    // One edit block for the whole batch: it is what tells the host an edit
    // happened at all, and it collapses ~100 writes into a single undo step and
    // a single cache invalidation.
    mtx::EditBlock block(this, "Load Multi Transform Preset");

    _applyingPreset = true;

    if (d.wholeEffect)
    {
        _stageCount->setValue(d.stageCount - 1);
        _filter->setValue(d.filterMode);
        _edge->setValue(d.edgeMode);

        _blurEnabled->setValue(d.blurEnabled);
        _shutterAngle->setValue(d.shutterAngle);
        _shutterPhase->setValue(d.shutterPhase);
        _blurSamples->setValue(d.blurSamples);
        _blurAdaptive->setValue(d.blurAdaptive);

        _baseScale->setValue(d.base.scaleX);
        _baseScaleY->setValue(d.base.scaleY);
        _baseLinkScale->setValue(d.base.linkScale);
        _basePos->setValue(d.base.posX, d.base.posY);
        _baseRot->setValue(d.base.rot);
        _baseTiltX->setValue(d.base.tiltX);
        _baseSwivelY->setValue(d.base.swivelY);
        _baseOpacity->setValue(d.base.opacity * 100.0);
        _baseAnchor->setValue(d.base.anchorX, d.base.anchorY);

        for (int i = 0; i < kMaxStages; ++i) applyStage(i, d.stages[i]);
    }
    else
    {
        int active = 0;
        _activeStage->getValue(active);
        if (active < 0) active = 0;
        if (active >= kMaxStages) active = kMaxStages - 1;
        applyStage(active, d.stages[0]);
    }

    _applyingPreset = false;

    // Derived state, refreshed once rather than on every one of the writes above.
    for (int i = 0; i < kMaxStages; ++i) updateDuration(i);
    syncStageVisibility();
}

void MultiTransformPlugin::savePreset(bool wholeEffect)
{
    std::string path;
    if (!mtx::ChoosePresetToSave(wholeEffect ? "MultiTransform" : "Stage", path)) return;

    mtx::PresetData d = capturePreset(wholeEffect);
    d.name = mtx::StemOf(path);

    std::string error;
    if (!mtx::WriteTextFile(path, mtx::ToJson(d), error))
    {
        sendMessage(OFX::Message::eMessageError, "", "Could not save the preset:\n" + error);
        return;
    }
    mtx::ProbeLog("preset saved: " + path);
}

void MultiTransformPlugin::loadPreset(bool fitToClip)
{
    std::string path;
    if (!mtx::ChoosePresetToOpen(path)) return;

    std::string text, error;
    if (!mtx::ReadTextFile(path, text, error))
    {
        sendMessage(OFX::Message::eMessageError, "", "Could not read the preset:\n" + error);
        return;
    }

    mtx::PresetData d;
    if (!mtx::FromJson(text, d, error))
    {
        // Refusing outright beats applying half a preset: a partly-loaded setup
        // is a state that never existed and is hard to spot or undo.
        sendMessage(OFX::Message::eMessageError, "",
                    "This is not a valid Multi Transform preset:\n" + error);
        return;
    }

    if (fitToClip)
    {
        double clipStart = 0.0, clipLength = 0.0;
        if (mtx::GetClipRange(this, clipStart, clipLength))
        {
            mtx::RescaleTiming(d, static_cast<float>(clipLength));
        }
        else
        {
            // Scaling by a guessed ratio would quietly mis-time everything, so
            // say so rather than pretending it worked.
            sendMessage(OFX::Message::eMessageWarning, "",
                        "The clip length is unknown, so the preset was applied "
                        "without rescaling its timing.");
        }
    }

    applyPreset(d);
    mtx::ProbeLog("preset loaded: " + path + (fitToClip ? " (fit to clip)" : ""));
}

void MultiTransformPlugin::changedClip(const OFX::InstanceChangedArgs& /*p_Args*/,
                                       const std::string& /*p_ClipName*/)
{
    // The clip has just been attached or replaced, so its extent is now
    // knowable and a legacy absolute timing can be rewritten. This is also an
    // instance-changed action, which is where the OFX spec permits parameter
    // edit blocks -- the constructor is not.
    migrateTimingIfNeeded();
}

void MultiTransformPlugin::migrateTimingIfNeeded()
{
    // Convert stage timing from timeline-absolute frames to clip-relative ones,
    // once per effect.
    //
    // Subtracting the clip's *current* start is what makes this lossless: an
    // existing animation carries on playing at exactly the frames it plays at
    // today, and only afterwards gains the ability to travel with the clip. The
    // conversion is guarded by a version parameter that defaults to 0, so a
    // project saved before that parameter existed -- where the host hands back
    // the default -- is correctly recognised as needing it.
    double clipStart = 0.0, clipLength = 0.0;
    if (!mtx::GetClipRange(this, clipStart, clipLength)) return;

    // Only act on a believable clip start.
    //
    // Asked too early -- before the clip is connected -- the host returns a
    // placeholder such as [0, 1999]. That passes the range validation, and a
    // previous version accepted it, converted nothing against a start of zero,
    // and then recorded the migration as complete. The effect locked itself out
    // of ever converting, which is strictly worse than not having tried. Below
    // this threshold there is nothing to convert anyway, since absolute and
    // relative frames coincide when a timeline starts near zero.
    if (clipStart <= 1000.0) return;

    // Nothing to do unless a value actually looks legacy. Checked before
    // opening an edit block so that the common case is completely silent.
    bool any = false;
    for (int i = 0; i < kMaxStages && !any; ++i)
    {
        any = mtx::LooksTimelineAbsolute(_stage[i].startFrame->getValue(), clipStart)
           || mtx::LooksTimelineAbsolute(_stage[i].endFrame->getValue(),   clipStart);
    }
    if (!any) return;

    int converted = 0;
    {
        mtx::EditBlock block(this, "Convert timing to clip-relative");
        for (int i = 0; i < kMaxStages; ++i)
        {
            const double s = _stage[i].startFrame->getValue();
            const double e = _stage[i].endFrame->getValue();

            if (mtx::LooksTimelineAbsolute(s, clipStart))
            {
                _stage[i].startFrame->setValue(s - clipStart);
                ++converted;
            }
            if (mtx::LooksTimelineAbsolute(e, clipStart))
            {
                _stage[i].endFrame->setValue(e - clipStart);
                ++converted;
            }
        }
    }

    for (int i = 0; i < kMaxStages; ++i) updateDuration(i);

    std::ostringstream o;
    o << "timing migration: clipStart=" << clipStart << " length=" << clipLength
      << " -- converted " << converted << " value(s) to clip-relative";
    mtx::ProbeLog(o.str());
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

    // The base pose has the same link rule as a stage, and this runs from every
    // place that could have changed it.
    bool baseLinked = true;
    _baseLinkScale->getValue(baseLinked);
    _baseScaleY->setIsSecret(baseLinked);

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

        // The group headers as well as their contents. Hiding only the contents
        // would leave four empty section headers per inactive stage, which is
        // worse than the flat list this replaced.
        s.grpTiming->setIsSecret(hidden);
        s.grpFrom->setIsSecret(hidden);
        s.grpTo->setIsSecret(hidden);
        s.grpEasing->setIsSecret(hidden);
        s.grpPath->setIsSecret(hidden);
        s.curveName->setIsSecret(hidden);
        s.saveCurve->setIsSecret(hidden);
        s.pathC1->setIsSecret(hidden);
        s.pathC2->setIsSecret(hidden);
        s.pathReset->setIsSecret(hidden);

        s.enabled->setIsSecret(hidden);
        s.timingAnchor->setIsSecret(hidden);
        s.timingAnchor->setIsSecret(hidden);
        s.setStart->setIsSecret(hidden);
        s.setEnd->setIsSecret(hidden);
        s.startFrame->setIsSecret(hidden);
        s.endFrame->setIsSecret(hidden);
        s.duration->setIsSecret(hidden);
        s.anchor->setIsSecret(hidden);

        s.copyFromTo->setIsSecret(hidden);
        s.copyToFrom->setIsSecret(hidden);
        s.swapEnds->setIsSecret(hidden);
        s.linkScale->setIsSecret(hidden);
        s.scaleFrom->setIsSecret(hidden);
        s.posFrom->setIsSecret(hidden);
        s.rotFrom->setIsSecret(hidden);
        s.tiltXFrom->setIsSecret(hidden);
        s.swivelYFrom->setIsSecret(hidden);
        s.opacityFrom->setIsSecret(hidden);

        s.scaleTo->setIsSecret(hidden);
        s.posTo->setIsSecret(hidden);
        s.rotTo->setIsSecret(hidden);
        s.tiltXTo->setIsSecret(hidden);
        s.swivelYTo->setIsSecret(hidden);
        s.opacityTo->setIsSecret(hidden);

        // Same combined rule as the bounce amounts: visible only when the stage
        // is active AND the axes are unlinked.
        bool linked = true;
        s.linkScale->getValue(linked);
        s.scaleYFrom->setIsSecret(hidden || linked);
        s.scaleYTo  ->setIsSecret(hidden || linked);

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

        // Under Stretch these fields hold percentages, not frames. Relabelling
        // costs nothing and heads off the obvious misreading -- a "Start Frame"
        // of 50 meaning halfway through the clip rather than frame 50.
        int anchor = kAnchorClipStart;
        s.timingAnchor->getValue(anchor);
        const bool pct = (anchor == kAnchorStretch);

        s.startFrame->setLabel(pct ? "Start (% of clip)"  : "Start Frame");
        s.endFrame->setLabel  (pct ? "End (% of clip)"    : "End Frame");
        s.duration->setLabel  (pct ? "Duration (% of clip)" : "Duration (frames)");
    }
}

void MultiTransformPlugin::applyEasingPreset(int stageIndex)
{
    int preset = 0;
    _stage[stageIndex].easingPreset->getValue(preset);
    if (preset == kEasingCustom) return;   // Custom means "leave my values alone"

    const EasingPresetValues v = PresetValues(preset);

    mtx::EditBlock block(this, "Easing Preset");
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

    mtx::EditBlock block(this, "Custom Easing");
    _syncingEasing = true;
    _stage[stageIndex].easingPreset->setValue(kEasingCustom);
    _syncingEasing = false;
}

double MultiTransformPlugin::playheadInStageFrames(int stageIndex, double absoluteTime) const
{
    // Capture the playhead in whatever units this stage's frames are expressed
    // in, so the button reads back the same number the Inspector shows.
    int anchor = kAnchorClipStart;
    _stage[stageIndex].timingAnchor->getValue(anchor);

    if (anchor == kAnchorTimeline) return absoluteTime;

    double clipStart = 0.0, clipLength = 0.0;
    if (!mtx::GetClipRange(const_cast<MultiTransformPlugin*>(this), clipStart, clipLength))
        return absoluteTime;

    // Reuse the engine's own mapping rather than repeating it, so the button
    // can never disagree with what actually renders.
    AnimParams a = AnimParams::Default();
    a.clipStart  = static_cast<float>(clipStart);
    a.clipLength = static_cast<float>(clipLength);
    a.stages[0].anchor = anchor;

    return StageLocalTime(a, a.stages[0], static_cast<float>(absoluteTime - clipStart));
}

void MultiTransformPlugin::updateDuration(int stageIndex)
{
    const double start = _stage[stageIndex].startFrame->getValue();
    const double end   = _stage[stageIndex].endFrame->getValue();

    mtx::EditBlock block(this, "Duration");
    _stage[stageIndex].duration->setValue(end - start);
}

void MultiTransformPlugin::changedParam(const OFX::InstanceChangedArgs& p_Args,
                                        const std::string& p_ParamName)
{
    // A bulk preset write must not trigger the per-parameter handlers below --
    // several of them would rewrite the very values being restored.
    if (_applyingPreset) return;

    if (p_ParamName == kParamSetFolder)
    {
        std::string picked;
        if (!mtx::ChooseFolder(mtx::PresetFolder(), picked)) return;   // cancelled

        mtx::Settings s = mtx::LoadSettings();
        s.presetFolder  = picked;

        std::string error;
        if (!mtx::SaveSettings(s, error))
        {
            sendMessage(OFX::Message::eMessageError, "",
                        "Could not save the preset folder preference:\n" + error);
            return;
        }
        syncPresetFolderLabel();
        return;
    }

    if (p_ParamName == kParamResetFolder)
    {
        mtx::Settings s = mtx::LoadSettings();
        s.presetFolder.clear();   // empty means "the built-in default", not a stored path

        std::string error;
        if (!mtx::SaveSettings(s, error))
        {
            sendMessage(OFX::Message::eMessageError, "",
                        "Could not save the preset folder preference:\n" + error);
            return;
        }
        syncPresetFolderLabel();
        return;
    }

    if (p_ParamName == kParamSaveEffect)    { savePreset(true);  return; }
    if (p_ParamName == kParamSaveStage)     { savePreset(false); return; }
    if (p_ParamName == kParamLoadPreset)    { loadPreset(false); return; }
    if (p_ParamName == kParamLoadPresetFit) { loadPreset(true);  return; }
    if (p_ParamName == kParamLoadFromOverlay) { loadPreset(false); return; }
    if (p_ParamName == kParamLoadFromOverlay) { loadPreset(false); return; }

    if (p_ParamName == kParamBaseReset)
    {
        mtx::EditBlock block(this, "Reset Base Transform");
        _baseScale->setValue(1.0);
        _baseScaleY->setValue(1.0);
        _baseLinkScale->setValue(true);
        _basePos->setValue(0.0, 0.0);
        _baseRot->setValue(0.0);
        _baseTiltX->setValue(0.0);
        _baseSwivelY->setValue(0.0);
        _baseOpacity->setValue(100.0);
        _baseAnchor->setValue(0.5, 0.5);
        syncStageVisibility();   // Scale Y goes back into hiding with the link
        return;
    }

    if (p_ParamName == kParamBaseLinkScale)
    {
        // Unlinking starts matched rather than snapping to whatever Scale Y last
        // held, which would look like the image jumped for no reason.
        bool linked = true;
        _baseLinkScale->getValue(linked);
        if (!linked)
        {
            mtx::EditBlock block(this, "Unlink Base Scale");
            _baseScaleY->setValue(_baseScale->getValue());
        }
        syncStageVisibility();
        return;
    }

    // Retry point: if the constructor ran before the clip was connected, the
    // clip extent was unknown and the conversion was deferred. It is a no-op
    // once done.
    migrateTimingIfNeeded();

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
        if (p_ParamName == StageParam(kParamAnchor2, i))
        {
            // Switching to or from Stretch changes what the frame fields mean,
            // so their labels have to follow.
            syncStageVisibility();
            return;
        }

        // Capturing the playhead is the only way to learn a timeline position:
        // Resolve hands the plugin timeline-absolute times and no usable clip
        // range, so park-and-click beats typing frame numbers.
        // Stored clip-relative, so the captured value keeps meaning the same
        // thing after the clip is moved or trimmed.
        if (p_ParamName == StageParam(kParamSetStart, i))
        {
            {
                mtx::EditBlock block(this, "Set Start to Playhead");
                _stage[i].startFrame->setValue(playheadInStageFrames(i, p_Args.time));
            }
            updateDuration(i);
            return;
        }
        if (p_ParamName == StageParam(kParamSetEnd, i))
        {
            {
                mtx::EditBlock block(this, "Set End to Playhead");
                _stage[i].endFrame->setValue(playheadInStageFrames(i, p_Args.time));
            }
            updateDuration(i);
            return;
        }
        if (p_ParamName == StageParam(kParamStartFrame, i) ||
            p_ParamName == StageParam(kParamEndFrame, i))
        {
            updateDuration(i);
            return;
        }
        if (p_ParamName == StageParam(kParamCopyFromTo, i)) { transferEnds(i, 0); return; }
        if (p_ParamName == StageParam(kParamCopyToFrom, i)) { transferEnds(i, 1); return; }
        if (p_ParamName == StageParam(kParamSwapEnds,   i)) { transferEnds(i, 2); return; }
        if (p_ParamName == StageParam(kParamLinkScale, i))
        {
            bool linked = true;
            _stage[i].linkScale->getValue(linked);
            if (!linked)
            {
                // Copy X into Y at both ends, so unlinking is visually a no-op
                // and the split starts from the animation already on screen.
                mtx::EditBlock block(this, "Unlink Scale");
                _stage[i].scaleYFrom->setValue(_stage[i].scaleFrom->getValue());
                _stage[i].scaleYTo  ->setValue(_stage[i].scaleTo->getValue());
            }
            syncStageVisibility();
            return;
        }
        if (p_ParamName == StageParam(kParamSaveCurve, i)) { saveCurve(i); return; }
        if (p_ParamName == StageParam(kParamEasingPreset, i))
        {
            applyEasingPreset(i);
            return;
        }
        if (p_ParamName == StageParam(kParamPathReset, i))
        {
            // Zero offsets restore an exactly straight line, not merely a
            // nearly straight one -- see PathControlPoints.
            mtx::EditBlock block(this, "Straighten Path");
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
    if (!_srcClip) return false;

    // Answer for the whole clip, not for this frame.
    //
    // Testing the transform at p_Args.time looks obviously right and is not:
    // outside a stage's range the progress pins to 0 or 1, so the transform
    // collapses to the From or To pose, which is normally the identity. The
    // effect then declares itself a pass-through on exactly the frames outside
    // its own animation. Hosts cache that per frame, so moving the stage's
    // start or end to cover one of those frames left the host still convinced
    // nothing happens there -- edits applied while the playhead sat outside the
    // range appeared to do nothing at all, and only a manual cache purge fixed
    // it. IsNoOp gives the same answer at every time, so there is no stale
    // verdict to get stuck on.
    if (!IsNoOp(fetchAnimParams(p_Args.time))) return false;

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

        // Evaluate in clip time, not timeline time. Stage start/end are frames
        // measured from the clip's first frame, so the animation stays put when
        // the clip is moved or trimmed rather than being anchored to absolute
        // timeline positions that stop meaning anything the moment the edit
        // changes.
        const double clipTime = mtx::ToClipTime(this, p_Args.time);

        st = BuildSampleTransforms(fetchAnimParams(p_Args.time),
                                   fetchBlurParams(p_Args.time),
                                   static_cast<float>(clipTime),
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

/** @brief Declare that changing this parameter invalidates the whole cache.
 *
 * The OFX default, kOfxParamInvalidateValueChange, means "invalidate only the
 * range of frames this parameter's keyframe affects". That is the right default
 * for an effect whose parameters are keyframed -- and completely wrong here.
 *
 * Nothing in this plugin is ever keyframed: the parameters are constants that
 * the animation engine interprets against the render time, so *every* parameter
 * affects *every* frame. Without this declaration a host that caches rendered
 * frames has no reason to throw any of them away when a value changes, and
 * keeps replaying stale output until the cache is purged by hand. Resolve is
 * lenient about it; Fusion is not, and it is Fusion that is behaving correctly.
 *
 * Deliberately not applied to parameters that only drive the viewer overlay
 * (Active Stage, Gizmo Edits, Show Curve Editor) -- those change nothing about
 * the rendered image, and purging the cache when a stage tab is clicked would
 * be a needless stall.
 */
template <class T>
T* InvalidatesCache(T* p)
{
    p->setCacheInvalidation(eCacheInvalidateValueAll);
    return p;
}

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
    return InvalidatesCache(p);
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
    return InvalidatesCache(p);
}

/** @brief A section heading: a static label used purely as a visual divider.
 *
 * Deliberately not a group parameter. Groups add a collapsible section with a
 * disclosure arrow, and four stages' worth of them turned the Inspector into a
 * wall of things to click open. A label just separates the controls beneath it.
 */
GroupParamDescriptor* DefineSection(OFX::ImageEffectDescriptor& desc, PageParamDescriptor* page,
                                    const std::string& name, const std::string& label,
                                    const std::string& hint = std::string())
{
    GroupParamDescriptor* g = desc.defineGroupParam(name);
    g->setLabels(label, label, label);
    if (!hint.empty()) g->setHint(hint);

    // Collapsed by default. One active stage puts about seventy rows in the
    // Inspector, and the great majority of them are set once and then left --
    // a base pose, an easing curve, a preset folder. Opening a section to reach
    // those is cheaper than scrolling past them every time.
    g->setOpen(false);

    page->addChild(*g);
    return g;
}

/** @brief Stage buttons that live at the top of the panel, outside every group.
 *
 * These are reached constantly while building a move, so putting them behind a
 * disclosure arrow costs a click every time. They are defined separately from
 * the rest of the stage because *position in the Inspector is decided by
 * definition order*, and ungrouped parameters must all come before the first
 * group -- an ungrouped island between two groups gets scattered by Resolve,
 * which is how the preset buttons ended up split across the panel once before.
 */
void DefineStageRootButtons(OFX::ImageEffectDescriptor& desc, PageParamDescriptor* page, int i)
{
    // Moving poses between the two ends. Building a move usually starts by
    // matching one end to the other and then changing only what should differ,
    // which is otherwise seven fields retyped by hand.
    PushButtonParamDescriptor* copyFwd = desc.definePushButtonParam(StageParam(kParamCopyFromTo, i));
    copyFwd->setLabels("Copy FROM to TO", "From > To", "Copy FROM to TO");
    copyFwd->setHint("Overwrite the TO pose with the FROM pose, leaving the stage holding still "
                     "until you change one of them.");
    page->addChild(*copyFwd);

    PushButtonParamDescriptor* copyBack = desc.definePushButtonParam(StageParam(kParamCopyToFrom, i));
    copyBack->setLabels("Copy TO to FROM", "To > From", "Copy TO to FROM");
    copyBack->setHint("Overwrite the FROM pose with the TO pose. Useful after posing the end "
                      "state on screen: copy it back, then pull the start away from it.");
    page->addChild(*copyBack);

    PushButtonParamDescriptor* swapEnds = desc.definePushButtonParam(StageParam(kParamSwapEnds, i));
    swapEnds->setLabels("Swap FROM and TO", "Swap", "Swap FROM and TO");
    swapEnds->setHint("Reverse the move. The motion path's two handles swap with it, so a bent "
                      "route keeps its exact shape and simply runs the other way.");
    page->addChild(*swapEnds);
}

void DefineStage(OFX::ImageEffectDescriptor& desc, PageParamDescriptor* page, int i)
{
    const std::string idx = std::to_string(i + 1);

    // Only one stage is ever visible at a time: the other three stages have
    // every parameter AND every group hidden, so the Inspector shows one
    // stage's five sections rather than twenty.

    // --- Timing ---
    GroupParamDescriptor* gTiming = DefineSection(desc, page, StageParam(kParamGroupTiming, i),
        "Stage " + idx + " \xE2\x80\x94 Timing",
        "When this stage runs, and what its frame numbers are measured from.");

    BooleanParamDescriptor* en = desc.defineBooleanParam(StageParam(kParamEnabled, i));
    en->setLabels("Enabled", "Enabled", "Enabled");
    en->setHint("Include this stage in the combined transform. Stages COMBINE rather than "
                "replace: two stages each scaling 1.0 to 1.5 give 2.25x overall.");
    en->setDefault(i == 0);
    en->setParent(*gTiming);
    page->addChild(*en);
    InvalidatesCache(en);

    ChoiceParamDescriptor* anchor = desc.defineChoiceParam(StageParam(kParamAnchor2, i));
    anchor->setLabels("Anchor", "Anchor", "Anchor");
    anchor->setHint("What this stage's Start and End are measured from. Clip Start puts 0 on "
                    "the clip's first frame -- use it for intros. Clip End puts 0 on the LAST "
                    "frame and counts backwards, so an outro written as -20 to 0 always "
                    "finishes exactly on the final frame however the clip is trimmed. Stretch "
                    "treats Start and End as PERCENTAGES of the clip, so 0 to 100 always fills "
                    "the whole clip and the move compresses or expands as the clip is trimmed. "
                    "Timeline uses absolute frames and ignores the clip. Stages choose "
                    "independently, so one effect can hold an intro and an outro at once.");
    anchor->appendOption("Clip Start");
    anchor->appendOption("Clip End");
    anchor->appendOption("Timeline (absolute)");
    anchor->appendOption("Stretch (% of clip)");
    anchor->setDefault(kAnchorClipStart);
    anchor->setParent(*gTiming);
    page->addChild(*anchor);
    InvalidatesCache(anchor);

    PushButtonParamDescriptor* setStart = desc.definePushButtonParam(StageParam(kParamSetStart, i));
    setStart->setLabels("Set Start to Playhead", "Set Start", "Set Start to Playhead");
    setStart->setHint("Park the playhead where this stage should begin and click.");
    setStart->setParent(*gTiming);
    page->addChild(*setStart);

    PushButtonParamDescriptor* setEnd = desc.definePushButtonParam(StageParam(kParamSetEnd, i));
    setEnd->setLabels("Set End to Playhead", "Set End", "Set End to Playhead");
    setEnd->setHint("Park the playhead where this stage should finish and click.");
    setEnd->setParent(*gTiming);
    page->addChild(*setEnd);

    DefineDouble(desc, page, gTiming, StageParam(kParamStartFrame, i), "Start Frame",
                 "Frames from the START OF THE CLIP, not from the timeline: 0 is the clip's "
                 "first frame. Move or trim the clip and the animation goes with it. "
                 "Staggering is just giving stages different start frames: 0 here and 6 on "
                 "the next stage is a six-frame stagger.",
                 0.0, -100000.0, 1e6, 0.0, 240.0, 1.0);

    DefineDouble(desc, page, gTiming, StageParam(kParamEndFrame, i), "End Frame",
                 "Frames from the start of the clip. Stages may differ in length as well as "
                 "in start time.",
                 24.0, -100000.0, 1e6, 0.0, 240.0, 1.0);

    // Duration is a derived read-out, so it must not claim to invalidate
    // anything: Start and End already do, and this would only purge the cache a
    // second time for a value that changes no pixels.
    DefineDouble(desc, page, gTiming, StageParam(kParamDuration, i), "Duration (frames)",
                 "End Frame minus Start Frame. Calculated automatically -- shown for "
                 "reference, not editable.",
                 24.0, -1e9, 1e9, 0.0, 240.0, 1.0)
        ->setCacheInvalidation(eCacheInvalidateValueChange);

    DefineDouble2D(desc, page, gTiming, StageParam(kParamAnchor, i), "Anchor",
                   "Point that scale and rotation pivot around. 0.5, 0.5 is the image centre. "
                   "Shared by both ends -- a pivot that moved mid-move would make the motion "
                   "impossible to reason about.",
                   0.5, 0.5);

    // Shared by both ends, like the anchor: scale that were linked at one end and
    // split at the other would be two different kinds of animation in one stage.
    BooleanParamDescriptor* link = desc.defineBooleanParam(StageParam(kParamLinkScale, i));
    link->setLabels("Link Scale X/Y", "Link Scale", "Link Scale X/Y");
    link->setHint("Scale both axes together. Turn this off to squash or stretch one axis -- "
                  "Scale Y then appears alongside Scale X at each end.");
    link->setDefault(true);
    link->setParent(*gTiming);
    page->addChild(*link);
    InvalidatesCache(link);

    // --- From / To ---
    // Split so a pose reads top to bottom in one block, instead of picking every
    // other row out of an interleaved "Scale From / Scale To / Position From..."
    // list.
    struct EndSpec { const char* groupName; const char* heading; bool isTo; };
    const EndSpec ends[2] = {
        { kParamGroupFrom, "From (start)", false },
        { kParamGroupTo,   "To (end)",     true  }
    };

    for (const EndSpec& end : ends)
    {
        GroupParamDescriptor* g = DefineSection(desc, page, StageParam(end.groupName, i),
            "Stage " + idx + " \xE2\x80\x94 " + end.heading,
            end.isTo ? "The pose this stage animates to."
                     : "The pose this stage animates from.");

        DefineDouble(desc, page, g,
                     StageParam(end.isTo ? kParamScaleTo : kParamScaleFrom, i), "Scale",
                     "Scale multiplier. Drives both axes while Link Scale X/Y is on; "
                     "the X axis alone once it is off.",
                     1.0, -100.0, 100.0, 0.0, 4.0, 0.01);

        DefineDouble(desc, page, g,
                     StageParam(end.isTo ? kParamScaleYTo : kParamScaleYFrom, i), "Scale Y",
                     "Vertical scale multiplier. Only used when Link Scale X/Y is off.",
                     1.0, -100.0, 100.0, 0.0, 4.0, 0.01);

        DefineDouble2D(desc, page, g,
                       StageParam(end.isTo ? kParamPosTo : kParamPosFrom, i), "Position",
                       "Offset, normalised: 1.0 is one full image width/height.", 0.0, 0.0);

        DefineDouble(desc, page, g,
                     StageParam(end.isTo ? kParamRotTo : kParamRotFrom, i), "Rotation",
                     "Rotation in degrees.", 0.0, -100000.0, 100000.0, -360.0, 360.0, 1.0);

        // Orthographic, not perspective: an axis rotation reads as a squash,
        // because nothing foreshortens and parallel edges stay parallel. Past 90
        // degrees the image mirrors, which is what the back of a card looks like.
        DefineDouble(desc, page, g,
                     StageParam(end.isTo ? kParamTiltXTo : kParamTiltXFrom, i), "Tilt (X axis)",
                     "Pseudo-3D rotation about the horizontal axis, in degrees -- the image "
                     "tips towards or away from you. Orthographic, so it squashes vertically "
                     "rather than converging in perspective. 90 is edge-on and invisible.",
                     0.0, -360.0, 360.0, -180.0, 180.0, 1.0);

        DefineDouble(desc, page, g,
                     StageParam(end.isTo ? kParamSwivelYTo : kParamSwivelYFrom, i), "Swivel (Y axis)",
                     "Pseudo-3D rotation about the vertical axis, in degrees -- a card flip. "
                     "Animate -90 to 0 for a swing-in. 90 is edge-on and invisible; beyond it "
                     "the image mirrors, as the back of a card would.",
                     0.0, -360.0, 360.0, -180.0, 180.0, 1.0);

        DefineDouble(desc, page, g,
                     StageParam(end.isTo ? kParamOpacityTo : kParamOpacityFrom, i), "Opacity",
                     "Fade level as a percentage. Set From to 0 and To to 100 for a fade in, "
                     "or the reverse for a fade out. Opacity animates on this stage's own "
                     "timing and easing, so a fade can be staggered against the movement.",
                     100.0, 0.0, 100.0, 0.0, 100.0, 1.0);

    }


    // --- Motion path ---
    // The path belongs to the stage as a whole rather than to either end, since
    // it is the route between them.
    GroupParamDescriptor* gPath = DefineSection(desc, page, StageParam(kParamGroupPath, i),
        "Stage " + idx + " \xE2\x80\x94 Motion Path",
        "The route between the two poses. Far easier to drag in the viewer than to type.");

    DefineDouble2D(desc, page, gPath, StageParam(kParamPathC1, i), "Path Handle 1",
                   "Bends the first part of the trajectory away from a straight line. Easier "
                   "to drag on screen than to type: enable Open FX Overlay and pull the "
                   "handles on the dotted path. 0,0 is a straight line.",
                   0.0, 0.0);

    DefineDouble2D(desc, page, gPath, StageParam(kParamPathC2, i), "Path Handle 2",
                   "Bends the second part of the trajectory. 0,0 is a straight line.",
                   0.0, 0.0);

    PushButtonParamDescriptor* pathReset = desc.definePushButtonParam(StageParam(kParamPathReset, i));
    pathReset->setLabels("Straighten Path", "Straighten", "Straighten Path");
    pathReset->setHint("Reset both handles, returning the motion to a straight line.");
    pathReset->setParent(*gPath);
    page->addChild(*pathReset);

    // --- Easing ---
    GroupParamDescriptor* gEase = DefineSection(desc, page, StageParam(kParamGroupEasing, i),
        "Stage " + idx + " \xE2\x80\x94 Easing",
        "How the move accelerates. The curve editor in the viewer overlay edits the same values.");

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
    ease->setParent(*gEase);
    page->addChild(*ease);
    InvalidatesCache(ease);

    // These four are the actual curve, and are always editable -- a preset is a
    // starting point, never a locked choice. Raw bezier handles (X1/Y1/X2/Y2)
    // were unusable as a UI: nobody thinks in terms of "X1 = 0.42".
    DefineDouble(desc, page, gEase, StageParam(kParamEaseIn, i), "Ease In",
                 "Damping at the start. 0 leaves at full speed; 100 creeps away very "
                 "gradually. Raise this to soften the beginning of the move. Setting both "
                 "Ease In and Ease Out to 100 gives the steepest S-curve available.",
                 42.0, 0.0, 100.0, 0.0, 100.0, 1.0);

    DefineDouble(desc, page, gEase, StageParam(kParamEaseOut, i), "Ease Out",
                 "Damping at the end. 0 stops dead; 100 glides to a halt. This is the one "
                 "to reach for when a move feels like it arrives too abruptly. Still applies "
                 "when a Bounce is active: it shapes the approach into the landing.",
                 42.0, 0.0, 100.0, 0.0, 100.0, 1.0);

    // Negative values are meaningful and deliberately allowed: they push the
    // curve handle past the opposite rail, which is how the steep, snappy
    // curves are made. Clamping these to positive-only left half the curve
    // space unreachable.
    DefineDouble(desc, page, gEase, StageParam(kParamAnticipation, i), "Anticipation",
                 "Pulls back before moving, the way a character crouches before jumping. "
                 "0 is off. Negative values do the opposite: the move leaves hard and fast, "
                 "which steepens the start.",
                 0.0, -200.0, 200.0, -100.0, 100.0, 1.0);

    DefineDouble(desc, page, gEase, StageParam(kParamOvershoot, i), "Overshoot",
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
    InvalidatesCache(bounce);
    bounce->setParent(*gEase);
    page->addChild(*bounce);

    DefineDouble(desc, page, gEase, StageParam(kParamBounceAmount, i), "Bounce Amount",
                 "How far the first rebound travels, as a fraction of the whole move. 0 is no "
                 "bounce at all. Negative values flip which way it bounces: a spring "
                 "undershoots before it overshoots, and a ball rebounds off the near side of "
                 "the target rather than the far side. Reach for a negative value when the "
                 "bounce is going the wrong way for the move. Either way the move still lands "
                 "exactly on its target value.",
                 35.0, -100.0, 100.0, -100.0, 100.0, 1.0);

    DefineDouble(desc, page, gEase, StageParam(kParamBounceCount, i), "Bounces",
                 "How many rebounds happen before the move settles. Fractional values are "
                 "allowed and are useful for landing mid-rebound.",
                 3.0, 0.0, 12.0, 1.0, 8.0, 0.5);

    DefineDouble(desc, page, gEase, StageParam(kParamBounceDamping, i), "Bounce Damping",
                 "How quickly the rebounds shrink. 0 keeps them all the same size, which "
                 "reads as mechanical; higher values decay them away for a natural settle.",
                 45.0, 0.0, 100.0, 0.0, 100.0, 1.0);

    DefineDouble(desc, page, gEase, StageParam(kParamBounceStart, i), "Bounce Start",
                 "Where in the stage the move lands and the bouncing begins, as a percentage "
                 "of its duration. The easing curve is compressed into the part before this, "
                 "and everything after it is the bounce -- so lower values give a quicker "
                 "arrival and a longer bounce.",
                 55.0, 5.0, 95.0, 5.0, 95.0, 1.0);

    // Saving a curve to the library. A name field and a button rather than a
    // file dialog: the whole point of the library is that curves are picked
    // visually, and making the *saving* half go through Explorer would put the
    // friction straight back.
    StringParamDescriptor* curveName = desc.defineStringParam(StageParam(kParamCurveName, i));
    curveName->setLabels("Curve Name", "Curve Name", "Curve Name");
    curveName->setStringType(eStringTypeSingleLine);
    curveName->setHint("Name to save this stage's easing under. Saving with a name that already "
                       "exists overwrites it.");
    curveName->setDefault("");
    curveName->setIsPersistant(false);   // scratch text, not part of the animation
    curveName->setParent(*gEase);
    page->addChild(*curveName);

    PushButtonParamDescriptor* saveCurve = desc.definePushButtonParam(StageParam(kParamSaveCurve, i));
    saveCurve->setLabels("Save Curve to Library", "Save Curve", "Save Curve to Library");
    saveCurve->setHint("Store this easing in the curve library, where it can be picked by its "
                       "shape from the LIBRARY panel in the viewer overlay.");
    saveCurve->setParent(*gEase);
    page->addChild(*saveCurve);
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
    InvalidatesCache(count);
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

    // --- Root-level buttons ------------------------------------------------
    //
    // Everything below this is inside a collapsible group; these are not. They
    // are the controls reached constantly while working, and a disclosure arrow
    // in front of each would cost a click every time.
    //
    // They must all be defined BEFORE the first group. Resolve lays the panel
    // out in definition order, and ungrouped parameters that sit between two
    // groups get scattered -- that is how the preset buttons once ended up split
    // between the top and the bottom of the panel.
    for (int i = 0; i < kMaxStages; ++i) DefineStageRootButtons(p_Desc, page, i);
    PushButtonParamDescriptor* saveEffect = p_Desc.definePushButtonParam(kParamSaveEffect);
    saveEffect->setLabels("Save Preset to File...", "Save Preset", "Save Preset to File...");
    saveEffect->setHint("Write every stage, plus motion blur and sampling, to a JSON file.");
    page->addChild(*saveEffect);

    PushButtonParamDescriptor* saveStage = p_Desc.definePushButtonParam(kParamSaveStage);
    saveStage->setLabels("Save Active Stage to File...", "Save Stage",
                         "Save Active Stage to File...");
    saveStage->setHint("Write only the active stage, for building a library of reusable pieces "
                       "such as a punch in or a fade out.");
    page->addChild(*saveStage);

    PushButtonParamDescriptor* loadPreset = p_Desc.definePushButtonParam(kParamLoadPreset);
    loadPreset->setLabels("Load Preset from File...", "Load Preset", "Load Preset from File...");
    loadPreset->setHint("Apply a preset exactly as it was saved. Loads everything: all stages, "
                        "motion blur and sampling. A stage preset loads into the active stage.");
    page->addChild(*loadPreset);

    // Invisible: the overlay's LOAD button writes to it, and nothing else ever
    // should. See kParamLoadFromOverlay.
    BooleanParamDescriptor* overlayLoad = p_Desc.defineBooleanParam(kParamLoadFromOverlay);
    overlayLoad->setDefault(false);
    overlayLoad->setIsSecret(true);
    overlayLoad->setIsPersistant(false);
    page->addChild(*overlayLoad);

    PushButtonParamDescriptor* loadFit = p_Desc.definePushButtonParam(kParamLoadPresetFit);
    loadFit->setLabels("Load from File (Fit to Clip)...", "Load (Fit)",
                       "Load from File (Fit to Clip)...");
    loadFit->setHint("Loads exactly the same settings as Load Preset from File, with one "
                     "difference: frame-based Start and End values are rescaled to this clip's "
                     "length so the pacing is preserved. Stages anchored to Stretch are already "
                     "proportional and are left untouched.");
    page->addChild(*loadFit);


    GroupParamDescriptor* gOverlay = DefineSection(p_Desc, page, kParamGroupOverlay,
        "Viewer Overlay",
        "State shared with the on-screen controls. Both of these have a button in the overlay "
        "itself, so they are here only for when the overlay is switched off.");

    ChoiceParamDescriptor* target = p_Desc.defineChoiceParam(kParamEditTarget);
    target->setLabels("Gizmo Edits", "Gizmo Edits", "Gizmo Edits");
    target->setHint("Whether the on-screen gizmo poses the start or the end of the active "
                    "stage.");
    target->appendOption("From (start)");
    target->appendOption("To (end)");
    target->setDefault(1);
    target->setParent(*gOverlay);
    page->addChild(*target);

    BooleanParamDescriptor* showCurve = p_Desc.defineBooleanParam(kParamShowCurve);
    showCurve->setLabels("Show Curve Editor", "Curve Editor", "Show Curve Editor");
    showCurve->setHint("Draw the easing curve panel in the viewer overlay. Turn it off when it "
                       "sits over something you are trying to drag -- it covers the top-right "
                       "of the image and takes clicks before the motion path does. Also "
                       "toggled by the CURVE button in the overlay.");
    showCurve->setDefault(true);
    showCurve->setParent(*gOverlay);
    page->addChild(*showCurve);

    BooleanParamDescriptor* showLib = p_Desc.defineBooleanParam(kParamShowLibrary);
    showLib->setLabels("Show Curve Library", "Curve Library", "Show Curve Library");
    showLib->setHint("Draw the saved-curve picker in the viewer overlay. Also toggled by the "
                     "LIBRARY button there. Clicking a curve applies it to the active stage.");
    showLib->setDefault(false);
    showLib->setParent(*gOverlay);
    page->addChild(*showLib);

    // --- Base transform ---
    //
    // A resting pose applied underneath the animation, so a layer can simply sit
    // somewhere at some size without spending a whole stage on a From == To pair.
    // It composes innermost, which keeps each stage's translation measured in
    // frame widths rather than in base-scaled widths.
    GroupParamDescriptor* gBase = DefineSection(p_Desc, page, kParamGroupBase, "Base Transform",
        "A resting pose applied underneath the animation, so a layer can simply sit somewhere "
        "at some size without spending a stage on it.");

    BooleanParamDescriptor* baseLink = p_Desc.defineBooleanParam(kParamBaseLinkScale);
    baseLink->setLabels("Link Scale X/Y", "Link Scale", "Link Scale X/Y");
    baseLink->setHint("Scale both axes together. Turn this off to squash or stretch the "
                      "resting pose on one axis.");
    baseLink->setDefault(true);
    baseLink->setParent(*gBase);
    page->addChild(*baseLink);
    InvalidatesCache(baseLink);

    DefineDouble(p_Desc, page, gBase, kParamBaseScale, "Scale",
                 "Resting scale. Drives both axes while Link Scale X/Y is on; the X axis "
                 "alone once it is off. The animation multiplies on top of this.",
                 1.0, -100.0, 100.0, 0.0, 4.0, 0.01);

    DefineDouble(p_Desc, page, gBase, kParamBaseScaleY, "Scale Y",
                 "Resting vertical scale. Only used when Link Scale X/Y is off.",
                 1.0, -100.0, 100.0, 0.0, 4.0, 0.01);

    DefineDouble2D(p_Desc, page, gBase, kParamBasePos, "Position",
                   "Resting offset, normalised: 1.0 is one full image width/height. Stage "
                   "movement is added on top, in the same units.", 0.0, 0.0);

    DefineDouble(p_Desc, page, gBase, kParamBaseRot, "Rotation",
                 "Resting rotation in degrees.", 0.0, -100000.0, 100000.0, -360.0, 360.0, 1.0);

    DefineDouble(p_Desc, page, gBase, kParamBaseTiltX, "Tilt (X axis)",
                 "Resting pseudo-3D rotation about the horizontal axis. Orthographic, so it "
                 "squashes vertically rather than converging in perspective.",
                 0.0, -360.0, 360.0, -180.0, 180.0, 1.0);

    DefineDouble(p_Desc, page, gBase, kParamBaseSwivelY, "Swivel (Y axis)",
                 "Resting pseudo-3D rotation about the vertical axis -- a held card flip.",
                 0.0, -360.0, 360.0, -180.0, 180.0, 1.0);

    DefineDouble(p_Desc, page, gBase, kParamBaseOpacity, "Opacity",
                 "Resting opacity as a percentage. Multiplies with each stage's fade, so a "
                 "base of 50 and a stage fading 0 to 100 ends at 50.",
                 100.0, 0.0, 100.0, 0.0, 100.0, 1.0);

    DefineDouble2D(p_Desc, page, gBase, kParamBaseAnchor, "Anchor",
                   "Point the resting scale and rotation pivot around. 0.5, 0.5 is the image "
                   "centre.", 0.5, 0.5);

    PushButtonParamDescriptor* baseReset = p_Desc.definePushButtonParam(kParamBaseReset);
    baseReset->setLabels("Reset Base Transform", "Reset Base", "Reset Base Transform");
    baseReset->setHint("Return the resting pose to neutral, leaving the animation alone.");
    baseReset->setParent(*gBase);
    page->addChild(*baseReset);

    // --- Stages ---
    for (int i = 0; i < kMaxStages; ++i) DefineStage(p_Desc, page, i);

    // --- Presets ---
    //
    // Placed here, ahead of the Motion Blur and Sampling groups, because these
    // buttons have no parent group. Sitting between two GroupParams they were an
    // island of ungrouped parameters in the middle of grouped ones, and the
    // Inspector scattered them -- two ended up at the top of the panel and two
    // stayed at the bottom. Keeping every ungrouped parameter ahead of the first
    // group leaves the ordering unambiguous.
    GroupParamDescriptor* gPresets = DefineSection(p_Desc, page, kParamGroupPresets, "Presets",
        "Saving and loading setups as JSON files, and where those files live.");


    // The folder those four dialogs open in. A preference, not a parameter:
    // it is stored per user in settings.json rather than in the project, so it
    // applies to every instance of the effect on this machine and does not
    // travel with a timeline sent to someone else.
    StringParamDescriptor* folder = p_Desc.defineStringParam(kParamPresetFolder);
    folder->setLabels("Preset Folder", "Folder", "Preset Folder");

    // A single-line field, greyed out -- deliberately not eStringTypeLabel.
    // That is the same type the old section dividers used, and it is why they
    // were invisible in Resolve: the text lives in the parameter's value, and
    // Resolve appears to draw only the label. A real text field shows its
    // contents, which for a path read-out is the entire point.
    folder->setStringType(eStringTypeSingleLine);
    folder->setHint("Where the four buttons above open. Shared by every instance of this "
                    "effect, and remembered between sessions.");
    folder->setEnabled(false);
    folder->setIsPersistant(false);   // read back from preferences, never from the project
    folder->setParent(*gPresets);
    page->addChild(*folder);

    PushButtonParamDescriptor* setFolder = p_Desc.definePushButtonParam(kParamSetFolder);
    setFolder->setLabels("Set Preset Folder...", "Set Folder", "Set Preset Folder...");
    setFolder->setHint("Choose where presets are kept. Applies to every instance of this effect "
                       "and is remembered between sessions.");
    setFolder->setParent(*gPresets);
    page->addChild(*setFolder);

    PushButtonParamDescriptor* resetFolder = p_Desc.definePushButtonParam(kParamResetFolder);
    resetFolder->setLabels("Use Default Folder", "Default Folder", "Use Default Folder");
    resetFolder->setHint("Go back to Documents\\MultiTransform\\Presets. Nothing is moved or "
                         "deleted -- only where the dialogs open changes.");
    resetFolder->setParent(*gPresets);
    page->addChild(*resetFolder);

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
    InvalidatesCache(blurOn);
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
    InvalidatesCache(samples);
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
    InvalidatesCache(adaptive);
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
    InvalidatesCache(filter);
    filter->setParent(*sampling);
    page->addChild(*filter);

    ChoiceParamDescriptor* edge = p_Desc.defineChoiceParam(kParamEdge);
    edge->setLabels("Edges", "Edges", "Edges");
    edge->setHint("What to show where the transformed image no longer covers the frame.");
    edge->appendOption("Transparent");
    edge->appendOption("Clamp");
    edge->appendOption("Mirror");
    edge->setDefault(kEdgeBlack);
    InvalidatesCache(edge);
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
