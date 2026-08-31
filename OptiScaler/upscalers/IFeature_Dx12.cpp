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
        else if (SecondUpscaler == nullptr)
        {
            SecondUpscaler = std::make_unique<DlssNr_SecondUpscaler_Dx12>("DLSS-NR Second Upscaler", Device);
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

                  const bool transferEdit =
                      !spatialEnlarge &&
                      Config::Instance()->DlssNrMultiPassJitter.value_or_default() != 0 &&
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
                          float alignX = 0.0f;
                          float alignY = 0.0f;
                          InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &alignX);
                          InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &alignY);

                          const float alignSign =
                              (float) Config::Instance()->DlssNrMultiPassAlign.value_or_default();

                          DlssNr::TransferEditOntoJittered(InCommandList, SecondUpscaler->EditBefore(),
                                                           input, paramColor, SecondUpscaler->EditResult(),
                                                           RenderWidth(), RenderHeight(),
                                                           alignX * alignSign, alignY * alignSign);
                          enlargeSource = SecondUpscaler->EditResult();
                      }
                  }



                  if (!SecondUpscaler->EnsureCreated(InCommandList, RenderWidth(), RenderHeight(),
                                                     DisplayWidth(), DisplayHeight(),
                                                     (int) PerfQualityValue(), DepthInverted(), JitteredMV(),
                                                     LowResMV()))
                  {
                      // Without the enlargement the frame would be a render-resolution
                      // image in the corner of a display-resolution buffer, so the mode
                      // stands down rather than showing that.
                      Config::Instance()->DlssNrMode.set_volatile_value((uint32_t) DlssNr::Mode::PostProcess);
                      State::Instance().changeBackend[Handle()->Id] = true;
                      return false;
                  }

                  SecondUpscaler->SetInputBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                  float mvScaleX = 1.0f;
                  float mvScaleY = 1.0f;
                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX);
                  InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY);

                  /*
                   * The first pass was a straight forward of whatever the game asked for -- its create
                   * flags, its motion vector scales and its jitter sequence all untouched. The
                   * divergence is here, and only here.
                   */
                  float jitterX = 0.0f;
                  float jitterY = 0.0f;

                  if (NRFinalPassForwardsJitter())
                  {
                      InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitterX);
                      InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY);
                  }

                  return SecondUpscaler->Evaluate(InCommandList, enlargeSource, output, paramDepth, paramMotion,
                                                  nullptr, jitterX, jitterY, mvScaleX, mvScaleY,
                                                  SecondUpscaler->ConsumeResetFlag());
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
