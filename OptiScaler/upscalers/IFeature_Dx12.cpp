#include <pch.h>

#include <functional>
#include <vector>

#include "IFeature_Dx12.h"
#include "State.h"
#include <dlssnr/DlssNr.h>

void IFeature_Dx12::ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                                    D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState) const
{
    if (InBeforeState == InAfterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = InResource;
    barrier.Transition.StateBefore = InBeforeState;
    barrier.Transition.StateAfter = InAfterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    InCommandList->ResourceBarrier(1, &barrier);
}


/*
 * Copies the render rect of one resource into another of the same size.
 *
 * Used to keep what the model was shown before it writes over it; the edit transfer needs both ends.
 */
static void CopyRenderRect(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* src, ID3D12Resource* dst)
{
    if (cmdList == nullptr || src == nullptr || dst == nullptr)
        return;

    D3D12_RESOURCE_BARRIER barriers[2] = {};

    for (int i = 0; i < 2; ++i)
    {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    barriers[0].Transition.pResource = src;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.pResource = dst;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    cmdList->ResourceBarrier(2, barriers);

    cmdList->CopyResource(dst, src);

    for (int i = 0; i < 2; ++i)
        std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);

    cmdList->ResourceBarrier(2, barriers);
}
bool IFeature_Dx12::Init(ID3D12Device* InDevice, ID3D12GraphicsCommandList* InCommandList,
                         NVSDK_NGX_Parameter* InParameters)
{
    Device = InDevice;

    auto result = InitInternal(InCommandList, InParameters);

    if (result)
    {
        if (!Config::Instance()->OverlayMenu.value_or_default() && (Imgui == nullptr || Imgui.get() == nullptr))
            Imgui = std::make_unique<Menu_Dx12>(Util::GetProcessWindow(), InDevice);

        OutputScaler = std::make_unique<OS_Dx12>("Output Scaling", InDevice, (TargetWidth() < DisplayWidth()));
        RCAS = std::make_unique<RCAS_Dx12>("RCAS", InDevice);
        Bias = std::make_unique<Bias_Dx12>("Bias", InDevice); // TODO: not needed on DLSS/DLSSD
        Magnifier = std::make_unique<Magnifier_Dx12>("Magnifier", InDevice);

        UpscalerTime = std::make_unique<GpuTime_Dx12>(InDevice);
    }

    return result;
}

bool IFeature_Dx12::Evaluate(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    if (!IsInited())
    {
        LOG_ERROR("Not inited!");
        return false;
    }

#if OPTI_DLSSNR
    /*
     * A placement change needs the feature rebuilt before anything else happens.
     *
     * The target resolution and the HDR and exposure flags are all fixed at creation, and nothing
     * else here notices: engines rebuild on a resolution change, and the game's resolutions have not
     * moved. Without this the multi-pass stage below -- which reads config live -- immediately routes
     * the upscaler's output into a render-resolution buffer while its feature is still built to write
     * display resolution, and every evaluate returns FAIL_InvalidParameter.
     *
     * Reported as success rather than failure: this is one deliberate frame during a mode change, not
     * an upscaler that has gone wrong, and saying otherwise puts an error on screen for it.
     */
    if (NRNeedsRebuild())
    {
        LOG_INFO("DLSS-NR placement changed; rebuilding the upscaler feature for it");
        State::Instance().changeBackend[Handle()->Id] = true;
        return true;
    }
#endif

    if (Config::Instance()->OverrideSharpness.value_or_default())
        _sharpness = Config::Instance()->Sharpness.value_or_default();
    else
        _sharpness = GetSharpness(InParameters);

    if (_sharpness > 1.0f)
        _sharpness = 1.0f;

    // Those upcalers don't have their own sharpness so always need to use RCAS when sharpness is set
    auto upscaler = GetUpscalerType();
    bool useRcas = upscaler == Upscaler::XeSS ||
                   (upscaler == Upscaler::DLSS && Version() >= feature_version(2, 5, 1)) || upscaler == Upscaler::DLSSD;

    if (!useRcas)
        useRcas = Config::Instance()->RcasEnabled.value_or_default();

    if (_sharpness == 0.0f)
        useRcas = false;

    // Need RCAS for MAS
    if (!useRcas && (Config::Instance()->MotionSharpnessEnabled.value_or_default() &&
                     Config::Instance()->MotionSharpness.value_or_default() > 0.0f))
    {
        useRcas = true;
    }

    if (!RCAS->IsInit())
        useRcas = false;

    bool useOutputScaling =
        Config::Instance()->OutputScalingEnabled.value_or_default() && (LowResMV() || RenderWidth() == DisplayWidth());

    if (!OutputScaler->IsInit())
        useOutputScaling = false;

    ID3D12Resource* paramOutput = nullptr;
    ID3D12Resource* paramMotion = nullptr;
    ID3D12Resource* paramDepth = nullptr;

    InParameters->Get(NVSDK_NGX_Parameter_Output, &paramOutput);
    InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &paramMotion);
    InParameters->Get(NVSDK_NGX_Parameter_Depth, &paramDepth);

    // Order is important as that's the order of shader dispatch
    std::vector<ShaderPass> pipeline;

#if OPTI_DLSSNR
    /*
     * Multi-pass: the model on the first pass's 1:1 result, then a second Super
     * Resolution feature performing the single enlargement to display size.
     *
     * Pushed first so it sits closest to the upscaler -- the model wants the
     * frame before Output Scaling or RCAS has been over it, and the enlargement
     * has to happen before either of those makes sense.
     *
     * Two features rather than one because the requirements are contradictory
     * within a single one: Ray Reconstruction refuses to be created without
     * IsHDR, while an upscaler consuming the model's display-referred output
     * needs it cleared. Nothing reconciles that in one feature; nothing prevents
     * holding two.
     */
    const bool useMultiPass = DlssNr::UsesTwoFeatures(NRBuiltMode());

    /*
     * How the enlargement is done, and why spatial is the default.
     *
     * The first pass resolves the game's jitter -- that is what DLAA and Ray Reconstruction are for --
     * so what reaches the enlargement is grid-aligned with no subpixel variation left. A temporal
     * upscaler then reconstructs from one sample position per pixel, identical every frame, while the
     * model re-decides detail underneath it. That is soft, and it warps whenever the camera moves.
     * Chaining two temporal passes cannot preserve jitter for the second one; it is a property of the
     * arrangement, not a bug in it.
     *
     * A spatial filter asks for no jitter and keeps no history, so neither failure is available to it.
     * It is no sharper -- both are limited to what the first pass produced -- but it is steady.
     */
    const bool spatialEnlarge = Config::Instance()->DlssNrMultiPassEnlarge.value_or_default() != 0;

    if (useMultiPass)
    {
        if (spatialEnlarge)
        {
            if (MultiPassScaler == nullptr)
                MultiPassScaler = std::make_unique<OS_Dx12>("DLSS-NR Enlarge", Device, true);
        }
        else
        {
            if (SecondUpscaler == nullptr)
                SecondUpscaler = std::make_unique<DlssNr_SecondUpscaler_Dx12>("DLSS-NR Second Upscaler", Device);

            SecondUpscaler->BeginFrame();

            /*
             * Built here, before the pipeline records a single command.
             *
             * This used to happen inside the dispatch, between the edit transfer writing the
             * enlargement's source texture and the enlargement reading it -- so any change that
             * forced a rebuild freed that texture in the gap and handed the freed pointer straight to
             * NGX. Toggling the colour-space match made it reachable from the menu, but a quality
             * preset change would have done the same thing, silently, all along.
             *
             * Everything it depends on is already known at this point, and nothing this frame has
             * been recorded yet, so a rebuild here costs one reset frame and disturbs nothing.
             */
            const bool matchColour = Config::Instance()->DlssNrMatchGameColourSpace.value_or_default();

            /*
             * The enlargement is created for the colour space it is actually handed.
             *
             * What reaches it is the game's own frame: Neural Rendering returns the picture in the
             * space it received it, and the edit transfer applies a near-unity brightness ratio to
             * the game's jittered buffer directly. Declaring that display-referred -- which is what
             * clearing IsHDR does -- selects the guide's LDR path, which quantises to 8 bits and
             * expects a perceptually linear encoding, and handing that linear colour is the guide's
             * own account of banding and colour shifting.
             */
            const bool passIsHdr = matchColour && NRGameIsHdr();
            const bool passAutoExposure = matchColour && AutoExposure();

            /*
             * The enlargement is put on the first pass's preset by default. Two Super Resolution
             * features in one frame on independently chosen presets can disagree about the exposure
             * of the picture they are passing between them -- the guide supports exposure input on
             * Presets J and K only, and Preset L always uses AutoExposure, which this pass may have
             * cleared.
             */
            unsigned int matchedPreset = 0;
            const bool hasPreset = Config::Instance()->DlssNrMatchPreset.value_or_default() &&
                                   DlssNr::PresetForQuality(InParameters, (int) PerfQualityValue(),
                                                            matchedPreset);

            if (!SecondUpscaler->EnsureCreated(InCommandList, RenderWidth(), RenderHeight(), DisplayWidth(),
                                               DisplayHeight(), (int) PerfQualityValue(), DepthInverted(),
                                               JitteredMV(), LowResMV(), hasPreset, matchedPreset, passIsHdr,
                                               passAutoExposure))
            {
                // Without the enlargement the frame would be a render-resolution image in the corner
                // of a display-resolution buffer, so the mode stands down rather than showing that.
                Config::Instance()->DlssNrMode.set_volatile_value((uint32_t) DlssNr::Mode::PostProcess);
                State::Instance().changeBackend[Handle()->Id] = true;
                return true;
            }
        }

        pipeline.push_back(
            { // Setup
              [&](ID3D12Resource* nextOutput) -> ID3D12Resource*
              {
                  // The first pass writes here, at the game's render resolution;
                  // this stage reads it and enlarges into nextOutput.
                  if (spatialEnlarge)
                  {
                      if (MultiPassScaler->CreateBufferResource(Device, nextOutput, RenderWidth(),
                                                                RenderHeight(),
                                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
                      {
                          MultiPassScaler->SetBufferState(InCommandList,
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                          return MultiPassScaler->Buffer();
                      }

                      return nullptr;
                  }

                  if (SecondUpscaler->CreateInputBuffer(nextOutput, RenderWidth(), RenderHeight()))
                  {
                      SecondUpscaler->SetInputBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                      return SecondUpscaler->InputBuffer();
                  }

                  return nullptr;
              },

              // Dispatch
              [&](ID3D12Resource* input, ID3D12Resource* output) -> bool
              {
                  if (paramDepth == nullptr || paramMotion == nullptr)
                      return true;

                  /*
                   * The model, at render resolution, on what the first pass produced.
                   *
                   * A copy is taken first when the edit is going to be transferred: the transfer needs
                   * both what the model was shown and what it returned, and the model writes over its
                   * input.
                   */
                  /*
                  /*
                   * The transfer carries the model's edit onto the game's original jittered frame, so
                   * the first pass's own IMAGE is discarded and only its influence on the model
                   * survives.
                   *
                   * For Super Resolution that is a clean trade: its antialiasing is lost, and the
                   * enlargement rebuilds it from properly jittered input, which is what DLSS is for.
                   *
                   * For Ray Reconstruction the reasoning says it should not be: RR's output is a
                   * denoised path-traced signal, a brightness ratio cannot carry a spatial denoise, and
                   * feeding the raw jittered frame back in restores the noise RR just removed. It is
                   * allowed anyway, because that is a prediction and this one is cheap to measure --
                   * and predictions of this kind have already been wrong here twice. The log says which
                   * pipeline took it.
                   */
                  const bool firstPassIsRr = GetUpscalerType() == Upscaler::DLSSD;

                  /*
                   * No longer a choice. Feeding the enlargement the resolved frame with zero offsets
                   * was kept for comparison and the comparison is settled: the first pass resolves
                   * the game's jitter, so that path hands a temporal upscaler one sample position per
                   * pixel, identical every frame, and it is soft and it warps. There is no title in
                   * which that is the better answer, and leaving it selectable only invited someone
                   * to find the bad one.
                   *
                   * It can still fail, and then the fallback is the resolved frame anyway -- but as a
                   * failure, which the jitter decision below follows rather than a setting.
                   */
                  const bool transferEdit =
                      !spatialEnlarge &&
                      SecondUpscaler->CreateEditBuffers(input, RenderWidth(), RenderHeight());

                  if (transferEdit)
                  {
                      static bool reported = false;

                      if (!reported)
                      {
                          reported = true;
                          LOG_INFO("DLSS-NR multi-pass: the edit is transferred onto the game's jittered "
                                   "frame, first pass is {}{}",
                                   firstPassIsRr ? "Ray Reconstruction" : "Super Resolution (DLAA)",
                                   firstPassIsRr ? " -- watch for path-trace noise returning, since a "
                                                   "brightness ratio cannot carry a denoise"
                                                 : "");
                      }
                  }



                  /*
                   * The magnification still to come, so the model's detail can be compensated for it.
                   *
                   * Detail synthesised at render resolution is spread over the display resolution by
                   * the pass after this one, which is why the model reads weaker here than it does in
                   * post-process at the same settings -- nothing about the model changed, only how
                   * many pixels its work ended up covering.
                   */
                  DlssNr::SetEnlargementRatio(spatialEnlarge || RenderWidth() == 0
                                                  ? 1.0f
                                                  : (float) DisplayWidth() / (float) RenderWidth());

                  if (transferEdit)
                      CopyRenderRect(InCommandList, input, SecondUpscaler->EditBefore());

                  (void) DlssNr::EvaluateAfterUpscale(InCommandList, InParameters, input, RenderWidth(),
                                               RenderHeight());

                  if (spatialEnlarge)
                  {
                      MultiPassScaler->SetBufferState(InCommandList,
                                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                      return MultiPassScaler->Dispatch(InCommandList, input, output);
                  }

                  /*
                   * Carry the edit onto the game's own jittered frame, so the enlargement gets real
                   * subpixel content to reconstruct from instead of a resolved image with none.
                   */
                  ID3D12Resource* enlargeSource = input;

                  if (transferEdit)
                  {
                      ID3D12Resource* paramColor = nullptr;
                      InParameters->Get(NVSDK_NGX_Parameter_Color, &paramColor);

                      if (paramColor != nullptr)
                      {
                          /*
                           * The ratio was measured on the resolved frame; the frame it is applied to
                           * sampled the scene up to half a pixel away. Sampling the pair at the jitter
                           * offset puts the model's edit on the feature it belongs to instead of
                           * beside it -- without which the enlargement's accumulation averages the
                           * misplacement across offsets and cancels the edit rather than blurring it,
                           * which reads as the model barely doing anything.
                           */
                          float jitterX = 0.0f;
                          float jitterY = 0.0f;
                          InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitterX);
                          InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY);

                          float mvSignX = 1.0f;
                          float mvSignY = 1.0f;
                          InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvSignX);
                          InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvSignY);

                          /*
                           * Derived rather than tried. The programming guide states jitter offsets
                           * use the same coordinate and direction system as motion vectors, with
                           * (0,0) meaning no jitter -- so the direction follows from the motion
                           * vector convention the game already declares, and the offset itself is
                           * negated because it displaces the rasterised scene. The manual override
                           * is still there for an engine that disagrees with the guide.
                           */
                          float alignX = 0.0f;
                          float alignY = 0.0f;
                          DlssNr::DerivedAlign(jitterX, jitterY, mvSignX, mvSignY, alignX, alignY);

                          DlssNr::TransferEditOntoJittered(InCommandList, SecondUpscaler->EditBefore(),
                                                           input, paramColor, SecondUpscaler->EditResult(),
                                                           RenderWidth(), RenderHeight(), alignX, alignY);
                          enlargeSource = SecondUpscaler->EditResult();
                      }
                  }



                  /*
                   * Only a check now. The feature was built before this pipeline recorded anything,
                   * because rebuilding it here would free the very texture the transfer has just
                   * written and this enlargement is about to read.
                   */
                  if (!SecondUpscaler->IsCreated())
                      return false;

                  ID3D12Resource* passExposure = nullptr;

                  // Forwarded only when this pass expects an exposure of its own: with AutoExposure
                  // set DLSS derives one, and with the flags cleared the feature falls back to its
                  // own identity texture.
                  if (Config::Instance()->DlssNrMatchGameColourSpace.value_or_default() && !AutoExposure())
                      InParameters->Get(NVSDK_NGX_Parameter_ExposureTexture, &passExposure);


                  SecondUpscaler->SetInputBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                  float mvScaleX = 1.0f;
                  float mvScaleY = 1.0f;
                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX);
                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY);

                  /*
                   * Re-read rather than reused, because in Multi-pass Custom they have changed.
                   *
                   * paramDepth and paramMotion were captured at the top of Evaluate, before
                   * ResampleFeature1Inputs rewrote the parameter block. In Multi-pass Custom that
                   * rewrite is the whole point: it replaces colour, depth and motion vectors with
                   * copies at the reduced size and scales the motion vector values to match. This
                   * feature is created at that reduced size -- RenderWidth() has been moved down to
                   * it -- so handing it the originals gave it full-render-resolution guides for a
                   * pass expecting smaller ones. DLSS reads the top-left corner of each, which is a
                   * crop rather than a view, and the motion vector scale read just above had already
                   * been scaled for the resampled buffers, so the values were wrong by that ratio
                   * too.
                   *
                   * The colour path never had this problem: it is read inside this lambda and so
                   * always saw the post-resample block. These now do the same.
                   */
                  ID3D12Resource* passDepth = paramDepth;
                  ID3D12Resource* passMotion = paramMotion;
                  InParameters->Get(NVSDK_NGX_Parameter_Depth, &passDepth);
                  InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &passMotion);

                  if (passDepth == nullptr)
                      passDepth = paramDepth;

                  if (passMotion == nullptr)
                      passMotion = paramMotion;

                  /*
                   * The first pass was a straight forward of whatever the game asked for -- its create
                   * flags, its motion vector scales and its jitter sequence all untouched. The
                   * divergence is here, and only here.
                   */
                  float jitterX = 0.0f;
                  float jitterY = 0.0f;

                  if (NRFinalPassForwardsJitter(transferEdit))
                  {
                      InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitterX);
                      InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY);
                  }

                  /*
                   * Recorded whether it is the game's sequence or the deliberate zeros, because the
                   * zeros are the case worth being able to prove. One distinct phase, forever, is what
                   * this pass is meant to be given -- and if the count ever reads anything else, the
                   * setting is not doing what it says.
                   */
                  DlssNr::ObserveJitter(DlssNr::JitterSite::Final, jitterX, jitterY);

                  /*
                   * The game's scene-transition reset reaches the enlargement too.
                   *
                   * Three temporal accumulators run in this chain -- the first pass, the model, and
                   * this one -- and a reset that lands on some of them is worse than one that lands on
                   * none: the stages then disagree about which scene they are accumulating. A cut used
                   * to reset only the first pass, so the enlargement kept blending the old scene's
                   * history into the new one and smeared straight through the transition.
                   */
                  int gameReset = 0;
                  InParameters->Get(NVSDK_NGX_Parameter_Reset, &gameReset);

                  DlssNr::BeginStage(DlssNr::diag::Stage::Enlarge, InCommandList);

                  const bool enlarged = SecondUpscaler->Evaluate(
                      InCommandList, enlargeSource, output, passDepth, passMotion, passExposure,
                      jitterX, jitterY, mvScaleX, mvScaleY,
                      SecondUpscaler->ConsumeResetFlag() || gameReset != 0);

                  DlssNr::EndStage(DlssNr::diag::Stage::Enlarge, InCommandList);

                  return enlarged;
              } });
    }
