#include <pch.h>

#include "DlssNr_Switch.h"

#if OPTI_DLSSNR

#include "DlssNr.h"
#include "DlssNr_Codec_Vk.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <proxies/NVNGX_Proxy.h>

#include <nvsdk_ngx_vk.h>

#include <mutex>
#include <vector>

// DLSS 5 Neural Rendering on Vulkan.
//
// Same sequence as the D3D12 module -- encode the frame into a display-referred proxy, optionally
// shrink it, run the model, compose the answer back over the untouched original -- on the game's own
// Vulkan device.
//
// Two things genuinely differ from the D3D paths, and both are Vulkan's own rules rather than
// anything about the model:
//
//   - Nothing is implicit. Every scratch image needs its own allocation, view and layout transition,
//     and every read-after-write between passes needs a barrier that D3D12 got from a state change and
//     D3D11 got for free.
//
//   - The model is handed NVSDK_NGX_Resource_VK structures rather than bare handles. A VkImage alone
//     says nothing about its view, format or extent, so each one is described in full.
//
// Every image the codec touches is kept in VK_IMAGE_LAYOUT_GENERAL. That is required for the storage
// images and merely allowed for the sampled ones, and these surfaces swap between the two roles from
// pass to pass -- the proxy is written by the encode and read by the resolve. One layout throughout
// removes a class of mistake that produces no validation error, just wrong pixels.

namespace
{

using VkHandle = void*;

using PFN_NrCreateVk = void*(__cdecl*) (const wchar_t*, const wchar_t*, VkHandle, VkHandle, VkHandle,
                                        VkHandle, void*, unsigned int, unsigned int, int, float, int,
                                        float, float, float, int, int);
using PFN_NrEvaluateVk = int(__cdecl*) (VkHandle, void*, void*, const void*, const void*, const void*,
                                        const void*, unsigned int, unsigned int, unsigned int,
                                        unsigned int, int, int, float, int, float, float, float, int,
                                        float, float);
using PFN_NrReleaseVk = void(__cdecl*) (void*);
using PFN_NrSetFloatSlot = void(__cdecl*) (int);
using PFN_NrProbeFloat = void(__cdecl*) (void*, const char*, float, int);

// A scratch image and everything needed to describe it, to the codec and to the model.
struct Surface
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    unsigned int width = 0;
    unsigned int height = 0;
    // Tracked so the first transition can come from UNDEFINED, which discards rather than preserving.
    bool initialised = false;
};

struct NrStateVk
{
    HMODULE forwarder = nullptr;
    PFN_NrCreateVk create = nullptr;
    PFN_NrEvaluateVk evaluate = nullptr;
    PFN_NrReleaseVk release = nullptr;
    PFN_NrSetFloatSlot setFloatSlot = nullptr;
    PFN_NrProbeFloat probeFloat = nullptr;
    bool floatSlotKnown = false;
    int* lastInit = nullptr;
    int* lastCreate = nullptr;

    NVSDK_NGX_Parameter* capabilityParams = nullptr;
    void* feature = nullptr;

    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;

    Surface colorCopy;
    Surface hdrCopy;
    Surface output;
    Surface colorSmall;

    unsigned int workWidth = 0;
    unsigned int workHeight = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool reset = true;

    bool guideDepthInverted = false;
    float guideMvScaleX = 1.0f;
    float guideMvScaleY = 1.0f;

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

NrStateVk g_nr;
codec::Codec_Vk g_codec;
std::mutex g_nrMutex;
std::filesystem::path g_dllDir;
unsigned long long g_frames = 0;

// Retired features and surfaces. Vulkan frees exactly when told to, with no reference counting to
// defer anything, so releasing either while the GPU is still several frames deep in work that names
// it is the way to lose the device.
struct RetiredVk
{
    void* feature = nullptr;
    Surface surface;
    unsigned long long freeAtFrame = 0;
};

std::vector<RetiredVk> g_retired;
constexpr unsigned long long kParkFrames = 32;
constexpr unsigned long long kSettleFrames = 30;

void DestroySurface(Surface& s)
{
    if (g_nr.device == VK_NULL_HANDLE)
        return;

    if (s.view != VK_NULL_HANDLE)
        vkDestroyImageView(g_nr.device, s.view, nullptr);

    if (s.image != VK_NULL_HANDLE)
        vkDestroyImage(g_nr.device, s.image, nullptr);

    if (s.memory != VK_NULL_HANDLE)
        vkFreeMemory(g_nr.device, s.memory, nullptr);

    s = {};
}

void ParkFeature(void*& feature)
{
    if (feature == nullptr)
        return;

    RetiredVk retired;
    retired.feature = feature;
    retired.freeAtFrame = g_frames + kParkFrames;
    g_retired.push_back(retired);
    feature = nullptr;
}

void ParkSurface(Surface& s)
{
    if (s.image == VK_NULL_HANDLE)
        return;

    RetiredVk retired;
    retired.surface = s;
    retired.freeAtFrame = g_frames + kParkFrames;
    g_retired.push_back(retired);
    s = {};
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

        DestroySurface(it->surface);
        it = g_retired.erase(it);
    }
}

