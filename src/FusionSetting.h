#pragma once

// Emitting a Fusion .setting from a baked transform.
//
// The format here is the one validated by loading into Fusion by hand: a
// Transform whose Center is an XYPath over two BezierSplines, and whose Size
// and Angle are BezierSplines, with every handle written as an absolute
// (frame, value) pair. Opacity, when it animates, becomes a Brightness/Contrast
// after the Transform -- the node the user asked for, gaining alpha down to
// fade. The move, the ease and the signs were all confirmed to match the
// plugin frame for frame; only the frame range differed, because a hand bake
// could not know the clip length. The plugin does, so BakeAndEmit takes it.
//
// Kept OFX-free and string-only, so the exact text can be diffed and tested
// against the known-good file without a host.
//
// ## Deliberately not emitted in v1
//
// Aspect (non-uniform scale) and any sheared or mirrored pose. A single-stage
// move -- the overwhelming common case -- is uniform scale plus rotation plus
// position, which Center/Size/Angle hold exactly. Non-uniform scale, an
// orthographic tilt/swivel composed under a rotation, or a swivel past 90
// degrees need Aspect and Flip, which are unproven in this format; the caller
// warns rather than emit something untested. See BakedTransform's flags.

#include <string>
#include <vector>

#include "FusionBake.h"
#include "ParamNames.h"   // blur param names, referenced by the linked expressions

namespace mtx {

namespace detail {

inline std::string FusNum(double v)
{
    char b[64];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

/** One scalar BezierSpline operator, the validated keyframe form. */
inline std::string FusSpline(const std::string& name, const std::vector<BezierKey>& k)
{
    std::string o = "\t\t" + name + " = BezierSpline {\n\t\t\tKeyFrames = {\n";
    for (size_t i = 0; i < k.size(); ++i)
    {
        const BezierKey& key = k[i];
        o += "\t\t\t\t[" + FusNum(key.frame) + "] = { " + FusNum(key.value);
        if (i + 1 < k.size())
            o += ", RH = { " + FusNum(key.frame + key.rhFrame) + ", "
                             + FusNum(key.value + key.rhValue) + " }";
        if (i > 0)
            o += ", LH = { " + FusNum(key.frame + key.lhFrame) + ", "
                             + FusNum(key.value + key.lhValue) + " }";
        o += " },\n";
    }
    o += "\t\t\t}\n\t\t}";
    return o;
}

} // namespace detail

/** @brief Motion-blur settings carried onto the baked Transform.
 *
 *  Only what a Fusion tool's built-in motion blur can express. The baked
 *  Transform animates along the same trajectory the plugin does, so Fusion's
 *  own blur -- driven by that animation's velocity -- lands close to the
 *  plugin's for the same shutter angle and sample count. The plugin's adaptive
 *  sampling has no Fusion equivalent (Fusion always renders Quality samples),
 *  and shutter *phase* maps only loosely onto Center Bias, so neither is
 *  carried; the caller can note it. */
struct FusionBlur
{
    bool   enabled      = false;
    double shutterAngle = 180.0;   ///< degrees, as the plugin authors it
    int    quality      = 16;      ///< sample count

