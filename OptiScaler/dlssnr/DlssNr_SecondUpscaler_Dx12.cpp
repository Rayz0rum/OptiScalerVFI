#include <pch.h>

#include "DlssNr_Switch.h"

#if OPTI_DLSSNR


#include "DlssNr_SecondUpscaler_Dx12.h"
#include "DlssNr_Dx12.h"

#include <Config.h>
#include <State.h>
#include <proxies/NVNGX_Proxy.h>

#include <nvsdk_ngx_defs.h>

DlssNr_SecondUpscaler_Dx12::DlssNr_SecondUpscaler_Dx12(std::string name, ID3D12Device* device)
    : _name(std::move(name)), _device(device)
{
    if (device != nullptr)
        GpuTime = std::make_unique<GpuTime_Dx12>(device);
}

DlssNr_SecondUpscaler_Dx12::~DlssNr_SecondUpscaler_Dx12() { Release(); }

bool DlssNr_SecondUpscaler_Dx12::CreateInputBuffer(ID3D12Resource* reference, uint32_t renderWidth, uint32_t renderHeight)
{
    if (_device == nullptr || reference == nullptr || renderWidth == 0 || renderHeight == 0)
        return false;

    if (_inputBuffer != nullptr)
    {
        auto existing = _inputBuffer->GetDesc();
        if (existing.Width == renderWidth && existing.Height == renderHeight)
            return true;

        _inputBuffer->Release();
        _inputBuffer = nullptr;
    }

    auto desc = reference->GetDesc();
    desc.Width = renderWidth;
    desc.Height = renderHeight;

    // Written by the NR pass, read by this feature.
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heapProperties;
    D3D12_HEAP_FLAGS heapFlags;

    if (reference->GetHeapProperties(&heapProperties, &heapFlags) != S_OK)
        return false;

    HRESULT hr = _device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                  IID_PPV_ARGS(&_inputBuffer));

    if (hr != S_OK)
    {
        LOG_ERROR("{}: input buffer CreateCommittedResource: {:X}", _name, (UINT64) hr);
        _inputBuffer = nullptr;
        return false;
    }

    _inputBufferState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return true;
}


bool DlssNr_SecondUpscaler_Dx12::CreateEditBuffers(ID3D12Resource* reference, uint32_t renderWidth,
                                                   uint32_t renderHeight)
{
    if (_device == nullptr || reference == nullptr || renderWidth == 0 || renderHeight == 0)
        return false;

    for (auto** slot : { &_editBefore, &_editResult })
    {
        if (*slot != nullptr)
        {
            auto existing = (*slot)->GetDesc();

            if (existing.Width == renderWidth && existing.Height == renderHeight)
                continue;

            (*slot)->Release();
            *slot = nullptr;
        }

        auto desc = reference->GetDesc();
        desc.Width = renderWidth;
        desc.Height = renderHeight;
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        D3D12_HEAP_PROPERTIES heapProperties;
        D3D12_HEAP_FLAGS heapFlags;

        if (reference->GetHeapProperties(&heapProperties, &heapFlags) != S_OK)
            return false;

        if (_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                             IID_PPV_ARGS(slot)) != S_OK)
        {
            *slot = nullptr;
            return false;
        }
    }

    return true;
}
void DlssNr_SecondUpscaler_Dx12::SetInputBufferState(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES state)
{
    if (_inputBuffer == nullptr || cmdList == nullptr || _inputBufferState == state)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = _inputBuffer;
    barrier.Transition.StateBefore = _inputBufferState;
    barrier.Transition.StateAfter = state;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    _inputBufferState = state;
}

