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

    /// Where the source's pixel (0,0) sits relative to the destination's.
    /// Zero on the Edit page; non-zero in Fusion when the input is cropped to
    /// its domain of definition. See ImageView::originX.
    void setSrcOrigin(int x, int y) { _srcOriginX = x; _srcOriginY = y; }

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
        mtx::ImageView v;
        v.data = nullptr; v.width = 0; v.height = 0; v.rowStrideFloats = 0;
        if (!_srcImg) return v;

        const OfxRectI b = _srcImg->getBounds();
        v.data            = static_cast<const float*>(_srcImg->getPixelData());
        v.width           = b.x2 - b.x1;
        v.height          = b.y2 - b.y1;
        v.rowStrideFloats = _srcImg->getRowBytes() / static_cast<int>(sizeof(float));
        v.originX         = _srcOriginX;
        v.originY         = _srcOriginY;
        return v;
    }

    /// A source worth sampling: present, non-empty, with pixels behind it.
    /// Fusion fails all three in turn while scrubbing -- a null image, then a
    /// zero-sized one, then one with no data -- and each used to be a different
    /// way to read memory that was not there.
    bool haveSrc() const { return _srcImg && !srcView().Empty(); }

    OFX::Image*            _srcImg = nullptr;
    mtx::SampleTransforms  _transforms{};
    mtx::FilterMode        _filter = mtx::kFilterBilinear;
    mtx::EdgeMode          _edge   = mtx::kEdgeBlack;
    int                    _srcOriginX = 0;
    int                    _srcOriginY = 0;
};

void TransformProcessor::processImagesCUDA()
{
    // Deliberately no early return on a missing source. The launcher clears the
    // destination in that case, and it must: returning here left the host's
    // buffer holding its previous contents, which is a stale frame exactly when
    // a node's input disappears mid-scrub. An empty view is passed as nulls so
    // the launcher takes that path too.
    const mtx::ImageView sv = srcView();
    const bool live = !sv.Empty();

    float* dstData = static_cast<float*>(_dstImg->getPixelData());
    if (!dstData) return;

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
                              live ? sv.data : nullptr, sv.width, sv.height, sv.rowStrideFloats,
                              dstData, dstWidth, dstHeight, dstRowFloats,
                              _transforms,
                              static_cast<int>(_filter), static_cast<int>(_edge),
                              sv.originX, sv.originY);

    if (err) mtx::ProbeOnce("cuda-error", std::string("CUDA error: ") + err);
}

