#include <pch.h>

#include "DlssNr_Switch.h"

#if OPTI_DLSSNR

#include "DlssNr.h"
#include "DlssNr_Codec_Dx11.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <proxies/NVNGX_Proxy.h>
#include <gpu_time/GpuTime_Dx11.h>

#include <mutex>

// DLSS 5 Neural Rendering on native Direct3D 11.
//
// The D3D12 module is the reference for this one and the two follow the same sequence: encode the
// frame into a display-referred proxy, optionally shrink it, run the model, compose the answer back
// over the untouched original. Only the API underneath differs.
//
// Native rather than bridged. OptiScaler can already carry a D3D11 game onto D3D12 and reach the model
// that way, but that route exists to give D3D11 games a D3D12 upscaler, and it costs a shared-resource
// round trip per frame. The snippet exports a complete D3D11 surface -- CreateFeature, EvaluateFeature
// and the rest are all present in the shipped DLL -- so a D3D11 game can drive the model on its own
// device, and one that is already using a native D3D11 upscaler never has to touch D3D12 at all.
//
// What is genuinely simpler here, and why this file is shorter than its D3D12 twin despite doing the
// same work:
//
//   - No resource states. D3D11 inserts its own hazard tracking, so every Barrier() pair is gone.
//   - No descriptor heap. Views are objects, cached by resource pointer in the codec.
//   - No deferred-destruction problem for textures. The D3D11 runtime keeps a reference until the GPU
//     is done, so scratch surfaces can simply be released. The model's own allocations are not the
//     runtime's, so the feature is still parked rather than freed.

namespace
{

using PFN_NrCreate11 = void*(__cdecl*) (const wchar_t*, const wchar_t*, ID3D11Device*, ID3D11DeviceContext*,
                                        void*, unsigned int, unsigned int, int, float, int, float, float,
                                        float, int, int);
using PFN_NrEvaluate11 = int(__cdecl*) (ID3D11DeviceContext*, void*, void*, ID3D11Resource*,
                                        ID3D11Resource*, ID3D11Resource*, ID3D11Resource*, unsigned int,
                                        unsigned int, unsigned int, unsigned int, int, int, float, int,
                                        float, float, float, int, float, float);
using PFN_NrRelease11 = void(__cdecl*) (void*);
using PFN_NrSetFloatSlot = void(__cdecl*) (int);
using PFN_NrProbeFloat = void(__cdecl*) (void*, const char*, float, int);

struct NrState11
{
    HMODULE forwarder = nullptr;
    PFN_NrCreate11 create = nullptr;
    PFN_NrEvaluate11 evaluate = nullptr;
    PFN_NrRelease11 release = nullptr;
    PFN_NrSetFloatSlot setFloatSlot = nullptr;
    PFN_NrProbeFloat probeFloat = nullptr;
    bool floatSlotKnown = false;
    int* lastInit = nullptr;
    int* lastCreate = nullptr;

    NVSDK_NGX_Parameter* capabilityParams = nullptr;
    void* feature = nullptr;

    // The model cannot read and write one resource, so the frame is staged through these.
    ID3D11Texture2D* colorCopy = nullptr;
    ID3D11Texture2D* output = nullptr;

    // The frame exactly as the upscaler wrote it, which is what the resolve composes against.
    ID3D11Texture2D* hdrCopy = nullptr;

    // The proxy shrunk for the model, when it works below full resolution.
    ID3D11Texture2D* colorSmall = nullptr;

    unsigned int workWidth = 0;
    unsigned int workHeight = 0;

    unsigned int width = 0;
    unsigned int height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool reset = true;

    bool guideDepthInverted = false;
    float guideMvScaleX = 1.0f;
    float guideMvScaleY = 1.0f;

    // The values the live feature was created with, and when a difference was first seen.
    unsigned int builtPreset = 0;
    float builtIntensity = 0.0f;
    unsigned int builtStyle = 0;
    float builtLocalStructure = 0.0f;
    float builtLocalTone = 0.0f;
    float builtSkinStructure = 0.0f;
    bool builtAutoMask = false;
    unsigned long long settledAt = 0;