void DlssNr_SecondUpscaler_Dx12::Release()
{
    for (auto** res : { &_inputBuffer, &_editBefore, &_editResult, &_exposure, &_exposureUpload })
    {
        if (*res != nullptr)
        {
            (*res)->Release();
            *res = nullptr;
        }
    }

    _exposureUploaded = false;

    if (_handle != nullptr)
    {
        if (auto release = NVNGXProxy::D3D12_ReleaseFeature())
            release(_handle);

        _handle = nullptr;
    }

    if (_params != nullptr)
    {
        if (auto destroy = NVNGXProxy::D3D12_DestroyParameters())
            destroy(_params);

        _params = nullptr;
    }

    _renderWidth = 0;
    _renderHeight = 0;
    _displayWidth = 0;
    _displayHeight = 0;
    _perfQuality = -1;
}

bool DlssNr_SecondUpscaler_Dx12::EnsureCreated(ID3D12GraphicsCommandList* cmdList, uint32_t renderWidth,
                                           uint32_t renderHeight, uint32_t displayWidth, uint32_t displayHeight,
                                           int perfQuality, bool depthInverted, bool jitteredMV,
                                           bool lowResMV, int preset, bool isHdr, bool autoExposure)
{
    if (_createFailed || cmdList == nullptr || renderWidth == 0 || displayWidth == 0)
        return false;

    bool geometryMatches = _handle != nullptr && _renderWidth == renderWidth && _renderHeight == renderHeight &&
                           _displayWidth == displayWidth && _displayHeight == displayHeight &&
                           _perfQuality == perfQuality && _depthInverted == depthInverted &&
                           _jitteredMV == jitteredMV && _lowResMV == lowResMV && _isHdr == isHdr &&
                           _autoExposure == autoExposure;

    if (geometryMatches)
        return true;

    auto createFeature = NVNGXProxy::D3D12_CreateFeature();
    auto allocParams = NVNGXProxy::D3D12_AllocateParameters();

    if (createFeature == nullptr || allocParams == nullptr)
    {
        LOG_WARN("{}: NGX has no D3D12 CreateFeature/AllocateParameters", _name);
        _createFailed = true;
        return false;
    }

    // Rebuild rather than mutate: the flags below are fixed at creation.
    if (_handle != nullptr)
        Release();

    if (_params == nullptr && (allocParams(&_params) != NVSDK_NGX_Result_Success || _params == nullptr))
    {
        LOG_ERROR("{}: could not allocate a parameter block", _name);
        _createFailed = true;
        return false;
    }

    /*
     * IsHDR and AutoExposure follow what the game declared, because what this feature receives is
     * the game's own colour space.
     *
     * They used to be cleared unconditionally, on the premise that Neural Rendering hands this pass a
     * tone-mapped picture. It does not. The codec's resolve ends by multiplying back out of the
     * normalised space it worked in, so the pass returns the frame in whatever space it was given --
     * at zero strength, bit for bit what went in. And the edit transfer goes further still: it
     * applies a brightness ratio to the game's own jittered buffer, so what arrives here is quite
     * literally the game's frame with a near-unity multiplier on it.
     *
     * Declaring that display-referred selects the programming guide's LDR path, which quantises to
     * 8 bits and expects a perceptually linear encoding. Handing it linear colour instead is the
     * guide's own description of banding and colour shifting, and is the most likely reason the
     * model's colours stop matching the upscaler's.
     */
    /*
     * Forwarded from what the game declared, never assumed.
     *
     * This flag was hardcoded on, which is wrong for any title that supplies display-resolution
     * motion vectors -- Borderlands 4 reports LowResMV false. DLSS then reads a display-sized vector
     * field as though it were render-sized, and reprojects history by a factor that grows with
     * distance from the origin: the whole frame smears radially the moment the camera moves.
     *
     * The first pass gets the game's flags untouched, so the two passes now agree about what the
     * vectors they share actually are.
     */
    int flags = 0;

    if (lowResMV)
        flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;

    if (depthInverted)
        flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

    if (jitteredMV)
        flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;

    if (isHdr)
        flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;

    if (autoExposure)
        flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

    _params->Set(NVSDK_NGX_Parameter_Width, renderWidth);
    _params->Set(NVSDK_NGX_Parameter_Height, renderHeight);
    _params->Set(NVSDK_NGX_Parameter_OutWidth, displayWidth);
    _params->Set(NVSDK_NGX_Parameter_OutHeight, displayHeight);
    _params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
    _params->Set(NVSDK_NGX_Parameter_PerfQualityValue, perfQuality);
    _params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1);
    _params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
    _params->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);

    /*
     * Put on the same preset as the first pass, when one is known.
     *
     * Leaving it unset is not neutral. The driver then chooses independently from this feature's own
     * ratio, so the two Super Resolution passes in the chain can end up on presets that disagree
     * about the frame -- and the guide attaches behaviour to that choice rather than only quality.
     * Exposure input is supported by Presets J and K alone, and Preset L always uses AutoExposure;
     * this feature deliberately clears AutoExposure and binds an identity exposure texture, so a
     * driver landing it on L would ignore that texture and auto-expose an already-normalised picture.
     */
    if (preset >= 0)
    {
        if (const char* presetKey = DlssNr::PresetKeyForQuality(perfQuality); presetKey != nullptr)
            _params->Set(presetKey, (unsigned int) preset);
    }

    NVSDK_NGX_Result result =
        createFeature(cmdList, NVSDK_NGX_Feature_SuperSampling, _params, &_handle);

    if (result != NVSDK_NGX_Result_Success || _handle == nullptr)
    {
        LOG_ERROR("{}: CreateFeature failed: 0x{:X}", _name, (unsigned) result);
        _handle = nullptr;
        _createFailed = true;
        return false;
    }

    _renderWidth = renderWidth;
    _renderHeight = renderHeight;
    _displayWidth = displayWidth;
    _displayHeight = displayHeight;
    _perfQuality = perfQuality;
    _depthInverted = depthInverted;
    _jitteredMV = jitteredMV;
    _lowResMV = lowResMV;
    _isHdr = isHdr;
    _autoExposure = autoExposure;
    _needsReset = true;

    LOG_INFO("{}: created, {}x{} -> {}x{}, IsHDR {}, exposure {}, motion vectors at {} (as "
             "the game declared them), depth {}, vectors {}, preset {}",
             _name, renderWidth, renderHeight, displayWidth, displayHeight, isHdr ? "set" : "cleared",
             autoExposure ? "automatic" : "supplied",
             lowResMV ? "render resolution" : "display resolution", depthInverted ? "inverted" : "normal",
             jitteredMV ? "jittered" : "not jittered",
             preset >= 0 ? std::to_string(preset) : std::string("left to the driver -- it may not match "
                                                                "the first pass"));

    return true;
}

