#include <pch.h>
#include <Config.h>
#include "IFeature.h"


#if OPTI_DLSSNR

DlssNr::Mode IFeature::NREffectiveMode() const
{
    auto mode = DlssNr::ConfiguredMode();

    if (!Config::Instance()->DlssNrEnabled.value_or_default())
        return DlssNr::Mode::PostProcess;

    /*
     * The reordered arrangements are built in IFeature_Dx12's pipeline and nowhere else. On D3D11 and
     * Vulkan the model still runs, but only after the upscaler -- so claiming any other mode there
     * would clear IsHDR and AutoExposure on a feature nothing runs ahead of, or hold it at 1:1 with no
     * second feature to enlarge the result. Both are worse than the honest fallback.
     */
    if (Api() != API::DX12)
        return DlssNr::Mode::PostProcess;

    /*
     * Only DLSS and Ray Reconstruction reach NRPrepareForCreate, so only they can have their flags
     * cleared or their target held at 1:1.
     *
     * This matters beyond the arrangement not working elsewhere. NRNeedsRebuild compares the mode the
     * feature was built for against this one, so returning anything but PostProcess for an upscaler
     * that never records a built mode would ask for a rebuild every single frame, for ever.
     */
    const auto upscaler = GetUpscalerType();

    if (upscaler != Upscaler::DLSS && upscaler != Upscaler::DLSSD)
        return DlssNr::Mode::PostProcess;

    if (!DlssNr::UsesTwoFeatures(mode))
        return mode;

    /*
     * The selected pipeline has to match the upscaler that is actually running.
     * OptiScaler does not substitute one for the other: Ray Reconstruction
     * needs G-buffer inputs a Super Resolution integration never supplies, and
     * running Super Resolution where the game set up for RR would hand the model
     * an undenoised path-traced signal.
     *
     * A mismatch falls back to the conventional ordering rather than
     * half-applying the arrangement, which would clear flags on one feature
     * while never creating the other.
     */
    const bool rrActive = upscaler == Upscaler::DLSSD;
    const bool wantsRr = Config::Instance()->DlssNrFeature1Pipeline.value_or_default() ==
                         (uint32_t) DlssNr::Feature1Pipeline::RayReconstruction;

    if (wantsRr != rrActive)
    {
        static bool warned = false;

        if (!warned)
        {
            warned = true;
            LOG_WARN("DLSS-NR: multi-pass is set to the {} pipeline but the active upscaler is {}; using "
                     "the conventional ordering instead",
                     wantsRr ? "Ray Reconstruction" : "Super Resolution", rrActive ? "RR" : "not RR");
        }

        return DlssNr::Mode::PostProcess;
    }

    return mode;
}

/*
 * The 1:1 size the first pass runs at.
 *
 * Equals the game's render resolution in every mode except Multi-pass Custom,
 * where it can be lowered to make that pass cheaper. The result is brought back
 * up to render resolution afterwards, so this trades the first pass's quality
 * for its cost and changes nothing downstream.
 *
 * The floor is Ultra Performance relative to the DISPLAY resolution -- one third
 * of it -- because below that the first pass has less to work with than any
 * shipping DLSS preset would ever hand it, and the filter afterwards cannot
 * invent what was thrown away. The ceiling is the render resolution itself:
 * feeding the first pass an upsampled image would cost more and add nothing.
 */
void IFeature::NRFeature1Size(unsigned int& outWidth, unsigned int& outHeight) const
{
    /*
     * _renderWidth may already have been moved down to the first pass's size by
     * SetInitParameters, so the preserved source is what has to be read here --
     * otherwise a second call clamps against an already-clamped value and drifts
     * downwards a little more every time.
     */
    outWidth = NRSourceWidth();
    outHeight = NRSourceHeight();

    if (NREffectiveMode() != DlssNr::Mode::MultiPassCustom)
        return;

    if (outHeight == 0 || _displayHeight == 0)
        return;

    const int percent = Config::Instance()->DlssNrFeature1Scale.value_or_default();

    // 0 means "leave it at render resolution", which is the no-resample case.
    if (percent <= 0)
        return;

    const unsigned int floorHeight = _displayHeight / 3; // Ultra Performance
    unsigned int wanted = (unsigned int) (((uint64_t) _displayHeight * (uint64_t) percent) / 100ull);

    if (wanted < floorHeight)
        wanted = floorHeight;

    // Never above the render resolution -- there is nothing to gain.
    if (wanted >= outHeight)
        return;

    // Keep it even; odd extents upset the filtering at the edges.
    outHeight = wanted & ~1u;

    if (outHeight < 32)
        outHeight = 32;

    outWidth = (unsigned int) (((uint64_t) NRSourceWidth() * (uint64_t) outHeight) /
                               (uint64_t) NRSourceHeight()) &
               ~1u;

    if (outWidth < 32)
        outWidth = 32;
}