    bool failed = false;
    const char* reason = "";
};

NrState11 g_nr;
codec::Codec11 g_codec;
std::mutex g_nrMutex;
std::filesystem::path g_dllDir;
unsigned long long g_frames = 0;
std::unique_ptr<GpuTime_Dx11> g_gpuTime;
std::optional<double> g_lastGpuTime;

// A feature the model still owns work for. Freeing one under the GPU loses the device, and unlike a
// texture the D3D11 runtime knows nothing about it, so it cannot defer the release on our behalf.
struct Retired11
{
    void* feature = nullptr;
    unsigned long long freeAtFrame = 0;
};

std::vector<Retired11> g_retired;
constexpr unsigned long long kParkFrames = 32;

// Tuning changes are debounced: the driver latches these at create time, so acting on every
// intermediate value as a slider moves would rebuild the feature dozens of times and exhaust the
// latches, after which the model stops responding until the process restarts.
constexpr unsigned long long kSettleFrames = 30;

void ParkFeature(void*& feature)
{
    if (feature == nullptr)
        return;

    g_retired.push_back({ feature, g_frames + kParkFrames });
    feature = nullptr;
}

void TickRetired()
{
    for (auto it = g_retired.begin(); it != g_retired.end();)
    {
        if (g_frames < it->freeAtFrame)
        {
            ++it;
            continue;
        }

        if (it->feature != nullptr && g_nr.release != nullptr)
            g_nr.release(it->feature);

        it = g_retired.erase(it);
    }
}

void ReleaseTexture(ID3D11Texture2D*& tex)
{
    if (tex == nullptr)
        return;

    // The codec caches views by resource pointer, and the next allocation may well land on this
    // address, so the views naming it have to go with it.
    g_codec.forget(tex);
    tex->Release();
    tex = nullptr;
}

ID3D11Texture2D* CreateScratch(ID3D11Device* device, DXGI_FORMAT format, unsigned int width,
                               unsigned int height)
{
    if (device == nullptr || width == 0 || height == 0)
        return nullptr;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = codec::TypedFormat11(format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    ID3D11Texture2D* tex = nullptr;

    if (FAILED(device->CreateTexture2D(&desc, nullptr, &tex)))
        return nullptr;

    return tex;
}

void ReleaseSurfaces()
{
    ReleaseTexture(g_nr.colorCopy);
    ReleaseTexture(g_nr.hdrCopy);
    ReleaseTexture(g_nr.output);
    ReleaseTexture(g_nr.colorSmall);
}

bool EnsureForwarder()
{
    if (g_nr.forwarder != nullptr)
        return g_nr.create != nullptr;

    if (g_dllDir.empty())
        g_dllDir = Util::DllPath().remove_filename();

    auto found = Util::FindFilePath(g_dllDir, "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
        found = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll not found beside OptiScaler ({}) or the game executable",
                  g_dllDir.string());
        g_nr.reason = "nvngx.dll_dlssnr.dll is missing";
        return false;
    }

    const auto path = found.value();
    g_nr.forwarder = LoadLibraryW(path.wstring().c_str());

    if (g_nr.forwarder == nullptr)
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll found at {} but would not load, error {}", path.string(),
                  GetLastError());
        g_nr.reason = "nvngx.dll_dlssnr.dll would not load";
        return false;
    }

    g_nr.create = (PFN_NrCreate11) GetProcAddress(g_nr.forwarder, "dlssnr_call_create_d3d11");
    g_nr.evaluate = (PFN_NrEvaluate11) GetProcAddress(g_nr.forwarder, "dlssnr_call_evaluate_d3d11");
    g_nr.release = (PFN_NrRelease11) GetProcAddress(g_nr.forwarder, "dlssnr_call_release_d3d11");
    g_nr.setFloatSlot = (PFN_NrSetFloatSlot) GetProcAddress(g_nr.forwarder, "dlssnr_call_set_float_slot");
    g_nr.probeFloat = (PFN_NrProbeFloat) GetProcAddress(g_nr.forwarder, "dlssnr_call_probe_float");
    g_nr.lastInit = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_init");
    g_nr.lastCreate = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_create");

    if (g_nr.create == nullptr || g_nr.evaluate == nullptr)
    {
        // A forwarder built before D3D11 was added has the D3D12 exports and not these, which is worth
        // saying plainly rather than reporting as a generic failure.
        g_nr.reason = "the forwarder has no D3D11 exports; it predates native D3D11 support";
        LOG_ERROR("DLSS-NR: {}", g_nr.reason);
        return false;
    }

    LOG_INFO("DLSS-NR (D3D11) forwarder loaded from {}", path.string());
    return true;
}