ID3D12Resource* DlssNr_SecondUpscaler_Dx12::IdentityExposure(ID3D12GraphicsCommandList* cmdList)
{
    if (_exposure != nullptr && _exposureUploaded)
        return _exposure;

    if (_device == nullptr || cmdList == nullptr)
        return nullptr;

    if (_exposure == nullptr)
    {
        D3D12_HEAP_PROPERTIES defaultHeap = {};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 1;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        HRESULT hr = _device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_exposure));

        if (hr != S_OK)
        {
            LOG_ERROR("identity exposure CreateCommittedResource: {:X}", (UINT64) hr);
            return nullptr;
        }

        D3D12_HEAP_PROPERTIES uploadHeap = {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        // One row, padded to D3D12's copy alignment.
        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = _device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(&_exposureUpload));

        if (hr != S_OK)
        {
            LOG_ERROR("identity exposure upload heap: {:X}", (UINT64) hr);
            _exposure->Release();
            _exposure = nullptr;
            return nullptr;
        }

        /*
         * Read back constants you assume. The whole point of this texture is
         * that it holds exactly 1.0; if the upload does not land, every image
         * comparison downstream is being made against garbage.
         */
        void* mapped = nullptr;
        D3D12_RANGE noRead = { 0, 0 };

        if (_exposureUpload->Map(0, &noRead, &mapped) == S_OK && mapped != nullptr)
        {
            const float one = 1.0f;
            memcpy(mapped, &one, sizeof(one));
            _exposureUpload->Unmap(0, nullptr);
        }
    }

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = _exposure;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = _exposureUpload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    src.PlacedFootprint.Footprint.Width = 1;
    src.PlacedFootprint.Footprint.Height = 1;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = _exposure;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    _exposureUploaded = true;

    LOG_INFO("{}: identity exposure uploaded (1x1 R32_SFLOAT = 1.0)", _name);

    return _exposure;
}