bool IFeature::NRApplyFeature1Hold()
{
    if (!NRUsesTwoFeatures())
        return false;

    if (_nrSourceWidth == 0 || _nrSourceHeight == 0)
        return false;

    unsigned int width = 0;
    unsigned int height = 0;
    NRFeature1Size(width, height);

    if (width == 0 || height == 0)
        return false;

    if (_targetWidth != width || _targetHeight != height)
    {
        LOG_DEBUG("DLSS-NR multi-pass: restoring the first pass's 1:1 target, {}x{} rather than {}x{}",
                  width, height, _targetWidth, _targetHeight);
    }

    _targetWidth = width;
    _targetHeight = height;

    return true;
}

bool IFeature::NRFeature1IsResampled() const
{
    unsigned int width = 0;
    unsigned int height = 0;
    NRFeature1Size(width, height);

    return width != NRSourceWidth() || height != NRSourceHeight();
}

/*
 * Whether the final Super Resolution pass in a multi-pass chain is given the game's real jitter
 * offsets, or zeros.
 *
 * One function for both pipelines on purpose. From this pass's point of view the only thing that
 * differs upstream is which feature produced the resolved 1:1 image -- DLAA on the non-RT path, Ray
 * Reconstruction on the RT one -- and nothing about that changes the answer. Splitting it in two is
 * how the variants would drift.
 *
 * The default is zero. The first pass consumed the game's sequence and its output is grid-aligned, so
 * a per-frame Halton offset describes a subpixel displacement the image no longer contains.
 * Reprojecting against it shimmers with the period of the jitter sequence, which is exactly what the
 * first pass existed to remove. The render-side projection jitter is untouched by any of this: the
 * first pass, DLAA or RR, still receives the game's sequence intact.
 *
 * MVJittered overrides the setting rather than competing with it. The flag says the game baked its
 * jitter into the motion vectors and expects DLSS to cancel it -- using these very offsets. Zeroing
 * them leaves the offset uncancelled, so every vector is wrong by a full jitter offset every frame,
 * which is worse than the problem being fixed. Forwarding the real offsets restores the cancellation
 * and accepts a sample-placement error bounded by a single jitter delta instead.
 *
 * The flag itself is read once, in SetInitParameters, from the create flags the game supplied --
 * DLSS's on the SR path, Ray Reconstruction's on the RR path, both through the same parameter and
 * into the same _initFlags.JitteredMV. The branch below consumes that one normalised value.
 */
bool IFeature::NRFinalPassForwardsJitter(bool editTransferred) const
{
    // The member rather than the accessor: that one is not const, and this needs to be.
    const bool mvJittered = _initFlags.JitteredMV;
    const bool forwards = mvJittered || editTransferred;

    // Once per feature, so the console says which pipeline a title took and which vector path it hit
    // without turning into per-frame noise.
    static bool reported = false;

    if (!reported)
    {
        reported = true;
        LOG_INFO("DLSS-NR multi-pass: first pass is {}, motion vectors are {}, final Super Resolution "
                 "pass gets {} jitter{}",
                 GetUpscalerType() == Upscaler::DLSSD ? "Ray Reconstruction" : "Super Resolution (DLAA)",
                 mvJittered ? "jittered (MVJittered set, so DLSS cancels the offset itself)"
                            : "not jittered",
                 forwards ? "the game's real" : "zero",
                 mvJittered && !editTransferred
                     ? " -- forced by MVJittered, which needs the real offsets to cancel with"
                     : "");
    }

    return forwards;
}