#endif

    if (useOutputScaling)
    {
        pipeline.push_back(
            { // Setup
              [&](ID3D12Resource* nextOutput) -> ID3D12Resource*
              {
                  if (OutputScaler->CreateBufferResource(Device, nextOutput, TargetWidth(), TargetHeight(),
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
                  {
                      OutputScaler->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                      return OutputScaler->Buffer();
                  }
                  return nullptr;
              },

              // Dispatch
              [&](ID3D12Resource* input, ID3D12Resource* output) -> bool
              {
                  LOG_DEBUG("Scaling output...");
                  OutputScaler->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                  if (!OutputScaler->Dispatch(InCommandList, input, output))
                  {
                      Config::Instance()->OutputScalingEnabled.set_volatile_value(false);
                      State::Instance().changeBackend[Handle()->Id] = true;
                      return false;
                  }
                  return true;
              } });
    }

    _actualSharpness = _sharpness;
    if (useRcas)
    {
        pipeline.push_back(
            { // Setup
              [&](ID3D12Resource* nextOutput) -> ID3D12Resource*
              {
                  // Disable any built-in sharpness shaders
                  InParameters->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
                  _sharpness = 0.0f;

                  if (RCAS->CreateBufferResource(Device, nextOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
                  {
                      RCAS->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                      return RCAS->Buffer();
                  }
                  return nullptr;
              },

              // Dispatch
              [&](ID3D12Resource* input, ID3D12Resource* output) -> bool
              {
                  if (!RCAS->CanRender() || !paramMotion || !paramOutput)
                      return true;

                  RCAS->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                  RcasConstants rcasConstants {};

                  rcasConstants.Sharpness = _actualSharpness.value_or(_sharpness);
                  rcasConstants.DepthIsLinear = DepthLinear();
                  rcasConstants.DepthIsReversed = DepthInverted();
                  rcasConstants.IsHdr = IsHdr();

                  // Restore value
                  _sharpness = _actualSharpness.value_or(_sharpness);
                  _actualSharpness.reset();

                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &rcasConstants.MvScaleX);
                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &rcasConstants.MvScaleY);

                  float nearPlane = 0.0f;
                  float farPlane = 0.0f;

                  // We need camera near and far for DLSSD
                  // We passthrough those values from the DLSSG params onto the upscaler's params
                  if (InParameters->Get("DLSSG.CameraNear", &nearPlane) == NVSDK_NGX_Result_Success &&
                      InParameters->Get("DLSSG.CameraFar", &farPlane) == NVSDK_NGX_Result_Success)
                  {
                      rcasConstants.CameraNear = nearPlane;
                      rcasConstants.CameraFar = farPlane;
                  }
                  else
                  {
                      rcasConstants.CameraNear = Config::Instance()->FsrCameraNear.value_or_default();
                      rcasConstants.CameraFar = Config::Instance()->FsrCameraFar.value_or_default();
                  }

                  if (!RCAS->Dispatch(InCommandList, input, paramMotion, rcasConstants, output, paramDepth))
                  {
                      Config::Instance()->RcasEnabled.set_volatile_value(false);
                      return false;
                  }
                  return true;
              } });
    }

    if (Magnifier->ShouldRun())
    {
        pipeline.push_back(
            { // Setup
              [&](ID3D12Resource* nextOutput) -> ID3D12Resource*
              {
                  if (Magnifier->CreateBufferResource(Device, nextOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
                  {
                      Magnifier->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                      return Magnifier->Buffer();
                  }

                  return nullptr;
              },

              // Dispatch
              [&](ID3D12Resource* input, ID3D12Resource* output) -> bool
              {
                  if (!Magnifier->CanRender() || !paramMotion || !paramOutput)
                      return true;

                  Magnifier->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                  return Magnifier->Dispatch(InCommandList, input, output);
              } });
    }

    // Iterate BACKWARDS to establish where each shader needs to pull its input from
    ID3D12Resource* currentTarget = paramOutput;
    for (auto it = pipeline.rbegin(); it != pipeline.rend(); ++it)
    {
        ID3D12Resource* requiredInput = it->Setup(currentTarget);
        if (requiredInput)
        {
            it->outputBuffer = currentTarget;
            it->inputBuffer = requiredInput;
            currentTarget = requiredInput; // Shift the target back for the next previous stage
        }
    }

    // Upscaler will write to the first active shader, or just output
    InParameters->Set(NVSDK_NGX_Parameter_Output, currentTarget);


#if OPTI_DLSSNR
    /*
     * The reordered arrangement: the model runs on the upscaler's own input, at
     * render resolution, and Super Resolution then enlarges its result.
     *
     * Placed here rather than after EvaluateInternal because that is the whole
     * point of the mode -- the model sees a frame that has not been magnified,
     * and the upscaler's temporal accumulation then works on enhanced pixels
     * instead of the model re-deciding detail on every enlarged frame.
     *
     * The feature was created with IsHDR and AutoExposure cleared to match, in
     * SetInitParameters. Colour is named explicitly because Output at this point
     * is a pipeline buffer the upscaler has not written yet.
     */
    if (NRBuiltMode() == DlssNr::Mode::UpscaleWithSR)
    {
        ID3D12Resource* paramColor = nullptr;
        InParameters->Get(NVSDK_NGX_Parameter_Color, &paramColor);

        if (paramColor != nullptr)
        {
            /*
             * The pass writes into a buffer of its own here, not into the game's colour buffer. A game
             * creates that as a render target and a shader resource; it does not generally allow
             * unordered access, so the codec's view over it cannot be created and its writes land
             * nowhere defined -- coloured blocks over the frame, worst where the values are largest.
             *
             * The upscaler is then pointed at what came back, which is the enhanced frame.
             */
            // Super Resolution magnifies the model's work straight after this, so the detail band is
            // compensated for that ratio the same way the multi-pass chain compensates for its own.
            DlssNr::SetEnlargementRatio(RenderWidth() == 0
                                            ? 1.0f
                                            : (float) TargetWidth() / (float) RenderWidth());

            ID3D12Resource* enhanced = DlssNr::EvaluateAfterUpscale(
                InCommandList, InParameters, paramColor, RenderWidth(), RenderHeight(), true);

            if (enhanced != nullptr && enhanced != paramColor)
                InParameters->Set(NVSDK_NGX_Parameter_Color, enhanced);
        }
    }
#endif


#if OPTI_DLSSNR
    /*
     * Multi-pass Custom runs the first pass below the game's render resolution, so its inputs have to
     * be reduced to match. Without this the game's full-size buffers go over unchanged while the
     * feature is told they are smaller, and DLSS reads the top-left corner of each -- a crop, not a
     * reduction, and the frame arrives as a magnified corner of itself.
     *
     * Before EvaluateInternal, because it rewrites the parameter block the first pass is about to read.
     */
    if (NRFeature1IsResampled())
    {
        unsigned int f1Width = 0;
        unsigned int f1Height = 0;
        NRFeature1Size(f1Width, f1Height);

        DlssNr::ResampleFeature1Inputs(InCommandList, InParameters, NRSourceWidth(), NRSourceHeight(),
                                       f1Width, f1Height);
    }
    else
    {
        /*
         * The first pass takes the game's sequence untouched here, so this is where it gets watched.
         *
         * Only in the else branch: the resample records the offsets it has just rescaled, which are
         * the ones that pass will actually see, and observing the same value twice a frame would make
         * the counter believe the sequence had cycled when it has only been read twice.
         */
        float f1JitterX = 0.0f;
        float f1JitterY = 0.0f;
        InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &f1JitterX);
        InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &f1JitterY);
        DlssNr::ObserveJitter(DlssNr::JitterSite::Feature1, f1JitterX, f1JitterY);
    }
#endif
#if OPTI_DLSSNR
    /*
     * One line in the log saying what this frame is actually built out of.
     *
     * Emitted here because this is the last point before the first pass runs at which every fact is
     * settled -- the parameter block is final, the resample has happened if it was going to, and the
     * arrangement cannot change again this frame. Re-emitted only when the structure changes, so a
     * placement change mid-session is visible and a steady session costs one line.
     *
     * The point of it is support. Every question asked about this feature so far has been answerable
     * from data the code already had at the time, and asking for it one round trip at a time is how a
     * five-minute diagnosis becomes a week.
     */
    if (DlssNr::IsRunning() || NRBuiltMode() != DlssNr::Mode::PostProcess)
    {
        DlssNr::report::Integration integration {};
        integration.backend = "D3D12";
        integration.gameHdr = NRGameIsHdr();
        integration.mvLowRes = LowResMV();
        integration.mvJittered = JitteredMV();
        integration.depthInverted = DepthInverted();
        integration.scaleFactor =
            NRSourceWidth() != 0 ? (float) DisplayWidth() / (float) NRSourceWidth() : 1.0f;

        const bool firstPassIsRr = GetUpscalerType() == Upscaler::DLSSD;
        unsigned int preset = 0;
        const bool hasPreset = DlssNr::PresetForQuality(InParameters, (int) PerfQualityValue(), preset);

        unsigned int distinct = 0;
        unsigned int outOfBounds = 0;
        bool settled = false;

        switch (NRBuiltMode())
        {
        case DlssNr::Mode::UpscaleWithSR:
        {
            integration.topology = "nr>sr";

            DlssNr::report::Pass nr {};
            nr.name = "NR";
            nr.inW = nr.outW = RenderWidth();
            nr.inH = nr.outH = RenderHeight();
            nr.hdr = NRGameIsHdr();
            nr.jitter = false; // the model takes motion vectors and a reset, never offsets
            integration.add(nr);

            DlssNr::report::Pass sr {};
            sr.name = "SR";
            sr.inW = RenderWidth();
            sr.inH = RenderHeight();
            sr.outW = TargetWidth();
            sr.outH = TargetHeight();
            sr.hasPreset = hasPreset;
            sr.preset = preset;
            sr.hdr = IsHdr();
            sr.exposure = AutoExposure();
            DlssNr::JitterStats(DlssNr::JitterSite::Feature1, distinct, settled, outOfBounds);
            sr.phases = distinct;
            sr.phasesSettled = settled;
            sr.phasesWanted = DlssNr::jitter::RecommendedPhases(RenderWidth(), RenderHeight(),
                                                                TargetWidth(), TargetHeight());
            integration.add(sr);
            break;
        }

        case DlssNr::Mode::MultiPass:
        case DlssNr::Mode::MultiPassCustom:
        {
            integration.topology = firstPassIsRr
                                       ? (spatialEnlarge ? "rr1>nr>spatial" : "rr1>nr>sr2")
                                       : (spatialEnlarge ? "sr1>nr>spatial" : "sr1>nr>sr2");

            DlssNr::report::Pass first {};
            first.name = firstPassIsRr ? "RR1" : "SR1";
            first.inW = first.outW = RenderWidth();
            first.inH = first.outH = RenderHeight();
            first.hasPreset = hasPreset;
            first.preset = preset;
            first.hdr = IsHdr();

            /*
             * The RR guide states exposure, auto-exposure and sharpness are not supported by Ray
             * Reconstruction and tells integrators to ignore the corresponding sections outright. So
             * an RR first pass reporting exposure=1 here is a defect, not a configuration -- which is
             * exactly the kind of thing that degrades quality without ever failing.
             */
            first.exposure = !firstPassIsRr && AutoExposure();
            DlssNr::JitterStats(DlssNr::JitterSite::Feature1, distinct, settled, outOfBounds);
            first.phases = distinct;
            first.phasesSettled = settled;
            first.phasesWanted =
                firstPassIsRr ? DlssNr::jitter::RecommendedPhasesRr(RenderWidth(), RenderHeight(),
                                                                    RenderWidth(), RenderHeight())
                              : DlssNr::jitter::RecommendedPhases(RenderWidth(), RenderHeight(),
                                                                  RenderWidth(), RenderHeight());
            integration.add(first);

            DlssNr::report::Pass nr {};
            nr.name = "NR";
            nr.inW = nr.outW = RenderWidth();
            nr.inH = nr.outH = RenderHeight();
            nr.hdr = NRGameIsHdr();
            nr.jitter = false;
            integration.add(nr);

            DlssNr::report::Pass last {};
            last.name = spatialEnlarge ? "SPATIAL" : "SR2";
            last.inW = RenderWidth();
            last.inH = RenderHeight();
            last.outW = DisplayWidth();
            last.outH = DisplayHeight();
            last.hdr = false; // created with IsHDR cleared; it reads a display-referred picture
            /*
             * The enlargement always gets the real offsets now: the edit transfer is no longer
             * optional, so what it reads is always the game's jittered frame carrying the model's
             * work. The only way it sees zeros is the spatial filter, which asks for none.
             */
            last.jitter = !spatialEnlarge && NRFinalPassForwardsJitter(true);

            if (!spatialEnlarge)
            {
                DlssNr::JitterStats(DlssNr::JitterSite::Final, distinct, settled, outOfBounds);
                last.phases = distinct;
                last.phasesSettled = settled;

                /*
                 * Only asked for when the offsets are being forwarded. When they are deliberately
                 * zeroed the pass has one phase on purpose, and holding that against the guide's
                 * recommendation would be reporting the design as a fault.
                 */
                if (last.jitter)
                    last.phasesWanted = DlssNr::jitter::RecommendedPhases(
                        RenderWidth(), RenderHeight(), DisplayWidth(), DisplayHeight());
            }

            integration.add(last);
            break;
        }

        default:
            integration.topology = "post";

            DlssNr::report::Pass sr {};
            sr.name = "SR";
            sr.inW = RenderWidth();
            sr.inH = RenderHeight();
            sr.outW = TargetWidth();
            sr.outH = TargetHeight();
            sr.hasPreset = hasPreset;
            sr.preset = preset;
            sr.hdr = IsHdr();
            sr.exposure = AutoExposure();
            DlssNr::JitterStats(DlssNr::JitterSite::Feature1, distinct, settled, outOfBounds);
            sr.phases = distinct;
            sr.phasesSettled = settled;
            sr.phasesWanted = DlssNr::jitter::RecommendedPhases(RenderWidth(), RenderHeight(),
                                                                TargetWidth(), TargetHeight());
            integration.add(sr);

            DlssNr::report::Pass nr {};
            nr.name = "NR";
            nr.inW = nr.outW = TargetWidth();
            nr.inH = nr.outH = TargetHeight();
            nr.hdr = NRGameIsHdr();
            nr.jitter = false;
            integration.add(nr);
            break;
        }

        DlssNr::LogIntegration(integration);
    }
#endif

    UpscalerTime->Start(InCommandList);

    auto evalResult = EvaluateInternal(InCommandList, InParameters);

    UpscalerTime->End(InCommandList);

    if (!evalResult)
        return false;

    // Iterate FORWARDS to execute the shaders in the defined order
    for (auto& pass : pipeline)
    {
        if (pass.inputBuffer && pass.outputBuffer)
        {
            if (!pass.Dispatch(pass.inputBuffer, pass.outputBuffer))
            {
                return true;
            }
        }
    }

    // imgui
    if (!Config::Instance()->OverlayMenu.value_or_default() && _frameCount > 30)
    {
        if (Imgui != nullptr && Imgui.get() != nullptr)
        {
            if (Imgui->IsHandleDifferent())
            {
                Imgui.reset();
            }
            else
                Imgui->Render(InCommandList, paramOutput);
        }
        else
        {
            if (Imgui == nullptr || Imgui.get() == nullptr)
                Imgui = std::make_unique<Menu_Dx12>(GetForegroundWindow(), Device);
        }
    }

    InParameters->Set(NVSDK_NGX_Parameter_Output, paramOutput);

    return evalResult;
}

std::optional<double> IFeature_Dx12::ReadUpscalerTime(void* commandQueueVoid)
{
    ID3D12CommandQueue* commandQueue = (ID3D12CommandQueue*) commandQueueVoid;

    lastUpscalerTime = UpscalerTime->ReadGpuTime(commandQueue);
    lastRcasTime = RCAS->ReadGpuTime(commandQueue);
    lastOutputScalingTime = OutputScaler->ReadGpuTime(commandQueue);

    return sumOpts(lastUpscalerTime, lastRcasTime, lastOutputScalingTime);
}

void IFeature_Dx12::ReadDetailedGpuTimes(void* commandQueueVoid, std::vector<DetailedGpuTime>& detailedGpuTimes)
{
    ID3D12CommandQueue* commandQueue = (ID3D12CommandQueue*) commandQueueVoid;

    detailedGpuTimes.clear();

    // Do not call ReadGpuTime twice for shaders
    if (lastUpscalerTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { ShortName(), lastUpscalerTime.value(), true });

    if (lastRcasTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { RCAS->Name(), lastRcasTime.value(), true });

    if (lastOutputScalingTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { OutputScaler->Name(), lastOutputScalingTime.value(), true });

    auto magnifierTime = Magnifier->ReadGpuTime(commandQueue);

    if (magnifierTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { Magnifier->Name(), magnifierTime.value(), false });
}

IFeature_Dx12::IFeature_Dx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters) {}

IFeature_Dx12::~IFeature_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;

    Imgui.reset();
    OutputScaler.reset();
    RCAS.reset();
    Bias.reset();
}