bool DlssNr_SecondUpscaler_Dx12::Evaluate(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* color,
                                      ID3D12Resource* output, ID3D12Resource* depth, ID3D12Resource* mvec,
                                      ID3D12Resource* exposure, float jitterX, float jitterY, float mvScaleX,
                                      float mvScaleY, bool reset)
{
    if (_handle == nullptr || _params == nullptr || cmdList == nullptr)
        return false;

    if (color == nullptr || output == nullptr || depth == nullptr || mvec == nullptr)
        return false;

    // The upscaler cannot write its own input.
    if (color == output)
    {
        LOG_WARN("{}: colour and output are the same resource", _name);
        return false;
    }

    auto evaluate = NVNGXProxy::D3D12_EvaluateFeature();
    if (evaluate == nullptr)
        return false;

    _params->Set(NVSDK_NGX_Parameter_Color, color);
    _params->Set(NVSDK_NGX_Parameter_Output, output);
    _params->Set(NVSDK_NGX_Parameter_Depth, depth);
    _params->Set(NVSDK_NGX_Parameter_MotionVectors, mvec);

    /*
     * An exposure is only needed where the feature has no other source for one.
     *
     * With AutoExposure set, DLSS estimates it from the frame and a supplied texture is redundant.
     * With it cleared, supplying nothing leaves the feature no exposure source at all and the frame
     * resolves toward black -- so the identity is not a nicety there, it is what keeps the picture.
     * The caller's own texture wins over both when it has one.
     */
    if (exposure == nullptr && !_autoExposure)
        exposure = IdentityExposure(cmdList);

    if (exposure != nullptr)
        _params->Set(NVSDK_NGX_Parameter_ExposureTexture, exposure);

    /*
     * The jitter offsets, decided by the caller.
     *
     * Normally zero: the first pass ran at 1:1 with the game's sequence and resolved it, so what
     * arrives here is grid-aligned and a per-frame offset would describe a subpixel displacement the
     * image no longer has -- shimmer at the period of the jitter sequence, which is what the first
     * pass was there to remove.
     *
     * Not zero when the game declares MVJittered, because then these values are also what DLSS
     * cancels the baked offset in the vectors with. See IFeature::NRFinalPassForwardsJitter.
     */
    _params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, jitterX);
    _params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, jitterY);
    _params->Set(NVSDK_NGX_Parameter_MV_Scale_X, mvScaleX);
    _params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, mvScaleY);

    _params->Set(NVSDK_NGX_Parameter_Reset, reset ? 1 : 0);
    _params->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);

    // The subrect the upscaler should read. Render resolution, since this
    // feature sits behind a pass that ran at render resolution.
    _params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, _renderWidth);
    _params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, _renderHeight);

    GpuTime->Start(cmdList);

    NVSDK_NGX_Result result = evaluate(cmdList, _handle, _params, nullptr);

    GpuTime->End(cmdList);

    if (result != NVSDK_NGX_Result_Success)
    {
        LOG_ERROR("{}: EvaluateFeature failed: 0x{:X}", _name, (unsigned) result);
        return false;
    }

    return true;
}

#endif // OPTI_DLSSNR