void DiscoverFloatSlot(NVSDK_NGX_Parameter* params)
{
    if (g_nr.floatSlotKnown || params == nullptr || g_nr.probeFloat == nullptr ||
        g_nr.setFloatSlot == nullptr)
        return;

    g_nr.floatSlotKnown = true;

    static const char* kProbeKey = "DLSSNR.OptiScalerFloatProbe";
    static const int kCandidates[] = { 1, 2, 5, 6, 7, 4, 3, 0 };
    const float expected = 0.375f; // exact in binary, so the round trip is exact or it is wrong

    for (int slot : kCandidates)
    {
        float readBack = 0.0f;
        g_nr.probeFloat(params, kProbeKey, expected, slot);

        if (params->Get(kProbeKey, &readBack) == NVSDK_NGX_Result_Success && readBack == expected)
        {
            g_nr.setFloatSlot(slot);
            LOG_INFO("DLSS-NR (D3D11) float parameters go through vtable slot {}", slot);
            return;
        }
    }

    LOG_ERROR("DLSS-NR could not find the float setter: intensity, local structure, local tone and skin "
              "structure will have no effect. The uint parameters still apply.");
}

bool EnsureCapabilityParams(ID3D11Device* device)
{
    if (g_nr.capabilityParams != nullptr)
        return true;

    if (!NVNGXProxy::IsDx11Inited() && !NVNGXProxy::InitDx11(device))
    {
        g_nr.reason = "the NGX core would not initialise";
        return false;
    }

    if (NVNGXProxy::D3D11_GetCapabilityParameters() == nullptr)
    {
        g_nr.reason = "the NGX core has no capability parameters";
        return false;
    }

    if (NVNGXProxy::D3D11_GetCapabilityParameters()(&g_nr.capabilityParams) != NVSDK_NGX_Result_Success ||
        g_nr.capabilityParams == nullptr)
    {
        g_nr.capabilityParams = nullptr;
        g_nr.reason = "the NGX core refused its capability parameters";
        return false;
    }

    DiscoverFloatSlot(g_nr.capabilityParams);
    return true;
}

