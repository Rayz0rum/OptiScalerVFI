#pragma once

#include "DlssNr_Switch.h"

#include <cstdint>

// Where the Neural Rendering pass sits relative to the upscaler.
//
// Kept in its own small header so Config.h can name these without pulling in D3D12, the same reason
// DlssNr_Switch.h exists.
//
// Only PostProcess is unconditional. The other three change how OptiScaler's own upscaler feature is
// created -- its target resolution, its HDR and exposure flags, or both -- so they apply to
// OptiScaler's upscalers and not to a game's native DLSS passing straight through. A native
// passthrough frame has already been enlarged by a feature nobody here created, and there is no
// honest way to run the model before that has happened.

namespace DlssNr
{

enum class Mode : uint32_t
{
    // The model runs on the finished frame, at display resolution. The arrangement the module was
    // built around, and the only one that needs nothing from the upscaler.
    PostProcess = 0,

    // The model runs at render resolution and Super Resolution enlarges its output.
    //
    // The upscaler is then working on a tone-mapped, display-referred picture rather than the linear
    // HDR one it expects, so its feature is created with IsHDR and AutoExposure cleared. Both are
    // latched at creation, which is why changing to or from this mode rebuilds the feature.
    UpscaleWithSR = 1,

    // Two features. The first runs 1:1 at render resolution -- denoising, if it is Ray Reconstruction;
    // antialiasing as DLAA, if it is Super Resolution -- then the model runs on that, and a second
    // Super Resolution feature performs the single enlargement to display resolution.
    //
    // The point is to give the model a temporally stable frame that has not yet been magnified.
    MultiPass = 2,

    // Multi-pass with the first feature's resolution lowered below the game's render resolution, to
    // buy back what the second feature costs. The result is brought back up before the model sees it,
    // so this trades the first pass's quality for its cost and changes nothing downstream.
    MultiPassCustom = 3,
};

// Which upscaler the first pass is. OptiScaler does not substitute one for the other -- Ray
// Reconstruction needs G-buffer inputs a Super Resolution integration never supplies -- so this
// states which the game is set up for, and a mismatch falls back rather than half-applying.
enum class Feature1Pipeline : uint32_t
{
    RayReconstruction = 0,
    SuperResolution = 1,
};

// The mode as configured, before any fallback. Reads config; safe from any thread.
Mode ConfiguredMode();

// Whether a mode needs the model to run before the enlargement rather than after.
inline bool RunsBeforeUpscale(Mode mode) { return mode != Mode::PostProcess; }

// Whether a mode needs a second Super Resolution feature to perform the enlargement.
inline bool UsesTwoFeatures(Mode mode) { return mode == Mode::MultiPass || mode == Mode::MultiPassCustom; }

// Whether the upscaler's own feature must be created with IsHDR and AutoExposure cleared. Only the
// single-feature reordering does: in multi-pass the first feature still sees the game's own linear
// frame and keeps its flags, and it is the second feature -- created separately -- that clears them.
inline bool WantsReorderedFlags(Mode mode) { return mode == Mode::UpscaleWithSR; }

} // namespace DlssNr
