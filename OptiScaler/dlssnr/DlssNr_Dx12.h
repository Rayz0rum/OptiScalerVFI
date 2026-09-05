#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>

#include "DlssNr_Diag.h"
#include "DlssNr_Jitter.h"
#include "DlssNr_Report.h"

// DLSS 5 Neural Rendering, run over the upscaler's output.
//
// Neural Rendering is a post-process, not an upscaler and not a denoiser: it takes a finished frame plus
// depth and motion vectors and synthesises detail. NVIDIA ships no public integration for it, so it is
// driven directly through nvngx_dlssnr.dll as feature 18.
//
// OptiScaler is the right host for it because of one thing it knows that an external hook cannot: which
// NGX evaluate belongs to the upscaler and which to frame generation. Both are handed depth and motion
// vectors, so anything guessing from the parameter block alone attaches to both and runs the model twice
// per rendered frame. Here it is a lookup on the feature handle.
class Config;

namespace DlssNr
{
// Runs the model over the frame on the same command list, immediately after the upscaler has written
// it. It is shown a display-referred proxy of that frame -- the sort of picture it was trained on --
// and its answer is composed back over the untouched original. Called only for upscaler evaluates,
// never for frame generation, which is the whole point.
//
// `targetOverride` names the image to work on instead of the parameter block's Output. The reordered
// and multi-pass arrangements need it: in those the model runs at render resolution, on the upscaler's
// input or on a first pass's 1:1 result, neither of which is what Output points at.
//
// `overrideWidth` and `overrideHeight` name the region of that image which actually holds the frame,
// and are not optional when the target is. A game commonly allocates its colour buffer at display size
// and renders into the top-left corner, so the resource extent is the wrong answer for the working
// size -- everything past the render rect is memory nobody wrote, and encoding it hands the model
// garbage that comes back as flickering coloured blocks.
//
// Safe to call every frame; it builds what it needs on first use and disables itself for the session if
// anything fails, rather than retrying into a crash.
// Returns the image the caller should treat as the frame from here on. Normally that is the target
// itself, edited in place. When `writeToScratch` is set the pass writes into a buffer of its own and
// returns that instead -- which the reordered arrangement needs, because there the target is the
// GAME's colour buffer, and a game does not generally create that with unordered access. The codec's
// view over it then cannot be created at all and the writes land nowhere defined, which is what puts
// coloured blocks over the frame.
ID3D12Resource* EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                                     ID3D12Resource* targetOverride = nullptr,
                                     unsigned int overrideWidth = 0, unsigned int overrideHeight = 0,
                                     bool writeToScratch = false);


// Frame generation titles tag their UI layer through Streamline; a copy of it makes the HUD mask
// exact at the finished frame. Called at tag time.




// The settings panel, drawn inside OptiScaler's menu.
void RenderMenu(::Config* config, float menuResScale);

// Clears the session failure latch, so a failure caused by transient thrash does not cost a restart.
void RetryAfterFailure();


// Whether the model is loaded and running, for the overlay.

/*
 * Carries the model's edit from a resolved image onto the game's jittered one.
 *
 * The multi-pass chain's way of ending in a real Super Resolution pass. The first pass resolves the
 * game's jitter so the model sees a clean frame, and that resolve is exactly what would leave the
 * enlargement with no subpixel content to reconstruct from. Measuring the edit here and applying it to
 * the still-jittered frame gives the enlargement both: the model's enhancement, and the game's own
 * subpixel sampling with the real jitter offsets to interpret it by.
 *
 * `before` is what the model was shown, `after` what it returned, `jittered` the game's own frame, and
 * `target` receives the jittered frame carrying the edit. All four are at render resolution.
 */
void TransferEditOntoJittered(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* before,
                              ID3D12Resource* after, ID3D12Resource* jittered, ID3D12Resource* target,
                              unsigned int width, unsigned int height, float alignX, float alignY);

/*
 * Resample the first pass's inputs down to the size it is being run at, for Multi-pass Custom.
 *
 * Without it the game's buffers are handed over unchanged while the feature is told they are smaller,
 * so DLSS reads the top-left corner of each -- a crop, not a reduction -- and the frame becomes a
 * magnified corner of itself. Rewrites Color, Depth, MotionVectors, the motion vector scales and the
 * render subrect in the parameter block. Call it before the first pass evaluates.
 */
