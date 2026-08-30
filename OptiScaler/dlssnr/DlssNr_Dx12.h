#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>

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
void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          ID3D12Resource* targetOverride = nullptr, unsigned int overrideWidth = 0,
                          unsigned int overrideHeight = 0);


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
                              unsigned int width, unsigned int height);
bool IsRunning();

// Why it is not, if it is not. Empty while it is running or has not been tried yet.
const char* FailureReason();

// The white point the exposure meter has settled on, or 0 if it has not taken a reading yet. For the
// overlay, so the number in use is visible rather than inferred.

// What the pass last cost on the GPU, in milliseconds, or nothing if it has not been measured yet.
std::optional<double> LastGpuTime();

// Writes a run of consecutive frames, each as the upscaler produced it and again after the model's edit.
// The pair is a control: same frames, same run, one variable.
void RequestCapture(unsigned int frames);
bool CaptureInProgress();

void Shutdown();
} // namespace DlssNr