void TransformProcessor::multiThreadProcessImages(OfxRectI p_ProcWindow)
{
    const OfxRectI db = _dstImg->getBounds();

    // No usable source -- absent, empty or data-less -- renders transparent.
    // The sampler would now return black texel by texel for an empty image as
    // well, but taking the branch here is both faster and skips the ghost.
    if (!haveSrc())
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

    void             copyEffect();    ///< every setting -> clipboard file
    void             pasteEffect();   ///< clipboard file -> every setting
    void             copyStage();     ///< active stage -> clipboard
    void             pasteStage();    ///< clipboard -> active stage
    void             flattenToFirstStage();

    /** @brief Move poses between a stage's two ends.
     *  @param mode 0 copies From onto To, 1 copies To onto From, 2 swaps them. */
    void             transferEnds(int stage, int mode);

    /** @brief Run one QuickAction. Shared by the Inspector's Apply button and
     *  the overlay's QUICK panel, so the two can never drift apart. */
    void             runQuickAction(int action);

    /** @brief The stage the overlay is editing, clamped to a valid index. */
    int              activeStageIndex() const;
    void             savePreset(bool wholeEffect);
    void             loadPreset(bool fitToClip);
    /** The playhead expressed in the units a given stage's frames use. */
    double playheadInStageFrames(int stageIndex, double absoluteTime) const;

    /** Slide a stage so its fastest moment lands on the playhead, keeping its
     *  duration and every other value untouched. */
    /** Composite a faded copy of the source at the pose being dragged. */
    void   addPreviewGhost(SampleTransforms& st, const AnimParams& a,
                           double clipTime, float w, float h) const;

    void   syncPeakToPlayhead(int stageIndex, double absoluteTime);

    /** Same target, reached by rebalancing Ease In against Ease Out instead of
     *  moving the stage. */
    void   syncPeakByEasing(int stageIndex, double absoluteTime);
    /** @brief Refresh a stage's derived Duration read-out.
     *  @param editable true in an instance-changed or interact action, where a
     *         parameter edit block is permitted and needed; false in the
     *         constructor, which is neither. */
    void updateDuration(int stageIndex, bool editable = true);
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
    OFX::StringParam*  _presetFolder = nullptr;
    OFX::BooleanParam* _previewGhost = nullptr;

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
        OFX::DoubleParam*   posOffset;
        OFX::DoubleParam*   scaleOffset;
        OFX::DoubleParam*   rotOffset;
        OFX::DoubleParam*   tiltOffset;
        OFX::DoubleParam*   swivelOffset;
        OFX::DoubleParam*   opacityOffset;
        OFX::BooleanParam*  enabled;
        OFX::ChoiceParam*   timingAnchor;
        OFX::DoubleParam*   startFrame;
        OFX::DoubleParam*   endFrame;
        OFX::PushButtonParam* setStart;
        OFX::PushButtonParam* setEnd;
        OFX::DoubleParam*   duration;
        OFX::PushButtonParam* syncPeak;
        OFX::PushButtonParam* syncPeakEase;
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

    /// The stage syncStageVisibility last made visible. Starts at 0 because
    /// that is what the *descriptors* say: DefineStage marks stages 2 to 4
    /// secret, so a fresh instance already looks the way it should and the
    /// first pass has only the active stage to deal with. Lets that function
    /// skip stages whose state cannot have changed -- see the comment there.
    int  _visibleStage = 0;

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
    _previewGhost = fetchBooleanParam(kParamPreviewGhost);

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
        s.posOffset     = fetchDoubleParam  (StageParam(kParamPosOffset,     i));
        s.scaleOffset   = fetchDoubleParam  (StageParam(kParamScaleOffset,   i));
        s.rotOffset     = fetchDoubleParam  (StageParam(kParamRotOffset,     i));
        s.tiltOffset    = fetchDoubleParam  (StageParam(kParamTiltOffset,    i));
        s.swivelOffset  = fetchDoubleParam  (StageParam(kParamSwivelOffset,  i));
        s.opacityOffset = fetchDoubleParam  (StageParam(kParamOpacityOffset, i));
        s.enabled       = fetchBooleanParam (StageParam(kParamEnabled,       i));
        s.timingAnchor  = fetchChoiceParam  (StageParam(kParamAnchor2,       i));
        s.startFrame    = fetchDoubleParam  (StageParam(kParamStartFrame,    i));
        s.endFrame      = fetchDoubleParam  (StageParam(kParamEndFrame,      i));
        s.setStart      = fetchPushButtonParam(StageParam(kParamSetStart,    i));
        s.setEnd        = fetchPushButtonParam(StageParam(kParamSetEnd,      i));
        s.duration      = fetchDoubleParam  (StageParam(kParamDuration,      i));
        s.syncPeak      = fetchPushButtonParam(StageParam(kParamSyncPeak,    i));
        s.syncPeakEase  = fetchPushButtonParam(StageParam(kParamSyncPeakEase,i));
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
        // greyed out.
        _stage[i].duration->setEnabled(false);

        // Refreshed, but *without* an edit block and only when it is actually
        // wrong -- see updateDuration. Duration is persisted with the project,
        // so on reopening it is already correct and this writes nothing at all;
        // it is here for the case where start or end changed behind its back.
        updateDuration(i, /*editable=*/false);
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

        s.offsetPos     = static_cast<float>(h.posOffset->getValueAtTime(p_Time));
        s.offsetScale   = static_cast<float>(h.scaleOffset->getValueAtTime(p_Time));
        s.offsetRot     = static_cast<float>(h.rotOffset->getValueAtTime(p_Time));
        s.offsetTilt    = static_cast<float>(h.tiltOffset->getValueAtTime(p_Time));
        s.offsetSwivel  = static_cast<float>(h.swivelOffset->getValueAtTime(p_Time));
        s.offsetOpacity = static_cast<float>(h.opacityOffset->getValueAtTime(p_Time));

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

    s.offsetPos     = static_cast<float>(h.posOffset->getValue());
    s.offsetScale   = static_cast<float>(h.scaleOffset->getValue());
    s.offsetRot     = static_cast<float>(h.rotOffset->getValue());
    s.offsetTilt    = static_cast<float>(h.tiltOffset->getValue());
    s.offsetSwivel  = static_cast<float>(h.swivelOffset->getValue());
    s.offsetOpacity = static_cast<float>(h.opacityOffset->getValue());

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
    h.posOffset->setValue(s.offsetPos);
    h.scaleOffset->setValue(s.offsetScale);
    h.rotOffset->setValue(s.offsetRot);
    h.tiltOffset->setValue(s.offsetTilt);
    h.swivelOffset->setValue(s.offsetSwivel);
    h.opacityOffset->setValue(s.offsetOpacity);

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

namespace {

/** @brief The stage clipboard.
 *
 * Deliberately process-global rather than per instance. Copying a stage is at
 * least as useful between two clips as within one -- carrying a move across a
 * cut is the whole reason it exists -- and an instance-local clipboard could
 * only ever paste into the effect it was copied from.
 */
struct StageClipboard
{
    mtx::PresetStage stage;
    bool             valid = false;
};

StageClipboard g_stageClipboard;

} // namespace

void MultiTransformPlugin::copyEffect()
{
    // Exists because Resolve cannot copy a single OFX effect between clips --
    // it is all of them or none. This is the whole node in one press: every
    // stage, the base pose, motion blur and sampling.
    const std::string path = mtx::EffectClipboardPath();
    if (path.empty())
    {
        sendMessage(OFX::Message::eMessageError, "",
                    "Could not locate the application data folder to copy into.");
        return;
    }

    // The same serialisation a preset uses, so the payload is already covered
    // by the preset tests and a copy is readable if it ever needs inspecting.
    mtx::PresetData d = capturePreset(true);
    d.name = "Clipboard";

    std::string error;
    if (!mtx::WriteTextFile(path, mtx::ToJson(d), error))
    {
        sendMessage(OFX::Message::eMessageError, "", "Could not copy the settings:\n" + error);
        return;
    }
    mtx::ProbeLog("effect copied to clipboard");
}

void MultiTransformPlugin::pasteEffect()
{
    const std::string path = mtx::EffectClipboardPath();

    std::string text, error;
    if (path.empty() || !mtx::ReadTextFile(path, text, error))
    {
        sendMessage(OFX::Message::eMessageError, "",
                    "Nothing has been copied yet.\n\nUse Copy All Settings on another "
                    "Multi Transform first -- on any clip, in any project.");
        return;
    }

    mtx::PresetData d;
    if (!mtx::FromJson(text, d, error))
    {
        sendMessage(OFX::Message::eMessageError, "",
                    "The copied settings could not be read:\n" + error);
        return;
    }

    // Applied exactly as copied, with no rescaling to this clip's length. The
    // anchors already do the right thing -- Clip Start and Clip End are
    // relative, Stretch is proportional, and capturePreset converts a
    // Timeline-anchored stage to clip-relative on the way out. Load from File
    // (Fit to Clip) is there for when the pacing should be rescaled instead.
    d.wholeEffect = true;
    applyPreset(d);

    mtx::ProbeLog("effect pasted from clipboard");
}

int MultiTransformPlugin::activeStageIndex() const
{
    int active = 0;
    _activeStage->getValue(active);
    if (active < 0) return 0;
    if (active >= kMaxStages) return kMaxStages - 1;
    return active;
}

/** Everything here acts on the *active* stage rather than taking a stage index.
 *  That is the stage the gizmo, the timeline lane and the curve editor are
 *  already editing, so "the stage" is never ambiguous while working -- and it is
 *  what let eight buttons collapse into one dropdown, since the per-stage
 *  variants of the end transfers were only ever needed because a push button
 *  cannot ask which stage it means. */
void MultiTransformPlugin::runQuickAction(int action)
{
    switch (action)
    {
        case kQuickCopyFromTo:  transferEnds(activeStageIndex(), 0); break;
        case kQuickCopyToFrom:  transferEnds(activeStageIndex(), 1); break;
        case kQuickSwapEnds:    transferEnds(activeStageIndex(), 2); break;
        case kQuickCopyStage:   copyStage();          break;
        case kQuickPasteStage:  pasteStage();         break;
        case kQuickFlatten:     flattenToFirstStage(); break;
        case kQuickCopyEffect:  copyEffect();         break;
        case kQuickPasteEffect: pasteEffect();        break;
        default: break;
    }
}

void MultiTransformPlugin::copyStage()
{
    const int active = activeStageIndex();

    g_stageClipboard.stage = captureStage(active);
    g_stageClipboard.valid = true;

    mtx::ProbeLog("stage " + std::to_string(active + 1) + " copied");
}

void MultiTransformPlugin::pasteStage()
{
    if (!g_stageClipboard.valid)
    {
        sendMessage(OFX::Message::eMessageError, "",
                    "Nothing to paste.\n\nUse Copy Stage first -- on this clip or another one.");
        return;
    }

    const int active = activeStageIndex();

    {
        // Suppressed exactly as a preset load is: applyStage writes the easing
        // dropdown and its four amounts, and letting changedParam run in between
        // would have the dropdown stamp its canned values over them.
        _applyingPreset = true;
        mtx::EditBlock block(this, "Paste Stage");
        applyStage(active, g_stageClipboard.stage);
        _applyingPreset = false;
    }

    updateDuration(active);
    syncStageVisibility();
}

void MultiTransformPlugin::flattenToFirstStage()
{
    // Where the animation has finished: the composed pose there is the one the
    // next clip has to start from if the movement is to continue across a cut.
    double clipStart = 0.0, clipLength = 0.0;
    mtx::GetClipRange(this, clipStart, clipLength);

    const AnimParams a = fetchAnimParams(clipStart);
    const float endT   = AnimationEndTime(a, 0.0f);

    OfxRectD rod = _srcClip ? _srcClip->getRegionOfDefinition(clipStart) : OfxRectD{ 0, 0, 0, 0 };
    float w = static_cast<float>(rod.x2 - rod.x1);
    float h = static_cast<float>(rod.y2 - rod.y1);
    if (!(w > 1.0f) || !(h > 1.0f)) { w = 1920.0f; h = 1080.0f; }

    const FlatPose f = FlattenTransform(a, endT, w, h);

    {
        mtx::EditBlock block(this, "Flatten to Stage 1");
        _applyingPreset = true;

        // The base is folded into the flattened pose, so leaving it set would
        // apply it a second time.
        _baseScale->setValue(1.0);      _baseScaleY->setValue(1.0);
        _baseLinkScale->setValue(true); _basePos->setValue(0.0, 0.0);
        _baseRot->setValue(0.0);        _baseTiltX->setValue(0.0);
        _baseSwivelY->setValue(0.0);    _baseOpacity->setValue(100.0);
        _baseAnchor->setValue(0.5, 0.5);

        // Stage 1 holds the flattened pose at both ends. A hold rather than a
        // move: the next thing the user does is drag the To somewhere, and
        // anything else here would animate away from the pose just inherited.
        mtx::PresetStage s = mtx::PresetStage::Default();
        s.enabled    = true;
        s.anchor     = kAnchorClipStart;
        s.startFrame = 0.0f;
        s.endFrame   = 24.0f;
        s.anchorX    = 0.5f;   s.anchorY  = 0.5f;
        s.linkScale  = false;              // the two axes rarely match after this
        s.scaleFrom  = f.scaleX;  s.scaleTo  = f.scaleX;
        s.scaleYFrom = f.scaleY;  s.scaleYTo = f.scaleY;
        s.rotFrom    = f.rot;     s.rotTo    = f.rot;
        s.posXFrom   = f.posX;    s.posXTo   = f.posX;
        s.posYFrom   = f.posY;    s.posYTo   = f.posY;
        // FlatPose carries opacity as 0..1, the way the renderer uses it, but a
        // PresetStage holds raw parameter values and the Opacity parameter is a
        // percentage. Without this the whole animation flattened to one percent.
        s.opacityFrom = f.opacity * 100.0f; s.opacityTo = f.opacity * 100.0f;

        // Tilt and swivel are orthographic scale factors, so the decomposition
        // has already absorbed them into scaleX and scaleY.
        applyStage(0, s);

        // Everything else goes back to neutral and switches off.
        for (int i = 1; i < kMaxStages; ++i)
        {
            mtx::PresetStage blank = mtx::PresetStage::Default();
            blank.enabled = false;
            applyStage(i, blank);
        }
        _stageCount->setValue(0);   // one stage

        _applyingPreset = false;
    }

    for (int i = 0; i < kMaxStages; ++i) updateDuration(i);
    syncStageVisibility();

    // Shear is the one thing a stage cannot express, so if any was dropped the
    // result genuinely does not match what was on screen and saying nothing
    // would be the wrong call.
    if (std::fabs(f.shear) > 0.01f)
    {
        sendMessage(OFX::Message::eMessageWarning, "",
                    "Flattened, but this animation contained shear that a single stage "
                    "cannot represent, so the pose is approximate.\n\n"
                    "Shear comes from scaling one axis after a rotation -- an outer stage "
                    "with Link Scale off above an inner stage that rotates.");
    }
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

void MultiTransformPlugin::addPreviewGhost(SampleTransforms& st, const AnimParams& a,
                                           double clipTime, float w, float h) const
{
    // Only while a gizmo drag is actually happening. The overlay sets this on
    // pen-down and clears it on pen-up and on every drag-recovery path, and the
    // parameter is not persisted, so a project can never be reopened -- or
    // rendered -- with a ghost still in it.
    bool wanted = false;
    _previewGhost->getValue(wanted);
    if (!wanted) return;

    // The pose the gizmo is dragging, which is the whole point: the viewer is
    // showing the frame the playhead sits on, so without this there is nothing
    // on screen that looks like the pose being edited.
    //
    // Evaluated at the *same* reference the gizmo uses -- the active stage's own
    // start or end frame, or the playhead for the base -- so the ghost lands
    // exactly inside the gizmo's outline rather than near it.
    int active = 0;
    _activeStage->getValue(active);
    if (active < 0) active = 0;
    if (active >= kMaxStages) active = kMaxStages - 1;

    int target = 1;
    fetchChoiceParam(kParamEditTarget)->getValue(target);

    float refTime = static_cast<float>(clipTime);          // 2 == the base pose
    if (target != 2)
    {
        const Stage& s = a.stages[active];
        refTime = ClipTimeFromStageFrame(a, s, target == 1 ? s.endFrame : s.startFrame);
    }

    const Mat3 fwd = EvaluateTransform(a, refTime, w, h);

    // A collapsed pose has no inverse worth sampling through, and Invert falls
    // back to the identity -- which would slap a full-size ghost over the frame.
    // Same trap the edge-on swivel set in BuildSampleTransforms.
    const float area   = w * h;
    const float minDet = area > 1.0f ? 1.0f / area : 1e-6f;
    const float det    = fwd.Determinant();
    if (det > -minDet && det < minDet) return;

    st.hasGhost     = true;
    st.ghostInv     = Invert(fwd);
    st.ghostOpacity = 0.9f;

    // Tinted towards the gizmo's own colour, so which end is being posed reads
    // from the picture as well as from the outline -- and so the preview is
    // never mistaken for the finished frame. Mixed halfway rather than applied
    // straight: a full multiply drowns the image, and the point is to see the
    // picture with a cast over it. Values mirror colours::kGizmo / kGizmoTo /
    // kStage[3] in the overlay, which cannot be included here.
    const float kFrom[3] = { 0.20f, 0.85f, 1.00f };   // cyan
    const float kTo  [3] = { 1.00f, 0.55f, 0.20f };   // orange
    const float kBase[3] = { 0.85f, 0.55f, 1.00f };   // violet

    const float* hue = (target == 2) ? kBase : (target == 1 ? kTo : kFrom);

    constexpr float kMix = 0.5f;
    for (int ch = 0; ch < 3; ++ch)
        st.ghostTint[ch] = 1.0f + (hue[ch] - 1.0f) * kMix;
}

void MultiTransformPlugin::syncPeakToPlayhead(int stageIndex, double absoluteTime)
{
    StageParamHandles& h = _stage[stageIndex];

    const double start = h.startFrame->getValue();
    const double end   = h.endFrame->getValue();
    const double span  = end - start;

    if (std::fabs(span) < 1e-6)
    {
        sendMessage(OFX::Message::eMessageError, "",
                    "This stage has no duration, so it has no moment of peak acceleration "
                    "to sync. Give it a Start and End that differ first.");
        return;
    }

    // The same curve the renderer uses, and the same peak the timeline lane
    // marks with its red tick -- so the frame this snaps to is the frame you
    // can already see.
    int bounceType = kBounceNone;
    h.bounceType->getValue(bounceType);

    const Easing easing = MakeEasing(static_cast<float>(h.easeIn->getValue()),
                                     static_cast<float>(h.easeOut->getValue()),
                                     static_cast<float>(h.anticipation->getValue()),
                                     static_cast<float>(h.overshoot->getValue()),
                                     bounceType,
                                     static_cast<float>(h.bounceAmount->getValue()),
                                     static_cast<float>(h.bounceCount->getValue()),
                                     static_cast<float>(h.bounceDamping->getValue()),
                                     static_cast<float>(h.bounceStart->getValue()));

    // PeakVelocityProgress is a fraction of the stage's *own* span, so it lands
    // in the same units Start and End are written in whatever the anchor is --
    // frames for most, percentages under Stretch.
    const double peak     = start + static_cast<double>(PeakVelocityProgress(easing)) * span;
    const double playhead = playheadInStageFrames(stageIndex, absoluteTime);

    double delta = playhead - peak;

    // Whole frames, except under Stretch where the values are percentages and
    // rounding would be a much coarser move than it looks.
    int anchor = kAnchorClipStart;
    h.timingAnchor->getValue(anchor);
    if (anchor != kAnchorStretch) delta = std::floor(delta + 0.5);

    if (std::fabs(delta) < 1e-9) return;   // already synced; nothing to record

    // Both ends by the same amount: the span is preserved exactly, so the
    // duration, the easing and the shape of the move are all untouched. Only
    // when it happens changes.
    mtx::EditBlock block(this, "Sync Acceleration to Playhead");
    h.startFrame->setValue(start + delta);
    h.endFrame->setValue(end + delta);
}

void MultiTransformPlugin::syncPeakByEasing(int stageIndex, double absoluteTime)
{
    StageParamHandles& h = _stage[stageIndex];

    const double start = h.startFrame->getValue();
    const double end   = h.endFrame->getValue();
    const double span  = end - start;

    if (std::fabs(span) < 1e-6)
    {
        sendMessage(OFX::Message::eMessageError, "",
                    "This stage has no duration, so it has no moment of peak acceleration "
                    "to sync. Give it a Start and End that differ first.");
        return;
    }

    // Where the playhead sits *within* the stage. Unlike the shift button, this
    // one cannot move the stage to reach the playhead, so a playhead outside the
    // stage has no answer at all rather than a poor one.
    const double playhead = playheadInStageFrames(stageIndex, absoluteTime);
    const double target   = (playhead - start) / span;

    if (target < 0.0 || target > 1.0)
    {
        sendMessage(OFX::Message::eMessageError, "",
                    "The playhead is outside this stage, so there is no point on its curve to "
                    "move the acceleration to.\n\nPark the playhead within the stage, or use "
                    "Sync Acceleration to Playhead to slide the stage to the playhead instead.");
        return;
    }

    int bounceType = kBounceNone;
    h.bounceType->getValue(bounceType);

    const Easing current = MakeEasing(static_cast<float>(h.easeIn->getValue()),
                                      static_cast<float>(h.easeOut->getValue()),
                                      static_cast<float>(h.anticipation->getValue()),
                                      static_cast<float>(h.overshoot->getValue()),
                                      bounceType,
                                      static_cast<float>(h.bounceAmount->getValue()),
                                      static_cast<float>(h.bounceCount->getValue()),
                                      static_cast<float>(h.bounceDamping->getValue()),
                                      static_cast<float>(h.bounceStart->getValue()));

    // Both amounts are free to go anywhere, including changing how much easing
    // there is overall. Matching the playhead is the only thing this button is
    // for, so it is not held back by preserving the curve's original softness --
    // and a linear stage is reshaped rather than refused, since "no easing yet"
    // is a starting point and not an obstacle.
    float easeIn = 0.0f, easeOut = 0.0f, achieved = 0.0f;
    SolvePeakEasing(current, static_cast<float>(target), easeIn, easeOut, achieved);

    // No _syncingEasing guard needed: writing these is exactly the case
    // markEasingCustom exists for, and flipping the dropdown to Custom is the
    // correct consequence of hand-shaping the curve.
    mtx::EditBlock block(this, "Sync Acceleration by Easing");
    h.easeIn->setValue(easeIn);
    h.easeOut->setValue(easeOut);
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

    const char* label = (mode == 0) ? "Copy A to B"
                      : (mode == 1) ? "Copy B to A"
                                    : "Swap A and B";
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

        // A stage that was already hidden and stays hidden is skipped outright.
        //
        // Each pass below is roughly forty setIsSecret calls, every one a round
        // trip into the host, and this function runs whenever the link toggle,
        // the bounce type, the timing anchor or the active stage changes. For a
        // hidden stage every combined rule collapses to "hidden" regardless of
        // what those values are, and its labels are not on screen to be read, so
        // there is nothing a repeat pass could put right.
        //
        // _visibleStage starts at 0 rather than at a "nothing known yet"
        // sentinel, because the starting state *is* known: DefineStage marks
        // stages 2 to 4 secret on the descriptor. A fresh instance whose active
        // stage is 1 therefore does one pass here instead of four.
        if (hidden && i != _visibleStage) continue;

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
        s.posOffset->setIsSecret(hidden);
        s.scaleOffset->setIsSecret(hidden);
        s.rotOffset->setIsSecret(hidden);
        s.tiltOffset->setIsSecret(hidden);
        s.swivelOffset->setIsSecret(hidden);
        s.opacityOffset->setIsSecret(hidden);
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

        s.syncPeak->setIsSecret(hidden);
        s.syncPeakEase->setIsSecret(hidden);
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

    _visibleStage = active;
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

void MultiTransformPlugin::updateDuration(int stageIndex, bool editable)
{
    const double start = _stage[stageIndex].startFrame->getValue();
    const double end   = _stage[stageIndex].endFrame->getValue();
    const double want  = end - start;

    // Nothing written unless the value is actually wrong.
    //
    // This matters far more than it looks. The constructor calls this for all
    // four stages, and the constructor runs every time a clip is *selected* in
    // Resolve -- so an unconditional write meant four parameter writes, each
    // inside its own paramEditBegin/End, on every click. That is four undo
    // entries the user never asked for, and four host round trips, for a value
    // that is persisted with the project and therefore already correct.
    //
    if (std::fabs(_stage[stageIndex].duration->getValue() - want) < 1e-9) return;

    // The edit block is conditional for the same reason. A plugin write outside
    // one leaves the host unaware the value changed, so the interactive callers
    // -- changedParam, the timing migration -- still need it. The constructor
    // must not open one: the OFX spec restricts edit blocks to instance-changed
    // and interact actions, and an undo entry created merely by selecting a clip
    // is wrong however cheap it is.
    if (!editable)
    {
        _stage[stageIndex].duration->setValue(want);
        return;
    }

    mtx::EditBlock block(this, "Duration");
    _stage[stageIndex].duration->setValue(want);
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

        // The resolved folder is cached for the life of the process, so a
        // preference that has just changed has to say so.
        mtx::InvalidatePresetFolderCache();
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

        // The resolved folder is cached for the life of the process, so a
        // preference that has just changed has to say so.
        mtx::InvalidatePresetFolderCache();
        syncPresetFolderLabel();
        return;
    }

    if (p_ParamName == kParamSaveEffect)    { savePreset(true);  return; }
    if (p_ParamName == kParamSaveStage)     { savePreset(false); return; }
    if (p_ParamName == kParamLoadPreset)    { loadPreset(false); return; }
    if (p_ParamName == kParamLoadPresetFit) { loadPreset(true);  return; }
    if (p_ParamName == kParamLoadFromOverlay) { loadPreset(false); return; }

    // Quick Control. The Inspector's Apply button and the overlay's QUICK panel
    // both land here, and both read the action out of the same choice parameter
    // -- the overlay sets it before flipping its trigger, so one trigger covers
    // all eight actions rather than needing a hidden button each.
    if (p_ParamName == kParamQuickApply || p_ParamName == kParamQuickFromOverlay)
    {
        int action = kQuickCopyFromTo;
        fetchChoiceParam(kParamQuickAction)->getValue(action);
        runQuickAction(action);
        return;
    }

    // The overlay's copies act on whichever stage is active, since that is the
    // stage its timeline lane and gizmo are already editing.
    if (p_ParamName == kParamSyncPeakFromOverlay || p_ParamName == kParamSyncEaseFromOverlay)
    {
        int active = 0;
        _activeStage->getValue(active);
        if (active < 0) active = 0;
        if (active >= kMaxStages) active = kMaxStages - 1;

        if (p_ParamName == kParamSyncPeakFromOverlay) syncPeakToPlayhead(active, p_Args.time);
        else                                          syncPeakByEasing(active, p_Args.time);
        return;
    }

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
        if (p_ParamName == StageParam(kParamSyncPeak, i))
        {
            syncPeakToPlayhead(i, p_Args.time);
            return;
        }
        if (p_ParamName == StageParam(kParamSyncPeakEase, i))
        {
            syncPeakByEasing(i, p_Args.time);
            return;
        }
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

    // The frame the transform is evaluated in is the *destination's* bounds.
    //
    // It used to be the source's, and on the Edit page that is the same thing.
    // Fusion crops a node's input to its domain of definition, so there the
    // source can be a small rectangle offset inside the frame -- and measured
    // against that, "position 1.0 = one image width" and "anchor 0.5 = the
    // centre" would mean the element's own little box rather than the canvas.
    // The destination is the canvas; the source's origin within it is handed to
    // the sampler so the pixels are still read from where they really are.
    const OfxRectI db = dst->getBounds();
    int srcOriginX = 0, srcOriginY = 0;

    if (src)
    {
        const OfxRectI sb = src->getBounds();
        srcOriginX = sb.x1 - db.x1;
        srcOriginY = sb.y1 - db.y1;

        // One line per process about what this host actually hands over, so
        // the next Fusion report can be read against facts rather than guesses.
        if (sb.x1 != db.x1 || sb.y1 != db.y1 || sb.x2 != db.x2 || sb.y2 != db.y2)
        {
            std::ostringstream o;
            o << "render: source bounds differ from destination -- src ["
              << sb.x1 << "," << sb.y1 << " - " << sb.x2 << "," << sb.y2 << "] dst ["
              << db.x1 << "," << db.y1 << " - " << db.x2 << "," << db.y2 << "]"
              << (src->getPixelData() ? "" : " (source has no pixel data)");
            mtx::ProbeOnce("src-dst-bounds-differ", o.str());
        }

        // Evaluate in clip time, not timeline time. Stage start/end are frames
        // measured from the clip's first frame, so the animation stays put when
        // the clip is moved or trimmed rather than being anchored to absolute
        // timeline positions that stop meaning anything the moment the edit
        // changes.
        const double clipTime = mtx::ToClipTime(this, p_Args.time);

        const AnimParams anim = fetchAnimParams(p_Args.time);
        const float      w    = static_cast<float>(db.x2 - db.x1);
        const float      h    = static_cast<float>(db.y2 - db.y1);

        st = BuildSampleTransforms(anim, fetchBlurParams(p_Args.time),
                                   static_cast<float>(clipTime), w, h);

        addPreviewGhost(st, anim, clipTime, w, h);
    }

    TransformProcessor processor(*this);
    processor.setDstImg(dst.get());
    processor.setSrcImg(src.get());
    processor.setSrcOrigin(srcOriginX, srcOriginY);
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

/** @brief Describe-time secrecy, applied to everything created while it is set.
 *
 * Only the active stage's controls are ever shown, and until this existed the
 * instance constructor did all of that hiding itself: roughly forty setIsSecret
 * calls per stage, for all four stages, on every instance built. That measured
 * at 4.2 to 5.7 ms of a 4.5 to 6.0 ms constructor -- about 93% of it -- because
 * changing a parameter's secrecy makes the host redraw the Inspector.
 *
 * Stating it on the descriptor instead means the host starts out with the right
 * answer for stages 2 to 4 and the constructor has nothing to correct. It took
 * the constructor to ~1.8 ms. Switching stages still costs a redraw, which is
 * fine: that is a redraw the user asked for.
 *
 * Worth knowing for whoever comes here next: this did *not* fix the lag on
 * selecting a clip, because instances are built when the timeline loads, not
 * when a clip is selected. Six separate hooks -- the constructor, beginEdit,
 * endEdit, getClipPreferences, isIdentity, render and the overlay draw -- were
 * all measured silent during selection. Whatever that cost is, it is inside the
 * host and not in this file.
 *
 * A flag rather than an argument threaded through a dozen helpers, because it
 * has exactly one setter and its scope is one function -- see DefineStage.
 */
bool g_describeSecret = false;

template <class T>
T* Secret(T* p)
{
    if (g_describeSecret) p->setIsSecret(true);
    return p;
}

/// Sets g_describeSecret for a scope and always clears it, so a return or a
/// throw part-way through describing a stage cannot leak it onto the next one.
struct DescribeSecretScope
{
    explicit DescribeSecretScope(bool secret) { g_describeSecret = secret; }
    ~DescribeSecretScope() { g_describeSecret = false; }
};

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
    return Secret(InvalidatesCache(p));
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
    return Secret(InvalidatesCache(p));
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
    Secret(g);

    // Collapsed by default. One active stage puts about seventy rows in the
    // Inspector, and the great majority of them are set once and then left --
    // a base pose, an easing curve, a preset folder. Opening a section to reach
    // those is cheaper than scrolling past them every time.
    g->setOpen(false);

    page->addChild(*g);
    return g;
}

void DefineStage(OFX::ImageEffectDescriptor& desc, PageParamDescriptor* page, int i)
{
    const std::string idx = std::to_string(i + 1);

    // Only one stage is ever visible at a time: the other three stages have
    // every parameter AND every group hidden, so the Inspector shows one
    // stage's five sections rather than twenty.
    //
    // Stated here, on the descriptors, rather than left for the instance
    // constructor to impose. Stage 1 is the one the host starts with showing,
    // which is why the instance can begin from _visibleStage = 0 and correct
    // nothing. See Secret() for what that measurement was.
    DescribeSecretScope secrecy(i != 0);

    // --- Timing ---
    GroupParamDescriptor* gTiming = DefineSection(desc, page, StageParam(kParamGroupTiming, i),
        "Stage " + idx + " \xE2\x80\x94 Timing",
        "When this stage runs, and what its frame numbers are measured from.");

    BooleanParamDescriptor* en = Secret(desc.defineBooleanParam(StageParam(kParamEnabled, i)));
    en->setLabels("Enabled", "Enabled", "Enabled");
    en->setHint("Include this stage in the combined transform. Stages COMBINE rather than "
                "replace: two stages each scaling 1.0 to 1.5 give 2.25x overall.");
    en->setDefault(i == 0);
    en->setParent(*gTiming);
    page->addChild(*en);
    InvalidatesCache(en);

    ChoiceParamDescriptor* anchor = Secret(desc.defineChoiceParam(StageParam(kParamAnchor2, i)));
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

    PushButtonParamDescriptor* setStart = Secret(desc.definePushButtonParam(StageParam(kParamSetStart, i)));
    setStart->setLabels("Set Start to Playhead", "Set Start", "Set Start to Playhead");
    setStart->setHint("Park the playhead where this stage should begin and click.");
    setStart->setParent(*gTiming);
    page->addChild(*setStart);

    PushButtonParamDescriptor* setEnd = Secret(desc.definePushButtonParam(StageParam(kParamSetEnd, i)));
    setEnd->setLabels("Set End to Playhead", "Set End", "Set End to Playhead");
    setEnd->setHint("Park the playhead where this stage should finish and click.");
    setEnd->setParent(*gTiming);
    page->addChild(*setEnd);

    PushButtonParamDescriptor* syncPeak = Secret(desc.definePushButtonParam(StageParam(kParamSyncPeak, i)));
    syncPeak->setLabels("Sync Acceleration to Playhead", "Sync Accel",
                        "Sync Acceleration to Playhead");
    syncPeak->setHint("Slide this stage so the fastest moment of its move lands on the playhead. "
                      "Start and End shift together, so the duration, the easing and every other "
                      "value are untouched -- only when the move happens changes. The moment it "
                      "syncs to is the red tick on the stage's timeline lane in the overlay.");
    syncPeak->setParent(*gTiming);
    page->addChild(*syncPeak);

    PushButtonParamDescriptor* syncEase =
        Secret(desc.definePushButtonParam(StageParam(kParamSyncPeakEase, i)));
    syncEase->setLabels("Sync Acceleration by Easing", "Sync Accel (Easing)",
                        "Sync Acceleration by Easing");
    syncEase->setHint("Same target as the button above, reached the other way: the stage stays "
                      "exactly where it is and the curve is reshaped instead. Ease In and Ease "
                      "Out are rebalanced against each other -- their total is preserved, so the "
                      "move stays about as soft as it was -- and anticipation, overshoot and "
                      "bounce are left alone. Use this when the timing is locked and only the "
                      "feel may move.");
    syncEase->setParent(*gTiming);
    page->addChild(*syncEase);

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

    // --- Per-channel timing offsets ---
    //
    // Every channel shares the stage's start, end and easing; an offset slides
    // that one channel's copy of the window. Shift, not squeeze: the channel
    // keeps its duration and curve and simply runs later (or earlier, when
    // negative). This is the in-stage version of what stages themselves do for
    // whole properties -- a fade trailing its move no longer costs a second
    // stage of the four.
    {
        struct OffsetSpec { const char* param; const char* label; const char* what; };
        const OffsetSpec offs[6] = {
            { kParamPosOffset,     "Position Offset", "the move along the motion path" },
            { kParamScaleOffset,   "Scale Offset",    "both scale axes"                },
            { kParamRotOffset,     "Rotation Offset", "the in-plane rotation"          },
            { kParamTiltOffset,    "Tilt Offset",     "the tilt about the X axis"      },
            { kParamSwivelOffset,  "Swivel Offset",   "the swivel about the Y axis"    },
            { kParamOpacityOffset, "Opacity Offset",  "the fade"                       },
        };

        for (const OffsetSpec& o : offs)
        {
            DefineDouble(desc, page, gTiming, StageParam(o.param, i), o.label,
                         std::string("Delays ") + o.what + " by this many frames against the "
                         "stage's own timing (negative leads). The channel keeps the stage's "
                         "duration and easing -- it just runs shifted, holding its A value "
                         "until its window opens. Under a Stretch anchor this is in percent "
                         "of the clip, like Start and End.",
                         0.0, -100000.0, 100000.0, -48.0, 48.0, 1.0);
        }
    }

    DefineDouble2D(desc, page, gTiming, StageParam(kParamAnchor, i), "Anchor",
                   "Point that scale and rotation pivot around. 0.5, 0.5 is the image centre. "
                   "Shared by both ends -- a pivot that moved mid-move would make the motion "
                   "impossible to reason about.",
                   0.5, 0.5);

    // Shared by both ends, like the anchor: scale that were linked at one end and
    // split at the other would be two different kinds of animation in one stage.
    BooleanParamDescriptor* link = Secret(desc.defineBooleanParam(StageParam(kParamLinkScale, i)));
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
    // A and B rather than From and To, throughout the interface.
    //
    // Purely a change of wording: every parameter name, struct field and flag
    // underneath still says From and To, because renaming those would break
    // every saved project and preset for no gain. "From" and "To" read badly
    // aloud and in a sentence -- "copy from to to" -- and as a pair of labels
    // they are lopsided, one three letters and one two. A and B are symmetric,
    // unambiguous and short enough for a 26px overlay button.
    struct EndSpec { const char* groupName; const char* heading; bool isTo; };
    const EndSpec ends[2] = {
        { kParamGroupFrom, "A (start)", false },
        { kParamGroupTo,   "B (end)",   true  }
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
                     "Fade level as a percentage. Set A to 0 and B to 100 for a fade in, "
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

    PushButtonParamDescriptor* pathReset = Secret(desc.definePushButtonParam(StageParam(kParamPathReset, i)));
    pathReset->setLabels("Straighten Path", "Straighten", "Straighten Path");
    pathReset->setHint("Reset both handles, returning the motion to a straight line.");
    pathReset->setParent(*gPath);
    page->addChild(*pathReset);

    // --- Easing ---
    GroupParamDescriptor* gEase = DefineSection(desc, page, StageParam(kParamGroupEasing, i),
        "Stage " + idx + " \xE2\x80\x94 Easing",
        "How the move accelerates. The curve editor in the viewer overlay edits the same values.");

    ChoiceParamDescriptor* ease = Secret(desc.defineChoiceParam(StageParam(kParamEasingPreset, i)));
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
    ChoiceParamDescriptor* bounce = Secret(desc.defineChoiceParam(StageParam(kParamBounceType, i)));
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
    StringParamDescriptor* curveName = Secret(desc.defineStringParam(StageParam(kParamCurveName, i)));
    curveName->setLabels("Curve Name", "Curve Name", "Curve Name");
    curveName->setStringType(eStringTypeSingleLine);
    curveName->setHint("Name to save this stage's easing under. Saving with a name that already "
                       "exists overwrites it.");
    curveName->setDefault("");
    curveName->setIsPersistant(false);   // scratch text, not part of the animation
    curveName->setParent(*gEase);
    page->addChild(*curveName);

    PushButtonParamDescriptor* saveCurve = Secret(desc.definePushButtonParam(StageParam(kParamSaveCurve, i)));
    saveCurve->setLabels("Save Curve to Library", "Save Curve", "Save Curve to Library");
    saveCurve->setHint("Store this easing in the curve library, where it can be picked by its "
                       "shape from the LIBRARY panel in the viewer overlay.");
    saveCurve->setParent(*gEase);
    page->addChild(*saveCurve);
}

} // namespace

/** @brief Defines all ~198 parameters. Measured at 4.35 ms, and Resolve calls it
 *  exactly once per plugin load rather than once per clip selection -- so the
 *  size of this function is not what makes selecting a clip feel slow. */
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
    // Quick Control: one dropdown and one Apply, in place of the eight push
    // buttons these actions used to have. See kParamQuickAction for why they are
    // collapsed rather than laid out, and why Apply is separate from the choice.
    ChoiceParamDescriptor* quick = p_Desc.defineChoiceParam(kParamQuickAction);
    quick->setLabels("Quick Control", "Quick", "Quick Control");
    quick->setHint("One-shot actions on the active stage or the whole effect. Choose one, then "
                   "press Apply. Nothing happens until Apply is pressed.");
    for (int a = 0; a < kQuickActionCount; ++a) quick->appendOption(QuickActionLabel(a));
    quick->setDefault(kQuickCopyFromTo);
    quick->setAnimates(false);
    page->addChild(*quick);

    PushButtonParamDescriptor* quickApply = p_Desc.definePushButtonParam(kParamQuickApply);
    quickApply->setLabels("Apply", "Apply", "Apply");
    quickApply->setHint("Run the chosen Quick Control action.\n\n"
                        "Copy/Swap A and B move poses between the two ends of the active "
                        "stage. Copy/Paste Stage move a whole stage -- pose, timing, easing and "
                        "path -- through a clipboard shared by every instance of this effect. "
                        "Copy/Paste All Settings move the entire effect, which is otherwise "
                        "impossible because Resolve can only copy all of a clip's effects or "
                        "none of them. Flatten collapses the finished animation into a single "
                        "held pose in Stage 1 and clears the rest, for continuing a move across "
                        "a cut.");
    page->addChild(*quickApply);

    // Overlay triggers. Hidden and not persisted -- these are messages, not
    // settings. See kParamLoadFromOverlay for why a boolean and not a button.
    const char* const kTriggers[3] = { kParamQuickFromOverlay,
                                       kParamSyncPeakFromOverlay, kParamSyncEaseFromOverlay };
    for (const char* name : kTriggers)
    {
        BooleanParamDescriptor* t = p_Desc.defineBooleanParam(name);
        t->setDefault(false);
        t->setIsSecret(true);
        t->setIsPersistant(false);
        page->addChild(*t);
    }
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
    target->setHint("What the on-screen gizmo poses: the start or the end of the active stage, or "
                    "the Base Transform. Also set by the A / B / BASE buttons in the overlay.");
    target->appendOption("A (start)");
    target->appendOption("B (end)");
    target->appendOption("Base Transform");
    target->setDefault(1);
    target->setParent(*gOverlay);
    page->addChild(*target);

    BooleanParamDescriptor* showCurve = p_Desc.defineBooleanParam(kParamShowCurve);
    showCurve->setLabels("Show Curve Editor", "Curve Editor", "Show Curve Editor");
    showCurve->setHint("Draw the easing curve panel in the viewer overlay. Turn it off when it "
                       "sits over something you are trying to drag -- it covers the top-right "
                       "of the image and takes clicks before the motion path does. Also "
                       "toggled by the CURVE button in the overlay.");
    // Off by default now: the overlay opens as gizmo + timeline + toolbar, and
    // every other panel is switched on when it is wanted.
    showCurve->setDefault(false);
    showCurve->setParent(*gOverlay);
    page->addChild(*showCurve);

    // Each panel switches independently, so the overlay can be pared back to
    // what a given edit actually needs. All five have a button in the overlay's
    // upper toolbar row; they are here so the state is visible and saved with
    // the project.
    BooleanParamDescriptor* dragPreview = p_Desc.defineBooleanParam(kParamDragPreview);
    dragPreview->setLabels("Preview While Dragging", "Drag Preview", "Preview While Dragging");
    dragPreview->setHint("While a gizmo is being dragged, show the picture at the pose being "
                         "dragged instead of the frame under the playhead, tinted towards the "
                         "gizmo's colour. Turn it off for moves where it says little -- a "
                         "straight zoom, say, where the outline already tells you enough. Also "
                         "toggled by the GHOST button in the overlay.");
    dragPreview->setDefault(true);
    dragPreview->setParent(*gOverlay);
    page->addChild(*dragPreview);

    // Momentary drag state, not a setting: hidden, and never written to the
    // project so a ghost cannot reappear on reload or leak into a render.
    BooleanParamDescriptor* ghost = p_Desc.defineBooleanParam(kParamPreviewGhost);
    ghost->setDefault(false);
    ghost->setIsSecret(true);
    ghost->setIsPersistant(false);
    ghost->setParent(*gOverlay);
    InvalidatesCache(ghost);
    page->addChild(*ghost);

    BooleanParamDescriptor* showTimeline = p_Desc.defineBooleanParam(kParamShowTimeline);
    showTimeline->setLabels("Show Timeline", "Timeline", "Show Timeline");
    showTimeline->setHint("Draw the stage timing lanes along the bottom of the viewer. Also "
                          "toggled by the TIME button in the overlay.");
    showTimeline->setDefault(true);
    showTimeline->setParent(*gOverlay);
    page->addChild(*showTimeline);

    BooleanParamDescriptor* showPath = p_Desc.defineBooleanParam(kParamShowPath);
    showPath->setLabels("Show Motion Path", "Motion Path", "Show Motion Path");
    showPath->setHint("Draw the route the active stage travels, with handles to bend it. Also "
                      "toggled by the PATH button in the overlay.");
    showPath->setDefault(false);
    showPath->setParent(*gOverlay);
    page->addChild(*showPath);

    BooleanParamDescriptor* showOpacity = p_Desc.defineBooleanParam(kParamShowOpacity);
    showOpacity->setLabels("Show Opacity Slider", "Opacity Slider", "Show Opacity Slider");
    showOpacity->setHint("Draw the opacity slider down the left edge of the viewer. Also toggled "
                         "by the OPAC button in the overlay.");
    showOpacity->setDefault(false);
    showOpacity->setParent(*gOverlay);
    page->addChild(*showOpacity);

    BooleanParamDescriptor* showLib = p_Desc.defineBooleanParam(kParamShowLibrary);
    showLib->setLabels("Show Curve Library", "Curve Library", "Show Curve Library");
    showLib->setHint("Draw the saved-curve picker in the viewer overlay. Also toggled by the "
                     "LIBRARY button there. Clicking a curve applies it to the active stage.");
    showLib->setDefault(false);
    showLib->setParent(*gOverlay);
    page->addChild(*showLib);

    // Hidden and not persisted, unlike the panel toggles above. It has a button
    // of its own on the overlay and no reason to be reachable from here, and an
    // open panel is a momentary state rather than a setting worth saving.
    BooleanParamDescriptor* showQuick = p_Desc.defineBooleanParam(kParamShowQuick);
    showQuick->setDefault(false);
    showQuick->setIsSecret(true);
    showQuick->setIsPersistant(false);
    page->addChild(*showQuick);

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