/*
 * Settle which arrangement this feature is being built for, and apply what that decides.
 *
 * Deliberately NOT part of SetInitParameters. That runs from a feature's constructor -- DLSSFeature's,
 * with DLSSFeatureDx12 not yet built -- where Api() and GetUpscalerType() are still pure virtual.
 * Calling them there aborts the process with "pure virtual function being called", which is exactly
 * what happened the first time a placement change actually rebuilt a feature.
 *
 * Called from ProcessInitParams instead, which runs from InitInternal with the object complete, and
 * before the create flags are assembled from _initFlags.
 */
void IFeature::NRPrepareForCreate()
{
    _nrModeAtCreate = NREffectiveMode();
    _nrReorderedAtCreate = NRWantsReorderedFlags();

    /*
     * In the reordered arrangement the model runs first and hands the upscaler a tone-mapped,
     * display-referred picture. Telling it the frame is still linear HDR blows out the colour; leaving
     * AutoExposure on has it metering a picture that has already been exposed. Both are latched at
     * creation, which is what NRNeedsRebuild exists to catch.
     *
     * The exposure the upscaler needs in AutoExposure's place is supplied as an explicit 1x1 identity.
     */
    if (_nrReorderedAtCreate)
    {
        _initFlags.IsHdr = false;
        _initFlags.AutoExposure = false;
        LOG_INFO("DLSS-NR: the model runs before the upscale, so this feature is created with IsHDR and "
                 "AutoExposure cleared");
    }

    /*
     * Multi-pass holds the first feature at 1:1 -- the second one performs the single enlargement in
     * the frame. In the Custom variant the first pass may sit below the game's render resolution, its
     * inputs resampled down and its result brought back up before the model sees it.
     */
    if (NRUsesTwoFeatures() && _nrSourceWidth != 0 && _nrSourceHeight != 0)
    {
        unsigned int f1Width = 0;
        unsigned int f1Height = 0;
        NRFeature1Size(f1Width, f1Height);

        if (f1Width != 0 && f1Height != 0)
        {
            _renderWidth = f1Width;
            _renderHeight = f1Height;
            _targetWidth = f1Width;
            _targetHeight = f1Height;

            LOG_INFO("DLSS-NR multi-pass: the first pass runs 1:1 at {}x{} (the game renders {}x{}), and "
                     "a second feature enlarges to {}x{}",
                     f1Width, f1Height, _nrSourceWidth, _nrSourceHeight, _displayWidth, _displayHeight);
        }
    }
}

#endif // OPTI_DLSSNR
void IFeature::SetHandle(unsigned int InHandleId)
{
    _handle = new NVSDK_NGX_Handle { InHandleId };
    LOG_INFO("Handle: {0}", _handle->Id);
}