ID3D11Resource* GetResource(NVSDK_NGX_Parameter* params, const char* a, const char* b)
{
    ID3D11Resource* res = nullptr;

    if (params->Get(a, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    res = nullptr;

    if (params->Get(b, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    return nullptr;
}

bool TuningMatchesFeature(const Config& cfg)
{
    return g_nr.builtPreset == cfg.DlssNrPreset.value_or_default() &&
           g_nr.builtIntensity == cfg.DlssNrIntensity.value_or_default() &&
           g_nr.builtStyle == cfg.DlssNrStyle.value_or_default() &&
           g_nr.builtLocalStructure == cfg.DlssNrLocalStructure.value_or_default() &&
           g_nr.builtLocalTone == cfg.DlssNrLocalTone.value_or_default() &&
           g_nr.builtSkinStructure == cfg.DlssNrSkinStructure.value_or_default() &&
           g_nr.builtAutoMask == cfg.DlssNrAutoMask.value_or_default();
}

void RecordBuiltTuning(const Config& cfg)
{
    g_nr.builtPreset = cfg.DlssNrPreset.value_or_default();
    g_nr.builtIntensity = cfg.DlssNrIntensity.value_or_default();
    g_nr.builtStyle = cfg.DlssNrStyle.value_or_default();
    g_nr.builtLocalStructure = cfg.DlssNrLocalStructure.value_or_default();
    g_nr.builtLocalTone = cfg.DlssNrLocalTone.value_or_default();
    g_nr.builtSkinStructure = cfg.DlssNrSkinStructure.value_or_default();
    g_nr.builtAutoMask = cfg.DlssNrAutoMask.value_or_default();
    g_nr.settledAt = 0;
}

} // namespace

namespace DlssNr
{

void EvaluateAfterUpscaleDx11(ID3D11DeviceContext* ctx, NVSDK_NGX_Parameter* params)
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    const Config& cfg = *Config::Instance();

    if (!cfg.DlssNrEnabled.value_or_default() || g_nr.failed || ctx == nullptr || params == nullptr)
        return;

    ID3D11Resource* target = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    ID3D11Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    ID3D11Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    // Without all three there is nothing to run on. Not a failure: some evaluates legitimately carry
    // none of it, so this stays quiet and tries again next frame.
    if (target == nullptr || depth == nullptr || motion == nullptr)
        return;

    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);

    if (device == nullptr)
        return;

    ID3D11Texture2D* targetTex = nullptr;

    if (FAILED(target->QueryInterface(IID_PPV_ARGS(&targetTex))) || targetTex == nullptr)
    {
        device->Release();
        return;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    targetTex->GetDesc(&desc);
    targetTex->Release();

    const unsigned int width = desc.Width;
    const unsigned int height = desc.Height;

    // Depth and motion are the upscaler's inputs and so sit at render resolution while colour and
    // output are at display resolution. The model takes that as a subrect per resource, which is why
    // nothing here resamples anything.
    unsigned int guideWidth = 0;
    unsigned int guideHeight = 0;
    params->Get(NVSDK_NGX_Parameter_Width, &guideWidth);
    params->Get(NVSDK_NGX_Parameter_Height, &guideHeight);

    if (guideWidth == 0 || guideHeight == 0)
    {
        guideWidth = width;
        guideHeight = height;
    }

    unsigned int createFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);
    g_nr.guideDepthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    const bool isHdrBuffer = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;

    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX) != NVSDK_NGX_Result_Success)
        mvScaleX = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY) != NVSDK_NGX_Result_Success)
        mvScaleY = 1.0f;

    // Two factors, and both are needed: the game's own scale turns its vectors into render pixels, and
    // the upscale ratio carries render pixels onto a display-resolution image. They coincide only at
    // native resolution.
    const float upscaleX = guideWidth != 0 ? (float) width / (float) guideWidth : 1.0f;
    const float upscaleY = guideHeight != 0 ? (float) height / (float) guideHeight : 1.0f;
    g_nr.guideMvScaleX = mvScaleX * upscaleX;
    g_nr.guideMvScaleY = mvScaleY * upscaleY;

    if (!EnsureForwarder() || !EnsureCapabilityParams(device))
    {
        g_nr.failed = true;
        LOG_ERROR("DLSS-NR (D3D11) unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }

    // What the model works at. The frame and its edit stay full resolution; only the model's input and
    // answer shrink, and the resolve enlarges the answer while compositing.
    float workScale = cfg.DlssNrWorkingScale.value_or_default();
    workScale = workScale < 0.25f ? 0.25f : (workScale > 1.0f ? 1.0f : workScale);
    const auto workWidth = (unsigned int) (width * workScale + 0.5f);
    const auto workHeight = (unsigned int) (height * workScale + 0.5f);
    const bool reduced = workWidth != width || workHeight != height;

    ++g_frames;
    TickRetired();

    // A format change invalidates the scratch set: a stale one either clamps linear HDR into an 8-bit
    // texture or hands the codec mismatched views, and both fail quietly.
    if (g_nr.format != DXGI_FORMAT_UNKNOWN && g_nr.format != desc.Format)
    {
        ReleaseSurfaces();
        ParkFeature(g_nr.feature);
    }

    g_nr.format = desc.Format;

    // The tuning is latched when the feature is created, so a change means a rebuild -- but only once
    // the value has stopped moving, or dragging a slider would burn through the driver's latches.
    if (g_nr.feature != nullptr && !TuningMatchesFeature(cfg))
    {
        if (g_nr.settledAt == 0)
            g_nr.settledAt = g_frames;

        if (g_frames - g_nr.settledAt >= kSettleFrames)
            ParkFeature(g_nr.feature);
    }
    else
    {
        g_nr.settledAt = 0;
    }

    if (g_nr.feature != nullptr &&
        (g_nr.width != width || g_nr.height != height || g_nr.workWidth != workWidth ||
         g_nr.workHeight != workHeight))
    {
        ReleaseSurfaces();
        ParkFeature(g_nr.feature);
    }

    if (g_nr.output == nullptr)
    {
        g_nr.output = CreateScratch(device, desc.Format, workWidth, workHeight);
        g_nr.colorCopy = CreateScratch(device, desc.Format, width, height);
        g_nr.hdrCopy = CreateScratch(device, desc.Format, width, height);
        g_nr.workWidth = workWidth;
        g_nr.workHeight = workHeight;
    }

    if (reduced && g_nr.colorSmall == nullptr)
        g_nr.colorSmall = CreateScratch(device, desc.Format, workWidth, workHeight);

    if (g_nr.output == nullptr || g_nr.colorCopy == nullptr || g_nr.hdrCopy == nullptr)
    {
        g_nr.failed = true;
        g_nr.reason = "the staging surfaces could not be created";
        LOG_ERROR("DLSS-NR (D3D11) unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }

    if (g_nr.feature == nullptr)
    {
        auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

        if (!snippet.has_value())
            snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

        if (!snippet.has_value())
        {
            g_nr.failed = true;
            g_nr.reason = "nvngx_dlssnr.dll was not found beside OptiScaler or the game";
            LOG_ERROR("DLSS-NR (D3D11) unavailable: {}", g_nr.reason);
            device->Release();
            return;
        }

        g_nr.feature = g_nr.create(
            snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(), device, ctx,
            g_nr.capabilityParams, workWidth, workHeight, (int) cfg.DlssNrPreset.value_or_default(),
            cfg.DlssNrIntensity.value_or_default(), (int) cfg.DlssNrStyle.value_or_default(),
            cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
            cfg.DlssNrSkinStructure.value_or_default(), cfg.DlssNrAutoMask.value_or_default() ? 1 : 0,
            // UI correction at the model's own default: with no UI layer fed to it there is nothing
            // for it to correct.
            1);

        if (g_nr.feature == nullptr)
        {
            g_nr.failed = true;
            g_nr.reason = "the model would not initialise";
            LOG_ERROR("DLSS-NR (D3D11) create failed: init 0x{:X}, create 0x{:X}",
                      g_nr.lastInit != nullptr ? *g_nr.lastInit : 0,
                      g_nr.lastCreate != nullptr ? *g_nr.lastCreate : 0);
            device->Release();
            return;
        }

        g_nr.width = width;
        g_nr.height = height;
        g_nr.reset = true;
        RecordBuiltTuning(cfg);
        LOG_INFO("DLSS-NR (D3D11) running at {}x{}, guides {}x{} (preset {}, intensity {}, style {})",
                 width, height, guideWidth, guideHeight, g_nr.builtPreset, g_nr.builtIntensity,
                 g_nr.builtStyle);

        // Creating and evaluating in one go is what hung the GPU on the D3D12 path. D3D11's immediate
        // context gives no submission boundary to place between them, so the first evaluate waits for
        // the next frame. One frame without the model is invisible.
        device->Release();
        return;
    }

    if (!g_codec.ensure(device))
    {
        g_nr.failed = true;
        g_nr.reason = "the colour codec would not compile";
        LOG_ERROR("DLSS-NR (D3D11) unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }

    static bool reportedGuides = false;

    if (!reportedGuides)
    {
        reportedGuides = true;
        LOG_INFO("DLSS-NR (D3D11) guides: depth {}, motion vector scale {} x {} (the game says {} x {}, "
                 "times the {}x{} upscale ratio); the buffer is {}",
                 g_nr.guideDepthInverted ? "inverted" : "not inverted", g_nr.guideMvScaleX,
                 g_nr.guideMvScaleY, mvScaleX, mvScaleY, upscaleX, upscaleY,
                 isHdrBuffer ? "linear HDR" : "already tone-mapped");
    }

    // Paper white, and nothing else. The frame is divided by this and encoded; the soft knee above
    // 0.75 takes whatever is left over.
    const float whitePoint = cfg.DlssNrWhitePointScale.value_or_default();

    if (g_gpuTime == nullptr)
        g_gpuTime = std::make_unique<GpuTime_Dx11>(device);

    if (g_gpuTime != nullptr)
        g_gpuTime->Start(ctx);

    codec::Params encodeParams {};
    encodeParams.mode = codec::MODE_ENCODE;
    // A frame that is already display-referred is handed over untouched: the encode becomes a copy.
    encodeParams.passthrough = isHdrBuffer ? 0u : 1u;
    encodeParams.whitePoint = whitePoint;
    encodeParams.width = width;
    encodeParams.height = height;

    g_codec.dispatch(ctx, encodeParams, target, nullptr, nullptr, g_nr.colorCopy, g_nr.hdrCopy);

    // Below full resolution the model is shown a filtered shrink of the proxy; the edit it returns is
    // enlarged during the resolve while the frame underneath stays full size and untouched.
    ID3D11Resource* modelInput = g_nr.colorCopy;

    if (reduced && g_nr.colorSmall != nullptr)
    {
        codec::Params down {};
        down.mode = codec::MODE_DOWNSAMPLE;
        down.width = workWidth;
        down.height = workHeight;
        g_codec.dispatch(ctx, down, modelInput, nullptr, nullptr, g_nr.colorSmall, nullptr);
        modelInput = g_nr.colorSmall;
    }

    // The vectors were scaled to full-frame pixels; the image the model reprojects is the working size.
    const float mvToWork = width != 0 ? (float) workWidth / (float) width : 1.0f;

    const int result = g_nr.evaluate(
        ctx, g_nr.feature, g_nr.capabilityParams, modelInput, depth, motion, g_nr.output, workWidth,
        workHeight, guideWidth, guideHeight, g_nr.guideDepthInverted ? 1 : 0, (g_nr.reset || cfg.DlssNrResetEveryFrame.value_or_default()) ? 1 : 0,
        cfg.DlssNrIntensity.value_or_default(), (int) cfg.DlssNrStyle.value_or_default(),
        cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
        cfg.DlssNrSkinStructure.value_or_default(), cfg.DlssNrAutoMask.value_or_default() ? 1 : 0,
        g_nr.guideMvScaleX * mvToWork, g_nr.guideMvScaleY * mvToWork);

    g_nr.reset = false;

    if (result == NVSDK_NGX_Result_Success)
    {
        codec::Params resolveParams {};
        resolveParams.mode = codec::MODE_RESOLVE;
        resolveParams.whitePoint = whitePoint;
        resolveParams.width = width;
        resolveParams.height = height;
        resolveParams.transferStrength = cfg.DlssNrTransferStrength.value_or_default();
        resolveParams.colourStrength = cfg.DlssNrColourStrength.value_or_default();
        resolveParams.colourGuard = cfg.DlssNrColourGuard.value_or_default();
        resolveParams.debugView = cfg.DlssNrDebugView.value_or_default();
        resolveParams.maxRatio = cfg.DlssNrMaxRatio.value_or_default();
        resolveParams.passthrough = isHdrBuffer ? 0u : 1u;

        g_codec.dispatch(ctx, resolveParams, modelInput, g_nr.output, g_nr.hdrCopy, target, nullptr,
                         motion);
    }
    else
    {
        g_nr.failed = true;
        g_nr.reason = "the model refused to run";
        LOG_ERROR("DLSS-NR (D3D11) evaluate returned 0x{:X}, disabling for this session",
                  (uint32_t) result);
    }

    if (g_gpuTime != nullptr)
    {
        g_gpuTime->End(ctx);

        if (auto measured = g_gpuTime->ReadGpuTime(ctx); measured.has_value())
            g_lastGpuTime = measured;
    }

    device->Release();
}

bool IsRunningDx11() { return g_nr.feature != nullptr && !g_nr.failed; }

const char* FailureReasonDx11() { return g_nr.failed ? g_nr.reason : ""; }

std::optional<double> LastGpuTimeDx11() { return g_lastGpuTime; }

void RetryAfterFailureDx11()
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    g_nr.failed = false;
    g_nr.reason = "";
}

void ShutdownDx11()
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);

    // Freed rather than parked: nothing is in flight once the device is going away, and there will be
    // no further frames to retire them on.
    for (auto& retired : g_retired)
    {
        if (retired.feature != nullptr && g_nr.release != nullptr)
            g_nr.release(retired.feature);
    }

    g_retired.clear();

    if (g_nr.feature != nullptr && g_nr.release != nullptr)
        g_nr.release(g_nr.feature);

    g_nr.feature = nullptr;

    ReleaseSurfaces();
    g_codec.release();
    g_gpuTime.reset();
}

} // namespace DlssNr

#endif // OPTI_DLSSNR