    static FusionBlur Default() { return {}; }
};

/** @brief The .setting text for a baked transform, ready to paste into Fusion.
 *
 *  One Transform node, its input left open for the user's own footage, plus a
 *  Brightness/Contrast when opacity animates. Node names are prefixed with
 *  @p prefix so two bakes pasted into one comp do not collide.
 */
inline std::string EmitFusionSetting(const BakedTransform& b, const std::string& prefix = "MTX",
                                     const FusionBlur& blur = FusionBlur::Default())
{
    using detail::FusSpline;

    const std::string xf = prefix + "Transform";
    const std::string bc = prefix + "Fade";

    // Opacity earns a node only when it actually moves: a constant channel is
    // two equal keys, and a fade node that does nothing is clutter the user
    // would just delete.
    bool opacityAnimates = false;
    for (const BezierKey& k : b.opacity)
        if (std::fabs(k.value - b.opacity.front().value) > 1e-4f) { opacityAnimates = true; break; }

    std::string s = "{\n\tTools = ordered() {\n";

    // The Transform. Input unconnected -- wire the clip in.
    std::string blurInputs;
    if (blur.enabled)
    {
        // Fusion's tool motion blur samples the animated transform between
        // frames, and this Transform carries the same animation, so matching
        // the shutter angle and sample count matches the plugin's blur closely.
        blurInputs =
            "\t\t\t\tMotionBlur = Input { Value = 1 },\n"
            "\t\t\t\tQuality = Input { Value = " + detail::FusNum(blur.quality) + " },\n"
            "\t\t\t\tShutterAngle = Input { Value = " + detail::FusNum(blur.shutterAngle) + " },\n";
    }

    s += "\t\t" + xf + " = Transform {\n\t\t\tInputs = {\n"
         "\t\t\t\tCenter = Input { SourceOp = \"" + xf + "Center\", Source = \"Value\" },\n"
         "\t\t\t\tSize = Input { SourceOp = \"" + xf + "Size\", Source = \"Value\" },\n"
         "\t\t\t\tAngle = Input { SourceOp = \"" + xf + "Angle\", Source = \"Value\" },\n"
         + blurInputs +
         "\t\t\t},\n\t\t\tViewInfo = OperatorInfo { Pos = { 220, 0 } },\n\t\t},\n";

    s += "\t\t" + xf + "Center = XYPath {\n\t\t\tInputs = {\n"
         "\t\t\t\tX = Input { SourceOp = \"" + xf + "CenterX\", Source = \"Value\" },\n"
         "\t\t\t\tY = Input { SourceOp = \"" + xf + "CenterY\", Source = \"Value\" },\n"
         "\t\t\t},\n\t\t},\n";

    s += FusSpline(xf + "CenterX", b.centreX) + ",\n";
    s += FusSpline(xf + "CenterY", b.centreY) + ",\n";
    s += FusSpline(xf + "Size",    b.size)    + ",\n";
    s += FusSpline(xf + "Angle",   b.angle)   + ",\n";

    if (opacityAnimates)
    {
        // Fade after the Transform: Gain drives all channels, Alpha included,
        // which is the premultiplied fade the plugin does.
        s += "\t\t" + bc + " = BrightnessContrast {\n\t\t\tInputs = {\n"
             "\t\t\t\tGain = Input { SourceOp = \"" + bc + "Gain\", Source = \"Value\" },\n"
             "\t\t\t\tAlpha = Input { Value = 1 },\n"
             // Clip the black and white points: Gain can drive values below 0 or
             // above 1, and on premultiplied edges those out-of-range pixels
             // show up as fringing or sparkle. Clipping holds the result in
             // [0,1] so a fade stays clean.
             "\t\t\t\tClipBlack = Input { Value = 1 },\n"
             "\t\t\t\tClipWhite = Input { Value = 1 },\n"
             "\t\t\t\tInput = Input { SourceOp = \"" + xf + "\", Source = \"Output\" },\n"
             "\t\t\t},\n\t\t\tViewInfo = OperatorInfo { Pos = { 440, 0 } },\n\t\t},\n";
        s += FusSpline(bc + "Gain", b.opacity) + ",\n";
    }

    s += "\t}\n}\n";
    return s;
}

/** @brief A Transform (and fade) pre-linked to a Multi Transform's output
 *  parameters by expression, ready to paste next to the plugin's own node.
 *
 *  The complement of EmitFusionSetting: no keyframes at all -- every input is
 *  an expression reading the plugin's Output Link read-outs, so the pasted
 *  node follows the plugin live and stays current through any edit. The
 *  plugin cannot know its own tool name in the comp, so @p sourceTool is the
 *  caller's best guess (the host names the first instance MultiTransform1);
 *  a wrong guess leaves the expressions unresolved until the user corrects
 *  either the node name or the expressions.
 */
inline std::string EmitFusionLinked(const std::string& sourceTool,
                                    const std::string& prefix = "MTXL")
{
    const std::string xf = prefix + "Transform";
    const std::string bc = prefix + "Fade";

    // The blur settings link live too, and more directly than the transform
    // channels: MotionBlur, Quality and ShutterAngle read the plugin's own
    // stored parameters -- which expressions can always see -- so they follow
    // a toggle or a slider instantly, with no Output Link publishing involved.
    // Adaptive sampling and shutter phase stay unlinked as ever: Fusion's tool
    // blur has no input for either.
    const std::string blurInputs =
        "\t\t\t\tMotionBlur = Input { Expression = \"" + sourceTool + "."
                + kParamBlurEnabled + "\" },\n"
        "\t\t\t\tQuality = Input { Expression = \"" + sourceTool + "."
                + kParamBlurSamples + "\" },\n"
        "\t\t\t\tShutterAngle = Input { Expression = \"" + sourceTool + "."
                + kParamShutterAngle + "\" },\n";

    std::string s = "{\n\tTools = ordered() {\n";

    s += "\t\t" + xf + " = Transform {\n\t\t\tInputs = {\n"
         "\t\t\t\tCenter = Input { Expression = \"Point(" + sourceTool + ".outCenterX, "
                 + sourceTool + ".outCenterY)\" },\n"
         "\t\t\t\tSize = Input { Expression = \"" + sourceTool + ".outSize\" },\n"
         "\t\t\t\tAngle = Input { Expression = \"" + sourceTool + ".outAngle\" },\n"
         + blurInputs +
         "\t\t\t},\n\t\t\tViewInfo = OperatorInfo { Pos = { 220, 0 } },\n\t\t},\n";

    // The fade is always included -- whether opacity animates is live in the
    // link, not knowable at emit time. Clipped as in the baked emitter.
    s += "\t\t" + bc + " = BrightnessContrast {\n\t\t\tInputs = {\n"
         "\t\t\t\tGain = Input { Expression = \"" + sourceTool + ".outOpacity\" },\n"
         "\t\t\t\tAlpha = Input { Value = 1 },\n"
         "\t\t\t\tClipBlack = Input { Value = 1 },\n"
         "\t\t\t\tClipWhite = Input { Value = 1 },\n"
         "\t\t\t\tInput = Input { SourceOp = \"" + xf + "\", Source = \"Output\" },\n"
         "\t\t\t},\n\t\t\tViewInfo = OperatorInfo { Pos = { 440, 0 } },\n\t\t},\n";

    s += "\t}\n}\n";
    return s;
}

} // namespace mtx