bool IFeature::SetInitParameters(NVSDK_NGX_Parameter* InParameters)
{
    unsigned int width = 0;
    unsigned int outWidth = 0;
    unsigned int height = 0;
    unsigned int outHeight = 0;
    int pqValue = 0;

    if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &_featureFlags) == NVSDK_NGX_Result_Success)
    {
        if (Config::Instance()->HDR.has_value())
        {
            LOG_INFO("HDR flag overrided by user: {}", Config::Instance()->HDR.value());
            _initFlags.IsHdr = Config::Instance()->HDR.value();
        }
        else
        {
            _initFlags.IsHdr = _featureFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
        }

        if (Config::Instance()->OverrideSharpness.has_value())
        {
            LOG_INFO("SharpenEnabled flag overrided by user: {}", Config::Instance()->OverrideSharpness.value());
            _initFlags.SharpenEnabled = Config::Instance()->OverrideSharpness.value();
        }
        else
        {
            _initFlags.SharpenEnabled = _featureFlags & NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;
        }

        if (Config::Instance()->DepthInverted.has_value())
        {
            LOG_INFO("DepthInverted flag overrided by user: {}", Config::Instance()->DepthInverted.value());
            _initFlags.DepthInverted = Config::Instance()->DepthInverted.value();
        }
        else
        {
            _initFlags.DepthInverted = _featureFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        }

        if (Config::Instance()->JitterCancellation.has_value())
        {
            LOG_INFO("JitteredMV flag overrided by user: {}", Config::Instance()->JitterCancellation.value());
            _initFlags.JitteredMV = Config::Instance()->JitterCancellation.value();
        }
        else
        {
            _initFlags.JitteredMV = _featureFlags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
        }

        if (Config::Instance()->DisplayResolution.has_value())
        {
            LOG_INFO("LowResMV flag overrided by user: {}", !Config::Instance()->DisplayResolution.value());
            _initFlags.LowResMV = !Config::Instance()->DisplayResolution.value();
        }
        else
        {
            _initFlags.LowResMV = _featureFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        }

        // First check state to prevent upscaler re-init loops
        if (State::Instance().autoExposure.has_value())
        {
            LOG_INFO("AutoExposure flag overrided by OptiScaler: {}", State::Instance().autoExposure.value());
            _initFlags.AutoExposure = State::Instance().autoExposure.value();
        }
        else if (Config::Instance()->AutoExposure.has_value())
        {
            LOG_INFO("AutoExposure flag overrided by user: {}", Config::Instance()->AutoExposure.value());
            _initFlags.AutoExposure = Config::Instance()->AutoExposure.value();
        }
        else
        {
            _initFlags.AutoExposure = _featureFlags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        }

#if OPTI_DLSSNR
        /*
         * What the game's own frame is, recorded before NRPrepareForCreate can change what IsHdr()
         * reports. The colour codec branches on this: reading the overridden flag would make the
         * encode a no-op on exactly the frames that most need it, and the model would then be shown a
         * linear frame it was never trained on while the pass appeared to do nothing.
         *
         * A plain copy, deliberately. Everything here runs from a feature's CONSTRUCTOR, before the
         * most-derived class exists, so Api() and GetUpscalerType() are still pure virtual -- asking
         * which arrangement is in force would abort the process. That decision is made later, in
         * NRPrepareForCreate.
         */
        _nrGameIsHdr = _initFlags.IsHdr;
#endif

        LOG_INFO("Init Flag AutoExposure: {}", _initFlags.AutoExposure);
        LOG_INFO("Init Flag DepthInverted: {}", _initFlags.DepthInverted);
        LOG_INFO("Init Flag IsHdr: {}", _initFlags.IsHdr);
        LOG_INFO("Init Flag JitteredMV: {}", _initFlags.JitteredMV);
        LOG_INFO("Init Flag LowResMV: {}", _initFlags.LowResMV);
        LOG_INFO("Init Flag SharpenEnabled: {}", _initFlags.SharpenEnabled);

        if (State::Instance().activeFgInput == FGInput::Upscaler)
        {
            Config::Instance()->FGXeFGDepthInverted = _initFlags.DepthInverted;
            Config::Instance()->FGXeFGJitteredMV = _initFlags.JitteredMV;
            Config::Instance()->FGXeFGHighResMV = !_initFlags.LowResMV;
            LOG_DEBUG("XeFG DepthInverted: {}", Config::Instance()->FGXeFGDepthInverted.value_or_default());
            LOG_DEBUG("XeFG JitteredMV: {}", Config::Instance()->FGXeFGJitteredMV.value_or_default());
            LOG_DEBUG("XeFG HighResMV: {}", Config::Instance()->FGXeFGHighResMV.value_or_default());
            Config::Instance()->SaveXeFG();
        }
    }

    if (InParameters->Get(NVSDK_NGX_Parameter_OutWidth, &outWidth) == NVSDK_NGX_Result_Success &&
        InParameters->Get(NVSDK_NGX_Parameter_OutHeight, &outHeight) == NVSDK_NGX_Result_Success)
    {
        InParameters->Get(NVSDK_NGX_Parameter_Width, &width);
        InParameters->Get(NVSDK_NGX_Parameter_Height, &height);
        InParameters->Get(NVSDK_NGX_Parameter_PerfQualityValue, &pqValue);

        GetDynamicOutputResolution(InParameters, &outWidth, &outHeight);

        // Thanks to Crytek added these checks
        if (width > 16384 || width < 20)
            width = 0;

        if (height > 16384 || height < 20)
            height = 0;

        if (outWidth > 16384 || outWidth < 20)
            outWidth = 0;

        if (outHeight > 16384 || outHeight < 20)
            outHeight = 0;

        if (pqValue > 5 || pqValue < 0)
            pqValue = 1;

        // When using extended limits render res might be bigger than display res
        // it might create rendering issues but extending limits is an advanced option after all
        if (!Config::Instance()->ExtendedLimits.value_or_default())
        {
            _displayWidth = width > outWidth ? width : outWidth;
            _displayHeight = height > outHeight ? height : outHeight;
            _targetWidth = _displayWidth;
            _targetHeight = _displayHeight;
            _renderWidth = width < outWidth ? width : outWidth;
            _renderHeight = height < outHeight ? height : outHeight;
        }
        else
        {
            _displayWidth = outWidth;
            _displayHeight = outHeight;
            _targetWidth = _displayWidth;
            _targetHeight = _displayHeight;
            _renderWidth = width;
            _renderHeight = height;
        }


#if OPTI_DLSSNR
        /*
         * The game's own render resolution, before the multi-pass hold can move _renderWidth down.
         * Everything after the first pass works at this size: the depth and motion vectors the game
         * supplies are here, the model runs here, and the second feature enlarges from here to
         * display.
         *
         * Another plain copy. The hold itself needs to know which arrangement is in force, which
         * cannot be asked from a constructor, so it happens in NRPrepareForCreate instead.
         */
        _nrSourceWidth = _renderWidth;
        _nrSourceHeight = _renderHeight;
#endif

        _perfQualityValue = (NVSDK_NGX_PerfQuality_Value) pqValue;

        LOG_INFO("Render Resolution: {0}x{1}, Display Resolution {2}x{3}, Quality: {4}", _renderWidth, _renderHeight,
                 _displayWidth, _displayHeight, pqValue);

        // If output scaling is enabled and render res is equal to display res, enable low res MVs
        // because we will use display res MV as render res and upscale with it
        if (Config::Instance()->OutputScalingEnabled.value_or_default() && !_initFlags.LowResMV &&
            _renderWidth == _displayWidth)
        {
            LOG_INFO("Output Scaling is active with render size equal to display size, enabling low res MVs");
            _initFlags.LowResMV = true;
        }

        return true;
    }

    LOG_ERROR("Can't set parameters!");
    return false;
}

