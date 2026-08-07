#pragma once

// A saved easing curve.
//
// Deliberately its own file type rather than a PresetData with everything but
// the easing left blank. A curve is reusable in a way a whole-effect preset is
// not: it says nothing about what is being animated, so the same "heavy
// settle" applies equally to a push-in, a fade and a card flip. Giving it its
// own type means a curve file is nine numbers a person can read, and it means
// the library can tell at a glance what a file is for.
//
// Files live in `<presets>\Curves`, and carry `"type": "curve"` as well. The
// folder is how the library finds them; the type field is how it knows a file
// dropped in the wrong place is still a curve, and -- more importantly -- how
// it refuses to apply an effect preset as if it were one.

#include <string>

#include "AnimEngine.h"

namespace mtx {

/** Marks a file as a curve. Written into every curve file and checked on load. */
constexpr const char* kCurveTypeTag = "curve";

struct CurvePreset
{
    std::string name;

    // Stored as the human-facing amounts rather than the derived bezier
    // handles, exactly as effect presets do: a file stays readable, and it
    // survives any future change to how amounts map onto handles.
    float easeIn, easeOut, anticipation, overshoot;
    int   bounceType;
    float bounceAmount, bounceCount, bounceDamping, bounceStart;

    static CurvePreset Default();

    /** @brief The curve as the engine evaluates it, for drawing a preview. */
    Easing ToEasing() const;
};

/** @brief Serialise to JSON. Round-trips exactly through FromJson. */
std::string CurveToJson(const CurvePreset& c);

/** @brief Parse a curve file.
 *
 * Strict about the type tag: a file that is not marked as a curve is refused
 * rather than silently read as one, so pointing the library at a folder of
 * effect presets produces an explanation instead of nine defaulted numbers
 * masquerading as somebody's saved curve.
 *
 * Missing keys inside a genuine curve file resolve to defaults, and unknown
 * keys are ignored, so a file written by a newer build still loads.
 */
bool CurveFromJson(const std::string& jsonText, CurvePreset& out, std::string& error);

/** @brief Strip characters a file name cannot contain.
 *
 * A curve is named in the Inspector and that name becomes the file name, so
 * without this a perfectly reasonable name like "in/out" fails with an error
 * about a path rather than about the name. Returns an empty string if nothing
 * usable is left, which the caller reports as "type a name first".
 *
 * Lives here rather than with the other file-system code so it can be tested:
 * it is pure string handling, and PresetIO.cpp is Win32-only.
 */
std::string SanitiseFileName(const std::string& name);

} // namespace mtx
