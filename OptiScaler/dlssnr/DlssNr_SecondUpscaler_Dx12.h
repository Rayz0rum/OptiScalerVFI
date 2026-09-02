#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>

#include <gpu_time/GpuTime_Dx12.h>

#include <memory>
#include <string>

/*
 * A second DLSS Super Resolution feature, owned by OptiScaler.
 *
 * Why this exists
 * ---------------
 * Ray Reconstruction refuses to be created without IsHDR - it fails outright
 * with "HDR Color required" from NgxSwinDenoiser::CreateDldnInstance. Neural
 * Rendering needs a display-referred image, and an upscaler consuming NR's
 * output therefore needs IsHDR cleared. Within a single feature those two
 * requirements are directly contradictory and no parameter reconciles them.
 *
 * The incompatibility is per-feature, not absolute. Nothing prevents holding
 * two handles: Ray Reconstruction keeps IsHDR and denoises at 1:1, and this
 * feature has it cleared and performs the only enlargement in the frame.
 *
 *   path trace (R, noisy HDR)
 *     -> RR       [feature 1, in = out = R, IsHDR on]     denoise only
 *     -> tone map (R)
 *     -> NR       (R)
 *     -> SR       [feature 2, R->D, IsHDR off, identity exposure]
 *     -> present
 *
 * Feature 1 must not upscale. If both enlarge R->D the chain becomes upscale,
 * downscale, upscale: Ray Reconstruction's reconstruction is thrown away by
 * the downscale that feeds NR, and the final detail is bounded by R anyway.
 *
 * What it costs
 * -------------
 * A second set of temporal history and the VRAM for it. And this feature
 * upscales a tone mapped image rather than linear HDR, which is a real quality
 * trade on the upscaler's side - less headroom above white, more risk of
 * banding in bright regions, and reduced ability to reconstruct highlight
 * detail that tone mapping has already compressed. Separating the features
 * fixes the incompatibility, not the colour-space compromise.
 */
class DlssNr_SecondUpscaler_Dx12
{
  public:
    DlssNr_SecondUpscaler_Dx12(std::string name, ID3D12Device* device);
    ~DlssNr_SecondUpscaler_Dx12();

    /*
     * Create the feature, or rebuild it when the geometry changed.
     *
     * Feature flags are fixed at creation, so a change to any of them has to
     * come through here rather than being set per frame.
     */
    /*
     * `preset` is the render preset to put this feature on, or -1 to leave it unset.
     *
     * Unset is not the same as "the driver's default": it means the driver picks one from the ratio,
     * independently of whatever the first pass is on, and the two can disagree about the frame. The
     * programming guide attaches real behaviour to that -- exposure input is only supported by Presets
     * J and K, and Preset L always uses AutoExposure -- and this feature binds an identity exposure
     * texture with AutoExposure cleared, so a driver choosing L for it would ignore that texture and
     * auto-expose a picture that has already been normalised.
     */
    bool EnsureCreated(ID3D12GraphicsCommandList* cmdList, uint32_t renderWidth, uint32_t renderHeight,
                       uint32_t displayWidth, uint32_t displayHeight, int perfQuality, bool depthInverted,
                       bool jitteredMV, bool lowResMV, int preset, bool isHdr, bool autoExposure);

    /*
     * Run the upscale. `exposure` is the 1x1 identity texture: this feature is
     * created with AutoExposure cleared, so it has no exposure source of its
     * own and would otherwise resolve toward black.
     */
    bool Evaluate(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* color, ID3D12Resource* output,
                  ID3D12Resource* depth, ID3D12Resource* mvec, ID3D12Resource* exposure, float jitterX, float jitterY,
                  float mvScaleX, float mvScaleY, bool reset);

    /*
     * The render-resolution image this feature reads.
     *
     * The pass ahead of it (Neural Rendering) writes here, and this feature
     * enlarges it into the frame's real output. Allocated against that output
     * so format and heap match.
     */
    bool CreateInputBuffer(ID3D12Resource* reference, uint32_t renderWidth, uint32_t renderHeight);

    /*
     * The pair the edit transfer needs: a copy of what the model was shown, and somewhere to put the
     * jittered frame once the edit has been carried onto it. Allocated only when that path is used.
     */
    bool CreateEditBuffers(ID3D12Resource* reference, uint32_t renderWidth, uint32_t renderHeight);

    ID3D12Resource* EditBefore() { return _editBefore; }
    ID3D12Resource* EditResult() { return _editResult; }

    ID3D12Resource* InputBuffer() { return _inputBuffer; }

    void SetInputBufferState(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES state);

    void Release();

  private:
    /* The 1x1 R32_FLOAT texture holding exactly 1.0, uploaded on first use. */
    ID3D12Resource* IdentityExposure(ID3D12GraphicsCommandList* cmdList);

  public:

    bool IsCreated() const { return _handle != nullptr; }

    /*
     * True once for each newly created feature, so its first evaluate carries Reset.
     *
     * A feature built this frame has no history, and its first frame would otherwise be blended
     * against whatever the allocation happened to contain.
     */
    bool ConsumeResetFlag()
    {
        const bool reset = _needsReset;
        _needsReset = false;
        return reset;
    }

    std::optional<double> ReadGpuTime(ID3D12CommandQueue* queue)
    {
        return GpuTime != nullptr ? GpuTime->ReadGpuTime(queue) : std::nullopt;
    }

  private:
    std::string _name;
    ID3D12Device* _device = nullptr;

    NVSDK_NGX_Handle* _handle = nullptr;
    NVSDK_NGX_Parameter* _params = nullptr;

    uint32_t _renderWidth = 0;
    uint32_t _renderHeight = 0;
    uint32_t _displayWidth = 0;
    uint32_t _displayHeight = 0;
    int _perfQuality = -1;
    bool _depthInverted = false;
    bool _jitteredMV = false;
    bool _lowResMV = false;

    // What this feature was created to expect, so Evaluate knows whether an exposure texture is
    // needed at all: with AutoExposure set, DLSS derives one and a supplied texture is redundant.
    bool _isHdr = false;
    bool _autoExposure = false;

    bool _createFailed = false;
    bool _needsReset = true;

    /*
     * The 1x1 identity exposure this feature needs, and its upload staging.
     *
     * It is created with AutoExposure cleared, so it has no exposure source of
     * its own: keeping AutoExposure would have DLSS estimate one and divide an
     * already-normalised picture toward black, and clearing it while supplying
     * nothing is black too. Uploaded once, then reused.
     */
    ID3D12Resource* _exposure = nullptr;
    ID3D12Resource* _exposureUpload = nullptr;
    bool _exposureUploaded = false;

    ID3D12Resource* _inputBuffer = nullptr;
    ID3D12Resource* _editBefore = nullptr;
    ID3D12Resource* _editResult = nullptr;
    D3D12_RESOURCE_STATES _inputBufferState = D3D12_RESOURCE_STATE_COMMON;

    // Its own timer. An existing row named after the first feature would not
    // cover this one, and its cost would silently go missing from the capture.
    std::unique_ptr<GpuTime_Dx12> GpuTime;
};