bool CreateSurface(Surface& s, VkFormat format, unsigned int width, unsigned int height)
{
    if (g_nr.device == VK_NULL_HANDLE || width == 0 || height == 0)
        return false;

    s = {};
    s.format = format;
    s.width = width;
    s.height = height;

    VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = { width, height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    // STORAGE for the codec's writes and the model's output, SAMPLED for the codec's reads, TRANSFER
    // for the copies the model's staging needs.
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(g_nr.device, &imageInfo, nullptr, &s.image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(g_nr.device, s.image, &requirements);

    VkPhysicalDeviceMemoryProperties memoryProps = {};
    vkGetPhysicalDeviceMemoryProperties(g_nr.physicalDevice, &memoryProps);

    uint32_t typeIndex = UINT32_MAX;

    for (uint32_t i = 0; i < memoryProps.memoryTypeCount; ++i)
    {
        if ((requirements.memoryTypeBits & (1u << i)) == 0)
            continue;

        if (memoryProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        {
            typeIndex = i;
            break;
        }
    }

    if (typeIndex == UINT32_MAX)
    {
        DestroySurface(s);
        return false;
    }

    VkMemoryAllocateInfo memoryInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    memoryInfo.allocationSize = requirements.size;
    memoryInfo.memoryTypeIndex = typeIndex;

    if (vkAllocateMemory(g_nr.device, &memoryInfo, nullptr, &s.memory) != VK_SUCCESS ||
        vkBindImageMemory(g_nr.device, s.image, s.memory, 0) != VK_SUCCESS)
    {
        DestroySurface(s);
        return false;
    }

    VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewInfo.image = s.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    if (vkCreateImageView(g_nr.device, &viewInfo, nullptr, &s.view) != VK_SUCCESS)
    {
        DestroySurface(s);
        return false;
    }

    return true;
}

// Moves an image to GENERAL and, for one already there, doubles as the read-after-write barrier
// between two codec passes.
void ToGeneral(VkCommandBuffer cmd, Surface& s)
{
    if (s.image == VK_NULL_HANDLE)
        return;

    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = s.initialised ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = s.image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    s.initialised = true;
}

// The game's own images arrive already in a layout it chose. Transitioning them would be wrong -- the
// upscaler just wrote them and the game will use them again -- so only a barrier is issued, to order
// this pass's reads after those writes.
void BarrierExternal(VkCommandBuffer cmd, VkImage image)
{
    if (image == VK_NULL_HANDLE)
        return;

    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
}

NVSDK_NGX_Resource_VK Describe(const Surface& s, bool readWrite)
{
    NVSDK_NGX_Resource_VK res = {};
    res.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    res.ReadWrite = readWrite;
    res.Resource.ImageViewInfo.ImageView = s.view;
    res.Resource.ImageViewInfo.Image = s.image;
    res.Resource.ImageViewInfo.Format = s.format;
    res.Resource.ImageViewInfo.Width = s.width;
    res.Resource.ImageViewInfo.Height = s.height;
    res.Resource.ImageViewInfo.SubresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return res;
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

    g_nr.create = (PFN_NrCreateVk) GetProcAddress(g_nr.forwarder, "dlssnr_call_create_vk");
    g_nr.evaluate = (PFN_NrEvaluateVk) GetProcAddress(g_nr.forwarder, "dlssnr_call_evaluate_vk");
    g_nr.release = (PFN_NrReleaseVk) GetProcAddress(g_nr.forwarder, "dlssnr_call_release_vk");
    g_nr.setFloatSlot = (PFN_NrSetFloatSlot) GetProcAddress(g_nr.forwarder, "dlssnr_call_set_float_slot");
    g_nr.probeFloat = (PFN_NrProbeFloat) GetProcAddress(g_nr.forwarder, "dlssnr_call_probe_float");
    g_nr.lastInit = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_init");
    g_nr.lastCreate = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_create");

    if (g_nr.create == nullptr || g_nr.evaluate == nullptr)
    {
        g_nr.reason = "the forwarder has no Vulkan exports; it predates Vulkan support";
        LOG_ERROR("DLSS-NR: {}", g_nr.reason);
        return false;
    }

    LOG_INFO("DLSS-NR (Vulkan) forwarder loaded from {}", path.string());
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
    const float expected = 0.375f;

    for (int slot : kCandidates)
    {
        float readBack = 0.0f;
        g_nr.probeFloat(params, kProbeKey, expected, slot);

        if (params->Get(kProbeKey, &readBack) == NVSDK_NGX_Result_Success && readBack == expected)
        {
            g_nr.setFloatSlot(slot);
            LOG_INFO("DLSS-NR (Vulkan) float parameters go through vtable slot {}", slot);
            return;
        }
    }

    LOG_ERROR("DLSS-NR could not find the float setter: intensity, local structure, local tone and skin "
              "structure will have no effect. The uint parameters still apply.");
}

bool EnsureCapabilityParams()
{
    if (g_nr.capabilityParams != nullptr)
        return true;

    if (NVNGXProxy::VULKAN_GetCapabilityParameters() == nullptr)
    {
        g_nr.reason = "the NGX core has no Vulkan capability parameters";
        return false;
    }

    if (NVNGXProxy::VULKAN_GetCapabilityParameters()(&g_nr.capabilityParams) != NVSDK_NGX_Result_Success ||
        g_nr.capabilityParams == nullptr)
    {
        g_nr.capabilityParams = nullptr;
        g_nr.reason = "the NGX core refused its capability parameters";
        return false;
    }

    DiscoverFloatSlot(g_nr.capabilityParams);
    return true;
}

NVSDK_NGX_Resource_VK* GetResource(NVSDK_NGX_Parameter* params, const char* a, const char* b)
{
    NVSDK_NGX_Resource_VK* res = nullptr;

    if (params->Get(a, (void**) &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    res = nullptr;

    if (params->Get(b, (void**) &res) == NVSDK_NGX_Result_Success && res != nullptr)
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

void ReleaseSurfaces()
{
    ParkSurface(g_nr.colorCopy);
    ParkSurface(g_nr.hdrCopy);
    ParkSurface(g_nr.output);
    ParkSurface(g_nr.colorSmall);
}

} // namespace

namespace DlssNr
{

void EvaluateAfterUpscaleVk(VkCommandBuffer cmd, NVSDK_NGX_Parameter* params, VkInstance instance,
                            VkPhysicalDevice physicalDevice, VkDevice device)
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    const Config& cfg = *Config::Instance();

    if (!cfg.DlssNrEnabled.value_or_default() || g_nr.failed || cmd == VK_NULL_HANDLE || params == nullptr)
        return;

    if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE)
        return;

    NVSDK_NGX_Resource_VK* target = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    NVSDK_NGX_Resource_VK* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    NVSDK_NGX_Resource_VK* motion =
        GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    // Without all three there is nothing to run on. Not a failure -- some evaluates legitimately carry
    // none of it -- so this stays quiet and tries again next frame.
    if (target == nullptr || depth == nullptr || motion == nullptr)
        return;

    if (target->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW)
        return;

    g_nr.device = device;
    g_nr.physicalDevice = physicalDevice;

    const auto& targetInfo = target->Resource.ImageViewInfo;
    const unsigned int width = targetInfo.Width;
    const unsigned int height = targetInfo.Height;

    if (width == 0 || height == 0)
        return;

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

    const float upscaleX = guideWidth != 0 ? (float) width / (float) guideWidth : 1.0f;
    const float upscaleY = guideHeight != 0 ? (float) height / (float) guideHeight : 1.0f;
    g_nr.guideMvScaleX = mvScaleX * upscaleX;
    g_nr.guideMvScaleY = mvScaleY * upscaleY;

    if (!EnsureForwarder() || !EnsureCapabilityParams())
    {
        g_nr.failed = true;
        LOG_ERROR("DLSS-NR (Vulkan) unavailable: {}", g_nr.reason);
        return;
    }

    float workScale = cfg.DlssNrWorkingScale.value_or_default();
    workScale = workScale < 0.25f ? 0.25f : (workScale > 1.0f ? 1.0f : workScale);
    const auto workWidth = (unsigned int) (width * workScale + 0.5f);
    const auto workHeight = (unsigned int) (height * workScale + 0.5f);
    const bool reduced = workWidth != width || workHeight != height;

    ++g_frames;
    TickRetired();

    if (g_nr.format != VK_FORMAT_UNDEFINED && g_nr.format != targetInfo.Format)
    {
        ReleaseSurfaces();
        ParkFeature(g_nr.feature);
    }

    g_nr.format = targetInfo.Format;

    // The tuning is latched when the feature is created, so a change means a rebuild -- but only once
    // the value has stopped moving, or dragging a slider burns through the driver's latches and the
    // model stops responding until the process restarts.
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

    if (g_nr.output.image == VK_NULL_HANDLE)
    {
        if (!CreateSurface(g_nr.output, targetInfo.Format, workWidth, workHeight) ||
            !CreateSurface(g_nr.colorCopy, targetInfo.Format, width, height) ||
            !CreateSurface(g_nr.hdrCopy, targetInfo.Format, width, height))
        {
            g_nr.failed = true;
            g_nr.reason = "the staging images could not be created";
            LOG_ERROR("DLSS-NR (Vulkan) unavailable: {}", g_nr.reason);
            return;
        }

        g_nr.workWidth = workWidth;
        g_nr.workHeight = workHeight;
    }

    if (reduced && g_nr.colorSmall.image == VK_NULL_HANDLE &&
        !CreateSurface(g_nr.colorSmall, targetInfo.Format, workWidth, workHeight))
    {
        g_nr.failed = true;
        g_nr.reason = "the reduced working image could not be created";
        LOG_ERROR("DLSS-NR (Vulkan) unavailable: {}", g_nr.reason);
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
            LOG_ERROR("DLSS-NR (Vulkan) unavailable: {}", g_nr.reason);
            return;
        }

        g_nr.feature = g_nr.create(
            snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(), instance,
            physicalDevice, device, cmd, g_nr.capabilityParams, workWidth, workHeight,
            (int) cfg.DlssNrPreset.value_or_default(), cfg.DlssNrIntensity.value_or_default(),
            (int) cfg.DlssNrStyle.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
            cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
            cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, 1);

        if (g_nr.feature == nullptr)
        {
            g_nr.failed = true;
            g_nr.reason = "the model would not initialise";
            LOG_ERROR("DLSS-NR (Vulkan) create failed: init 0x{:X}, create 0x{:X}",
                      g_nr.lastInit != nullptr ? *g_nr.lastInit : 0,
                      g_nr.lastCreate != nullptr ? *g_nr.lastCreate : 0);
            return;
        }

        g_nr.width = width;
        g_nr.height = height;
        g_nr.reset = true;
        RecordBuiltTuning(cfg);
        LOG_INFO("DLSS-NR (Vulkan) running at {}x{}, guides {}x{} (preset {}, intensity {}, style {})",
                 width, height, guideWidth, guideHeight, g_nr.builtPreset, g_nr.builtIntensity,
                 g_nr.builtStyle);

        // The create recorded work into this command buffer. Evaluating on the same one is the
        // dice-roll that hung the GPU on D3D12; the first evaluate waits for the next frame.
        return;
    }

    if (!g_codec.ensure(device, physicalDevice))
    {
        g_nr.failed = true;
        g_nr.reason = "the colour codec pipeline could not be created";
        LOG_ERROR("DLSS-NR (Vulkan) unavailable: {}", g_nr.reason);
        return;
    }

    static bool reportedGuides = false;

    if (!reportedGuides)
    {
        reportedGuides = true;
        LOG_INFO("DLSS-NR (Vulkan) guides: depth {}, motion vector scale {} x {} (the game says {} x {}, "
                 "times the {}x{} upscale ratio); the buffer is {}",
                 g_nr.guideDepthInverted ? "inverted" : "not inverted", g_nr.guideMvScaleX,
                 g_nr.guideMvScaleY, mvScaleX, mvScaleY, upscaleX, upscaleY,
                 isHdrBuffer ? "linear HDR" : "already tone-mapped");
    }

    const float whitePoint = cfg.DlssNrWhitePointScale.value_or_default();

    ToGeneral(cmd, g_nr.colorCopy);
    ToGeneral(cmd, g_nr.hdrCopy);
    ToGeneral(cmd, g_nr.output);

    if (reduced)
        ToGeneral(cmd, g_nr.colorSmall);

    BarrierExternal(cmd, targetInfo.Image);

    codec::Params encodeParams {};
    encodeParams.mode = codec::MODE_ENCODE;
    encodeParams.passthrough = isHdrBuffer ? 0u : 1u;
    encodeParams.whitePoint = whitePoint;
    encodeParams.width = width;
    encodeParams.height = height;

    g_codec.dispatch(cmd, encodeParams, targetInfo.ImageView, VK_NULL_HANDLE, VK_NULL_HANDLE,
                     VK_NULL_HANDLE, g_nr.colorCopy.view, g_nr.hdrCopy.view);

    // Orders the model's read of the proxy after the encode wrote it.
    ToGeneral(cmd, g_nr.colorCopy);
    ToGeneral(cmd, g_nr.hdrCopy);

    const Surface* modelInput = &g_nr.colorCopy;

    if (reduced && g_nr.colorSmall.image != VK_NULL_HANDLE)
    {
        codec::Params down {};
        down.mode = codec::MODE_DOWNSAMPLE;
        down.width = workWidth;
        down.height = workHeight;

        g_codec.dispatch(cmd, down, g_nr.colorCopy.view, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                         g_nr.colorSmall.view, VK_NULL_HANDLE);

        ToGeneral(cmd, g_nr.colorSmall);
        modelInput = &g_nr.colorSmall;
    }

    NVSDK_NGX_Resource_VK colorRes = Describe(*modelInput, false);
    NVSDK_NGX_Resource_VK outputRes = Describe(g_nr.output, true);

    const float mvToWork = width != 0 ? (float) workWidth / (float) width : 1.0f;

    const int result =
        g_nr.evaluate(cmd, g_nr.feature, g_nr.capabilityParams, &colorRes, depth, motion, &outputRes,
                      workWidth, workHeight, guideWidth, guideHeight, g_nr.guideDepthInverted ? 1 : 0,
                      g_nr.reset ? 1 : 0, cfg.DlssNrIntensity.value_or_default(),
                      (int) cfg.DlssNrStyle.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
                      cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
                      cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, g_nr.guideMvScaleX * mvToWork,
                      g_nr.guideMvScaleY * mvToWork);

    g_nr.reset = false;

    if (result == NVSDK_NGX_Result_Success)
    {
        // Orders the resolve's read of the model's answer after the model wrote it.
        ToGeneral(cmd, g_nr.output);

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

        g_codec.dispatch(cmd, resolveParams, modelInput->view, g_nr.output.view, g_nr.hdrCopy.view,
                         motion->Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW
                             ? motion->Resource.ImageViewInfo.ImageView
                             : VK_NULL_HANDLE,
                         targetInfo.ImageView, VK_NULL_HANDLE);

        // Orders the game's later use of the frame after the resolve wrote it.
        BarrierExternal(cmd, targetInfo.Image);
    }
    else
    {
        g_nr.failed = true;
        g_nr.reason = "the model refused to run";
        LOG_ERROR("DLSS-NR (Vulkan) evaluate returned 0x{:X}, disabling for this session",
                  (uint32_t) result);
    }
}

bool IsRunningVk() { return g_nr.feature != nullptr && !g_nr.failed; }

const char* FailureReasonVk() { return g_nr.failed ? g_nr.reason : ""; }

void RetryAfterFailureVk()
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    g_nr.failed = false;
    g_nr.reason = "";
}

void ShutdownVk()
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);

    // Freed rather than parked: nothing is in flight once the device is going away, and there will be
    // no further frames to retire anything on.
    for (auto& retired : g_retired)
    {
        if (retired.feature != nullptr && g_nr.release != nullptr)
            g_nr.release(retired.feature);

        DestroySurface(retired.surface);
    }

    g_retired.clear();

    if (g_nr.feature != nullptr && g_nr.release != nullptr)
        g_nr.release(g_nr.feature);

    g_nr.feature = nullptr;

    DestroySurface(g_nr.colorCopy);
    DestroySurface(g_nr.hdrCopy);
    DestroySurface(g_nr.output);
    DestroySurface(g_nr.colorSmall);

    g_codec.release();
    g_nr.device = VK_NULL_HANDLE;
}

} // namespace DlssNr

#endif // OPTI_DLSSNR
