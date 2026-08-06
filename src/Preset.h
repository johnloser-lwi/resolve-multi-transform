#pragma once

// Preset serialisation.
//
// Deliberately free of any OFX dependency, like ClipRange.h, so the schema and
// the timing rules can be unit tested without a host. A preset that silently
// mis-times an animation is far worse than one that refuses to load, so the
// parser is strict and the rescaling rules are pinned by tests.

#include <string>

#include "AnimEngine.h"

namespace mtx {

/** Bumped only when the schema changes incompatibly. Unknown keys are ignored
 *  and missing ones fall back to defaults, so most additions need no bump. */
constexpr int kPresetSchemaVersion = 1;

/** @brief One stage's worth of a preset: everything except derived values. */
struct PresetStage
{
    bool  enabled;
    int   anchor;            ///< TimingAnchor
    float startFrame, endFrame;

    float scaleFrom, scaleTo;
    float posXFrom, posYFrom, posXTo, posYTo;
    float rotFrom, rotTo;
    float opacityFrom, opacityTo;
    float anchorX, anchorY;
    float pathC1X, pathC1Y, pathC2X, pathC2Y;

    int   easingPreset;
    float easeIn, easeOut, anticipation, overshoot;
    int   bounceType;
    float bounceAmount, bounceCount, bounceDamping, bounceStart;

    static PresetStage Default();
};

/** @brief A saved setup: the whole effect, or a single stage. */
struct PresetData
{
    int         schemaVersion;
    std::string name;

    /// false means the file holds one stage, to be loaded into the active one.
    bool  wholeEffect;

    /// Length of the clip this was authored on, in frames, or 0 if the host did
    /// not report it. Only needed by RescaleTiming.
    float sourceClipLength;

    // Globals -- meaningful only when wholeEffect is true.
    int   stageCount;
    int   filterMode;
    int   edgeMode;
    bool  blurEnabled;
    float shutterAngle, shutterPhase;
    int   blurSamples;
    bool  blurAdaptive;

    /// For a single-stage preset only stages[0] is populated.
    PresetStage stages[kMaxStages];

    static PresetData Default();
};

/** @brief Serialise to JSON. Round-trips exactly through FromJson. */
std::string ToJson(const PresetData& data);

/** @brief Parse JSON produced by ToJson.
 *
 * Strict: on any parse failure this returns false with a message and leaves
 * @p out untouched, rather than applying a partially-read preset.
 *
 * Missing keys resolve to defaults rather than to whatever the caller already
 * had, so loading the same preset twice from different starting states gives
 * the same result. Unknown keys are ignored, so a file written by a newer build
 * still loads.
 */
bool FromJson(const std::string& json, PresetData& out, std::string& error);

/** @brief Scale frame-based stage timings to a different clip length.
 *
 * Applies only to anchors measured in frames. A Stretch stage is stored as
 * percentages and is therefore already proportional -- rescaling it would apply
 * the length ratio a second time and break precisely the case it exists for.
 *
 * A no-op when either length is unknown or non-positive: scaling by a guessed
 * factor would quietly mis-time everything, and leaving the values alone is the
 * honest failure. Durations are clamped to at least one frame so that a short
 * punch on a long clip does not collapse into a zero-length instant cut when
 * moved to a short one.
 */
void RescaleTiming(PresetData& data, float targetClipLength);

} // namespace mtx
