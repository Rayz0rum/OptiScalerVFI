#pragma once
#include "SysUtils.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>

#include <unordered_set>
#include <Util.h>
#include <dlssnr/DlssNr_Modes.h>

#define DLSS_MOD_ID_OFFSET 1000000

inline static unsigned int handleCounter = DLSS_MOD_ID_OFFSET;

struct InitFlags
{
    bool IsHdr;
    bool SharpenEnabled;
    bool LowResMV;
    bool AutoExposure;
    bool DepthInverted;
    bool JitteredMV;
};

static auto sumOpts(const auto&... opts) -> std::optional<double>
{
    if ((opts.has_value() || ... || false))
    {
        return (opts.value_or(0.0) + ... + 0.0);
    }

    return std::nullopt;
}

struct DetailedGpuTime
{
    std::string name;
    double time = 0.0;
    bool includedInUpscalerTime = false;
};

class IFeature
{
  private:
    bool _isInited = false;
    int _featureFlags = 0;
    InitFlags _initFlags = {};

    NVSDK_NGX_PerfQuality_Value _perfQualityValue;

    struct JitterInfo
    {
        float x;
        float y;
    };

    struct hashFunction
    {
        size_t operator()(const std::pair<float, float>& p) const
        {
            size_t h1 = std::hash<float>()(p.first);
            size_t h2 = std::hash<float>()(p.second);
            return h1 ^ (h2 << 1);
        }
    };

    std::unordered_set<std::pair<float, float>, hashFunction> _jitterInfo;

  protected:
    // D3D11with12
    inline static ID3D12Device* _dx11on12Device = nullptr;
    inline static ID3D12Device* _localDx11on12Device = nullptr;

    bool _initParameters = false;
    NVSDK_NGX_Handle* _handle = nullptr;

    float _sharpness = 0; // Used by the feature itself, might get spoofed to 0 when RCAS is used
    std::optional<float> _actualSharpness = std::nullopt;
    bool _hasColor = false;
    bool _hasDepth = false;
    bool _hasMV = false;
    bool _hasTM = false;
    bool _accessToReactiveMask = false;
    bool _hasExposure = false;
    bool _hasOutput = false;
    bool _depthLinear = false;

    unsigned int _renderWidth = 0;
    unsigned int _renderHeight = 0;
    unsigned int _targetWidth = 0;
    unsigned int _targetHeight = 0;
    unsigned int _displayWidth = 0;
    unsigned int _displayHeight = 0;

#if OPTI_DLSSNR
    /*
     * The game's own render resolution, kept because Multi-pass Custom moves
     * _renderWidth/_renderHeight down to the first pass's size.
     *
     * Everything after that pass still works at the game's resolution: the depth
     * and motion vectors it supplies are at this size, the model runs here, and
     * the second Super Resolution feature enlarges from here to display. Zero
     * until SetInitParameters has run.
     */
    unsigned int _nrSourceWidth = 0;
    unsigned int _nrSourceHeight = 0;

    /*
     * Which arrangement this feature was actually created for.
     *
     * IsHDR and AutoExposure are latched at creation. Engines commonly decide
     * whether to rebuild by comparing resolutions alone, so switching modes
     * without checking this would leave a feature built for the wrong colour
     * space -- washed-out colour with an unstable exposure, and no error
     * anywhere to point at it.
     */
    bool _nrReorderedAtCreate = false;

    /*
     * The arrangement this feature was actually built for.
     *
     * Not the same question as _nrReorderedAtCreate, which only asks about the
     * flags. Multi-pass also fixes the target resolution at creation, and the
     * pipeline stage that feeds it reads config live -- so switching placement
     * in the menu starts routing the upscaler into a render-resolution buffer
     * while its feature is still built to write display resolution, and the
     * evaluate fails with FAIL_InvalidParameter every frame.
     */
    DlssNr::Mode _nrModeAtCreate = DlssNr::Mode::PostProcess;

    /*
     * Whether the GAME's own image is linear HDR, recorded before any override.
     *
     * IsHdr() reports the flag this feature was created with, and the reordered
     * modes deliberately clear it to describe the tone-mapped image the upscaler
     * will be handed. That is the wrong question for the colour codec, which
     * needs to know what the frame actually is when the model sees it.
     */
    bool _nrGameIsHdr = false;
#endif