void IFeature::GetRenderResolution(const NVSDK_NGX_Parameter* InParameters, unsigned int* OutWidth,
                                   unsigned int* OutHeight)
{
    if (InParameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, OutWidth) !=
            NVSDK_NGX_Result_Success ||
        InParameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, OutHeight) !=
            NVSDK_NGX_Result_Success)
    {
        LOG_WARN("No subrect dimension info!");

        unsigned int width;
        unsigned int height;
        unsigned int outWidth;
        unsigned int outHeight;

        do
        {
            if (InParameters->Get(NVSDK_NGX_Parameter_Width, &width) == NVSDK_NGX_Result_Success &&
                InParameters->Get(NVSDK_NGX_Parameter_Height, &height) == NVSDK_NGX_Result_Success)
            {
                if (InParameters->Get(NVSDK_NGX_Parameter_OutWidth, &outWidth) == NVSDK_NGX_Result_Success &&
                    InParameters->Get(NVSDK_NGX_Parameter_OutHeight, &outHeight) == NVSDK_NGX_Result_Success)
                {
                    if (width < outWidth)
                    {
                        *OutWidth = width;
                        *OutHeight = height;
                        break;
                    }

                    *OutWidth = outWidth;
                    *OutHeight = outHeight;
                }
                else
                {
                    if (width < RenderWidth())
                    {
                        *OutWidth = width;
                        *OutHeight = height;
                        break;
                    }

                    *OutWidth = RenderWidth();
                    *OutHeight = RenderHeight();
                    return;
                }
            }

            *OutWidth = RenderWidth();
            *OutHeight = RenderHeight();

        } while (false);
    }

    _renderWidth = *OutWidth;
    _renderHeight = *OutHeight;

    // Should not be needed but who knows
    // if (_renderHeight == _displayHeight && _renderWidth == _displayWidth && _perfQualityValue !=
    // NVSDK_NGX_PerfQuality_Value_DLAA)
    //{
    //	InParameters->Set(NVSDK_NGX_Parameter_PerfQualityValue, 5);
    //	InParameters->Set(NVSDK_NGX_Parameter_Scale, 1.0f);
    //	InParameters->Set(NVSDK_NGX_Parameter_SuperSampling_ScaleFactor, 1.0f);
    // }

    JitterInfo ji {};
    if (_jitterInfo.size() < 350 &&
        InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &ji.x) == NVSDK_NGX_Result_Success &&
        InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &ji.y) == NVSDK_NGX_Result_Success)
    {
        _jitterInfo.insert(std::make_pair(ji.x, ji.y));
    }
}