bool ResampleFeature1Inputs(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                            unsigned int srcWidth, unsigned int srcHeight, unsigned int dstWidth,
                            unsigned int dstHeight);
bool IsRunning();

// Why it is not, if it is not. Empty while it is running or has not been tried yet.
const char* FailureReason();

// The white point the exposure meter has settled on, or 0 if it has not taken a reading yet. For the
// overlay, so the number in use is visible rather than inferred.

// What the pass last cost on the GPU, in milliseconds, or nothing if it has not been measured yet.
std::optional<double> LastGpuTime();

// And the same per stage, so the total can be attributed rather than guessed at. Nothing when the
// stage did not run this frame, which the overlay has to distinguish from a stage that cost nothing.
std::optional<double> StageTime(diag::Stage stage);

// The stages that ran, added up. Not the frame's Neural Rendering cost -- work overlaps on the GPU --
// but the right denominator for asking what share of it a given stage is.
double StageTotal();

// Records a stage from outside this module -- the enlargement is a whole second feature living in its
// own translation unit, and it is exactly the stage the multi-pass cost question is about.
void BeginStage(diag::Stage stage, ID3D12GraphicsCommandList* cmdList);
void EndStage(diag::Stage stage, ID3D12GraphicsCommandList* cmdList);

/*
 * Which pass's jitter is being reported.
 *
 * Only two sites matter. The first pass takes the game's sequence, possibly rescaled for a reduced
 * render target; the final enlargement takes either the game's offsets or zeros. Everything the
 * acceptance criteria ask about jitter is a question about one of those two.
 */
enum class JitterSite : unsigned int
{
    Feature1 = 0,
    Final = 1,
    Count
};

/*
 * True when the game named a preset for that quality mode, with the value in outPreset.
 *
 * A bool and an out-parameter rather than a sentinel, because there is no integer left over to mean
 * "none". Presets arrive as unsigned values and real ones have been seen with the high bit set -- a
 * log from Mortal Shell 2 shows 0x8000000B -- so returning them as a signed int made a perfectly
 * valid preset test as negative, and a "did the game set one" check written as >= 0 then silently
 * answered no. The enlargement was left on the driver's own choice for exactly the titles the
 * matching was meant to help.
 */
bool PresetForQuality(NVSDK_NGX_Parameter* params, int perfQuality, unsigned int& outPreset);

// The parameter name a preset for that quality mode lives under, or nullptr for a mode that has none.
// Presets are per performance mode, so writing one means picking the right key first.
const char* PresetKeyForQuality(int perfQuality);

/*
 * How much the model's work will be magnified after the pass runs, stated by the pipeline each frame.
 *
 * The model synthesises detail at whatever resolution it ran at, and anything that enlarges its
 * output afterwards spreads that detail over more pixels and attenuates it -- which is why the pass
 * reads progressively weaker the more upscaling sits behind it, at identical settings. The module can
 * see its own working scale but not what the caller does next, so the caller says. 1.0 means nothing
 * follows, which is the post-process case and the default.
 */
void SetEnlargementRatio(float ratio);

// Records an offset a pass was actually given. Cheap enough for the per-frame path.
void ObserveJitter(JitterSite site, float x, float y);

// Distinct offsets seen, whether the sequence has been observed to repeat (so a low count can be
// trusted as final rather than merely early), and how many fell outside the guide's +/-0.5.
void JitterStats(JitterSite site, unsigned int& distinct, bool& settled, unsigned int& outOfBounds);

/*
 * Where the edit transfer must sample the resolved pair so its ratio lands on the content the
 * jittered frame holds at this pixel.
 *
 * Replaces the three-way sign guess with a derivation. The programming guide states jitter offsets
 * use the same coordinate and direction system as motion vectors, with (0,0) meaning no jitter, so
 * the direction follows from the motion vector convention the game already declares rather than from
 * trying values. The manual override remains for an engine that disagrees with the guide.
 */
void DerivedAlign(float jitterX, float jitterY, float mvScaleX, float mvScaleY, float& outX, float& outY);

// Emits the structured integration line, if anything about the arrangement has changed since the last
// one. Also raises the phase-count warning, which is the one part of the report that is a verdict.
void LogIntegration(const report::Integration& in);

// Writes a run of consecutive frames, each as the upscaler produced it and again after the model's edit.
// The pair is a control: same frames, same run, one variable.
void RequestCapture(unsigned int frames);
bool CaptureInProgress();

void Shutdown();
} // namespace DlssNr