    long _frameCount = 0;
    bool _featureFrozen = false;
    bool _moduleLoaded = false;

    std::optional<double> lastUpscalerTime {};
    std::optional<double> lastRcasTime {};
    std::optional<double> lastOutputScalingTime {};

    void SetHandle(unsigned int InHandleId);
    bool SetInitParameters(NVSDK_NGX_Parameter* InParameters);
    void GetRenderResolution(const NVSDK_NGX_Parameter* InParameters, unsigned int* OutWidth, unsigned int* OutHeight);
    void GetDynamicOutputResolution(NVSDK_NGX_Parameter* InParameters, unsigned int* width, unsigned int* height);
    float GetSharpness(const NVSDK_NGX_Parameter* InParameters);

    virtual void SetInit(bool InValue) { _isInited = InValue; }

  public:

#if OPTI_DLSSNR
    /*
     * The Neural Rendering arrangement actually in force for this feature.
     *
     * Multi-pass is defined as a 1:1 first pass with a second feature doing the
     * enlargement, and the selected pipeline has to match the upscaler that is
     * really running: OptiScaler does not substitute Ray Reconstruction for
     * Super Resolution or the reverse, since RR needs G-buffer inputs an SR
     * integration never supplies. A mismatch degrades to the conventional
     * ordering rather than half-applying the arrangement -- which would clear
     * IsHDR and AutoExposure on one feature while never creating the other,
     * giving a black frame with nothing in the log to explain it.
     */
    DlssNr::Mode NREffectiveMode() const;

    bool NRUsesTwoFeatures() const { return DlssNr::UsesTwoFeatures(NREffectiveMode()); }

    /*
     * Whether *this* feature has to be created with IsHDR and AutoExposure
     * cleared. Only the single-feature reordering does; in multi-pass the first
     * feature still sees the game's own linear frame, and the second one is
     * created elsewhere with its own flags.
     */
    bool NRWantsReorderedFlags() const { return DlssNr::WantsReorderedFlags(NREffectiveMode()); }

    /*
     * True when the arrangement changed since this feature was built.
     *
     * Both the flags and the target resolution are fixed at creation, so a
     * placement change cannot take effect without a rebuild. Nothing else
     * catches it: engines decide whether to rebuild by comparing resolutions,
     * and the game's resolutions have not moved.
     *
     * Left unchecked, the pipeline stage -- which reads config live -- starts
     * routing the upscaler's output into a render-resolution buffer while its
     * feature is still built to write display resolution, and every evaluate
     * fails with FAIL_InvalidParameter.
     *
     * This must ask the same question SetInitParameters asked, or it answers
     * "yes" forever and the backend rebuilds every frame.
     */
    bool NRNeedsRebuild() const { return _nrModeAtCreate != NREffectiveMode(); }

    /*
     * The arrangement the live feature was built for.
     *
     * What the pipeline must branch on, rather than the configured mode: the two agree only after a
     * rebuild, and a stage that disagrees with its feature is exactly the failure above.
     */
    DlssNr::Mode NRBuiltMode() const { return _nrModeAtCreate; }

    /* The 1:1 size the first pass runs at, and whether that differs from the
     * game's render resolution. See the implementation for the clamping rules. */
    void NRFeature1Size(unsigned int& outWidth, unsigned int& outHeight) const;
    bool NRFeature1IsResampled() const;

    /*
     * Re-apply the multi-pass 1:1 hold to the target resolution.
     *
     * SetInitParameters establishes it, but every upscaler's ProcessInitParams
     * recomputes the target afterwards -- to the display size, or to that times
     * the Output Scaling ratio -- and either branch silently undoes the hold.
     * The feature then enlarges when it was supposed to run 1:1, and the second
     * feature downstream, created expecting render resolution, receives a
     * display-sized image and magnifies its corner by the upscale ratio.
     *
     * Returns true when it took ownership of the target, in which case the
     * caller must not apply the Output Scaling multiplier either: this
     * arrangement already contains exactly one enlargement, and a second would
     * produce the upscale-downscale-upscale chain it exists to avoid.
     */
    /*
     * Settle which arrangement this feature is being built for, and apply what that decides -- the
     * flag overrides and the 1:1 hold.
     *
     * Must be called from ProcessInitParams, never from a constructor: it asks Api() and
     * GetUpscalerType(), which are still pure virtual while the base classes are being built.
     */
    void NRPrepareForCreate();