float IFeature::GetSharpness(const NVSDK_NGX_Parameter* InParameters)
{
    if (Config::Instance()->OverrideSharpness.value_or_default())
        return Config::Instance()->Sharpness.value_or_default();

    float sharpness = 0.0f;

    if (InParameters->Get(NVSDK_NGX_Parameter_Sharpness, &sharpness) == NVSDK_NGX_Result_Success)
    {
        if (sharpness < 0.0f)
            sharpness = 0.0f;
        else if (sharpness > 1.0f)
            sharpness = 1.0f;
    }

    return sharpness;
}

void IFeature::TickFrozenCheck()
{
    static long updatesWithoutFramecountChange = 0;

    if (_isInited)
    {
        static auto lastFrameCount = _frameCount;

        if (_frameCount == lastFrameCount)
            updatesWithoutFramecountChange++;
        else
            updatesWithoutFramecountChange = 0;

        lastFrameCount = _frameCount;

        _featureFrozen = updatesWithoutFramecountChange > 10;
    }
}

bool IFeature::UpdateOutputResolution(const NVSDK_NGX_Parameter* InParameters)
{
    // Check for FSR's dynamic resolution output
    auto fsrDynamicOutputWidth = 0;
    auto fsrDynamicOutputHeight = 0;

    InParameters->Get("FSR.upscaleSize.width", &fsrDynamicOutputWidth);
    InParameters->Get("FSR.upscaleSize.height", &fsrDynamicOutputHeight);

    if (Config::Instance()->OutputScalingEnabled.value_or_default())
    {
        if (_targetWidth == fsrDynamicOutputWidth || _targetHeight == fsrDynamicOutputHeight)
            return false;

        if (fsrDynamicOutputWidth > 0 && fsrDynamicOutputHeight > 0 &&
            ((unsigned int) (fsrDynamicOutputWidth * Config::Instance()->OutputScalingMultiplier.value_or_default()) !=
                 _targetWidth ||
             fsrDynamicOutputWidth != _displayWidth ||
             (unsigned int) (fsrDynamicOutputHeight * Config::Instance()->OutputScalingMultiplier.value_or_default()) !=
                 _targetHeight ||
             fsrDynamicOutputHeight != _displayHeight))
        {
            _targetWidth = static_cast<unsigned int>(fsrDynamicOutputWidth *
                                                     Config::Instance()->OutputScalingMultiplier.value_or_default());
            _displayWidth = fsrDynamicOutputWidth;
            _targetHeight = static_cast<unsigned int>(fsrDynamicOutputHeight *
                                                      Config::Instance()->OutputScalingMultiplier.value_or_default());
            _displayHeight = fsrDynamicOutputHeight;

            return true;
        }
    }
    else
    {
        if (fsrDynamicOutputWidth > 0 && fsrDynamicOutputHeight > 0 &&
            (fsrDynamicOutputWidth != _targetWidth || fsrDynamicOutputWidth != _displayWidth ||
             fsrDynamicOutputHeight != _targetHeight || fsrDynamicOutputHeight != _displayHeight))
        {
            _targetWidth = fsrDynamicOutputWidth;
            _displayWidth = fsrDynamicOutputWidth;
            _targetHeight = fsrDynamicOutputHeight;
            _displayHeight = fsrDynamicOutputHeight;

            return true;
        }
    }

    return false;
}

void IFeature::GetDynamicOutputResolution(NVSDK_NGX_Parameter* InParameters, unsigned int* width, unsigned int* height)
{
    // FSR 3.1 uses upscaleSize for this, max size should stay the same
    int supportsUpscaleSize = 0;
    InParameters->Get("OptiScaler.SupportsUpscaleSize", &supportsUpscaleSize);
    if (supportsUpscaleSize)
    {
        InParameters->Set("OptiScaler.SupportsUpscaleSize", 0);
        return;
    }

    // Check for FSR's dynamic resolution output
    auto fsrDynamicOutputWidth = 0;
    auto fsrDynamicOutputHeight = 0;

    InParameters->Get("FSR.upscaleSize.width", &fsrDynamicOutputWidth);
    InParameters->Get("FSR.upscaleSize.height", &fsrDynamicOutputHeight);

    if (fsrDynamicOutputWidth > 0 && fsrDynamicOutputHeight > 0)
    {
        *width = fsrDynamicOutputWidth;
        *height = fsrDynamicOutputHeight;
    }
}