    /*
     * Whether the final Super Resolution pass gets the game's real jitter offsets or zeros. Shared by
     * both multi-pass pipelines; see the implementation for why MVJittered overrides the setting.
     */
    bool NRFinalPassForwardsJitter() const;

    bool NRApplyFeature1Hold();

    /* The resolution everything after the first pass works at -- the game's own
     * render resolution. Identical to RenderWidth/Height except in Multi-pass
     * Custom, where the first pass has been moved below it. */
    unsigned int NRSourceWidth() const { return _nrSourceWidth != 0 ? _nrSourceWidth : _renderWidth; }
    unsigned int NRSourceHeight() const { return _nrSourceHeight != 0 ? _nrSourceHeight : _renderHeight; }

    /* What the colour codec must branch on: whether the frame the model is about
     * to see is linear HDR. Distinct from IsHdr(), which describes the flag this
     * feature was created with. */
    bool NRGameIsHdr() const { return _nrGameIsHdr; }
#endif
    NVSDK_NGX_Handle* Handle() const { return _handle; };
    static unsigned int GetNextHandleId() { return handleCounter++; }
    int GetFeatureFlags() const { return _featureFlags; }

    virtual bool IsWithDx12() = 0;
    virtual feature_version Version() = 0;
    virtual Upscaler GetUpscalerType() const = 0;
    virtual API Api() const = 0;
    std::string Name() const { return UpscalerDisplayName(GetUpscalerType()); };
    std::string ShortName() const { return UpscalerShortName(GetUpscalerType()); }; // Without the version
    virtual std::optional<double> ReadUpscalerTime(void* commandQueue) { return std::nullopt; }
    virtual void ReadDetailedGpuTimes(void* commandQueue, std::vector<DetailedGpuTime>& detailedGpuTimes) {};

    virtual size_t JitterCount() { return _jitterInfo.size(); }

    virtual void TickFrozenCheck();
    virtual bool IsFrozen() { return _featureFrozen; };
    virtual bool UpdateOutputResolution(const NVSDK_NGX_Parameter* InParameters);
    virtual unsigned int DisplayWidth() { return _displayWidth; };
    virtual unsigned int DisplayHeight() { return _displayHeight; };
    virtual unsigned int TargetWidth() { return _targetWidth; };
    virtual unsigned int TargetHeight() { return _targetHeight; };
    virtual unsigned int RenderWidth() { return _renderWidth; };
    virtual unsigned int RenderHeight() { return _renderHeight; };
    virtual NVSDK_NGX_PerfQuality_Value PerfQualityValue() { return _perfQualityValue; }
    virtual bool IsInitParameters() { return _initParameters; };
    virtual bool IsInited() { return _isInited; }
    virtual float Sharpness()
    {
        if (_actualSharpness.has_value())
            return _actualSharpness.value();
        return _sharpness;
    }
    virtual bool HasColor() { return _hasColor; }
    virtual bool HasDepth() { return _hasDepth; }
    virtual bool HasMV() { return _hasMV; }
    virtual bool HasTM() { return _hasTM; }
    virtual bool AccessToReactiveMask() { return _accessToReactiveMask; }
    virtual bool HasExposure() { return _hasExposure; }
    virtual bool HasOutput() { return _hasOutput; }
    virtual bool ModuleLoaded() { return _moduleLoaded; }
    virtual long FrameCount() { return _frameCount; }
    virtual bool DepthLinear() { return _depthLinear; }

    virtual bool AutoExposure() { return _initFlags.AutoExposure; }
    virtual bool DepthInverted() { return _initFlags.DepthInverted; }
    virtual bool IsHdr() { return _initFlags.IsHdr; }
    virtual bool JitteredMV() { return _initFlags.JitteredMV; }
    virtual bool LowResMV() { return _initFlags.LowResMV; }
    virtual bool SharpenEnabled() { return _initFlags.SharpenEnabled; }

    virtual bool CallsUpscalerEndByItself() { return false; }

    IFeature(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters) { SetHandle(InHandleId); }

    virtual ~IFeature() {}
};
