// DLSS Neural Rendering calls, isolated in a module the snippet will accept as a caller.
//
// The snippet resolves the module owning its return address and requires that module's path to contain
// "nvngx.dll" (the driver core is _nvngx.dll), rejecting anything else with FAIL_PlatformError before it
// inspects a single argument. Neither a ReShade add-on nor OptiScaler is named anything like that, so the
// calls are made from here instead and reached through the exports below.
//
// The parameter block is the core's capability block rather than a fresh one: it carries the snippet and
// preset callbacks a feature expects at create time. The core exports no Set/Get helpers (they are
// static-library inlines), so it is driven through its vtable. NVSDK_NGX_Parameter declares eight Set
// overloads then eight Get overloads, in this order: ULL, float, double, uint, int, ID3D11Resource*,
// ID3D12Resource*, void*.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>

namespace {

// Slot indices confirmed by round-tripping values through the live block: a setter at N is read back by
// the getter at N+8. The unsigned setter is slot 3 (a feature create driven through it succeeds) and the
// resource getter answers at slot 8, so resources are written through slot 0 -- the 64-bit setter, which
// is what a resource handle is. Writing them through the typed D3D12 setter left them unset.
constexpr int VT_SET_ULL = 0;
// Where the float setter actually lives. The public header declares it at slot 1, and this block --
// the driver's own, not the header's implementation -- does not keep a float there: every float written
// to slot 1 reads back as FAIL_UnsupportedParameter while every uint lands. The host discovers the real
// slot by round-tripping a value and sets it here before anything else is written.
int g_floatSlot = 1;
constexpr int VT_SET_UINT = 3;

using PFN_SetULL = void(__thiscall *)(void *, const char *, unsigned long long);
using PFN_SetFloat = void(__thiscall *)(void *, const char *, float);
using PFN_SetUInt = void(__thiscall *)(void *, const char *, unsigned int);

void setUInt(void *params, const char *name, unsigned int v) {
    void **vt = *reinterpret_cast<void ***>(params);
    reinterpret_cast<PFN_SetUInt>(vt[VT_SET_UINT])(params, name, v);
}

void setFloat(void *params, const char *name, float v) {
    void **vt = *reinterpret_cast<void ***>(params);
    reinterpret_cast<PFN_SetFloat>(vt[g_floatSlot])(params, name, v);
}

void setResource(void *params, const char *name, const void *v) {
    void **vt = *reinterpret_cast<void ***>(params);
    reinterpret_cast<PFN_SetULL>(vt[VT_SET_ULL])(params, name, (unsigned long long) v);
}

using PFN_NrInitExt = int(__cdecl *)(unsigned long long, const wchar_t *, ID3D12Device *, int,
                                     const void *);
using PFN_NrCreate = int(__cdecl *)(ID3D12GraphicsCommandList *, int, const void *, void **);
using PFN_NrEvaluate = int(__cdecl *)(ID3D12GraphicsCommandList *, const void *, const void *, void *);
using PFN_NrRelease = int(__cdecl *)(void *);

struct Snippet {
    HMODULE module = nullptr;
    PFN_NrInitExt init = nullptr;
    PFN_NrCreate create = nullptr;
    PFN_NrEvaluate evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    bool initialised = false;
};

Snippet g_snip;

bool loadSnippet(const wchar_t *path) {
    if (g_snip.module) {
        return g_snip.create != nullptr;
    }
    g_snip.module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_snip.module) {
        return false;
    }
    g_snip.init = (PFN_NrInitExt) GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_Init_Ext");
    g_snip.create = (PFN_NrCreate) GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_CreateFeature");
    g_snip.evaluate = (PFN_NrEvaluate) GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_EvaluateFeature");
    g_snip.release = (PFN_NrRelease) GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_ReleaseFeature");
    return g_snip.create != nullptr && g_snip.evaluate != nullptr;
}

// ---------------------------------------------------------------------------------------------------
// D3D11 and Vulkan
//
// The snippet exports a full surface for all three APIs (confirmed against the shipped DLL: D3D11,
// D3D12, VULKAN and CUDA entry points are all present), and the caller gate applies to every one of
// them, so each needs a route through this module for exactly the same reason D3D12 does.
//
// Only three things differ per API: which exports are resolved, what a command list is, and what a
// resource handle points at. Everything written into the parameter block is identical, because the
// block is just named values -- and resources go through the 64-bit setter regardless, so a
// VkImage-bearing NVSDK_NGX_Resource_VK* travels the same path an ID3D12Resource* does.
//
// Vulkan handles are kept as void* rather than including vulkan.h: this project deliberately has no
// include directories beyond the Windows SDK, and every handle in question is a dispatchable pointer.

using VkInstanceH = void *;
using VkPhysicalDeviceH = void *;
using VkDeviceH = void *;
using VkCommandBufferH = void *;

// Same shape as the D3D12 form: the snippet build takes the parameter block where the public header
// takes a FeatureCommonInfo, and the version as a plain int.
using PFN_NrInitExt11 = int(__cdecl *)(unsigned long long, const wchar_t *, ID3D11Device *, int,
                                       const void *);
using PFN_NrCreate11 = int(__cdecl *)(ID3D11DeviceContext *, int, const void *, void **);
using PFN_NrEvaluate11 = int(__cdecl *)(ID3D11DeviceContext *, const void *, const void *, void *);

// Vulkan inserts the instance, physical device and device where D3D has one device.
using PFN_NrInitExtVk = int(__cdecl *)(unsigned long long, const wchar_t *, VkInstanceH,
                                       VkPhysicalDeviceH, VkDeviceH, int, const void *);
using PFN_NrCreateVk = int(__cdecl *)(VkCommandBufferH, int, const void *, void **);
using PFN_NrEvaluateVk = int(__cdecl *)(VkCommandBufferH, const void *, const void *, void *);

struct Snippet11 {
    HMODULE module = nullptr;
    PFN_NrInitExt11 init = nullptr;
    PFN_NrCreate11 create = nullptr;
    PFN_NrEvaluate11 evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    bool initialised = false;
};

struct SnippetVk {
    HMODULE module = nullptr;
    PFN_NrInitExtVk init = nullptr;
    PFN_NrCreateVk create = nullptr;
    PFN_NrEvaluateVk evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    bool initialised = false;
};

Snippet11 g_snip11;
SnippetVk g_snipVk;

bool loadSnippet11(const wchar_t *path) {
    if (g_snip11.module) {
        return g_snip11.create != nullptr;
    }
    g_snip11.module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_snip11.module) {
        return false;
    }
    g_snip11.init = (PFN_NrInitExt11) GetProcAddress(g_snip11.module, "NVSDK_NGX_D3D11_Init_Ext");
    g_snip11.create = (PFN_NrCreate11) GetProcAddress(g_snip11.module, "NVSDK_NGX_D3D11_CreateFeature");
    g_snip11.evaluate =
        (PFN_NrEvaluate11) GetProcAddress(g_snip11.module, "NVSDK_NGX_D3D11_EvaluateFeature");
    g_snip11.release = (PFN_NrRelease) GetProcAddress(g_snip11.module, "NVSDK_NGX_D3D11_ReleaseFeature");
    return g_snip11.create != nullptr && g_snip11.evaluate != nullptr;
}

bool loadSnippetVk(const wchar_t *path) {
    if (g_snipVk.module) {
        return g_snipVk.create != nullptr;
    }
    g_snipVk.module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_snipVk.module) {
        return false;
    }
    g_snipVk.init = (PFN_NrInitExtVk) GetProcAddress(g_snipVk.module, "NVSDK_NGX_VULKAN_Init_Ext");
    g_snipVk.create = (PFN_NrCreateVk) GetProcAddress(g_snipVk.module, "NVSDK_NGX_VULKAN_CreateFeature");
    g_snipVk.evaluate =
        (PFN_NrEvaluateVk) GetProcAddress(g_snipVk.module, "NVSDK_NGX_VULKAN_EvaluateFeature");
    g_snipVk.release = (PFN_NrRelease) GetProcAddress(g_snipVk.module, "NVSDK_NGX_VULKAN_ReleaseFeature");
    return g_snipVk.create != nullptr && g_snipVk.evaluate != nullptr;
}

// The tuning the model latches at create time. Identical for every API.
void writeCreateParams(void *p, unsigned int width, unsigned int height, int preset, float intensity,
                       int style, float localStructure, float localTone, float skinStructure,
                       int useAutoMask, int uiCorrection) {
    setUInt(p, "DLSSNR.Enabled", 1);
    setUInt(p, "DLSSNR.Width", width);
    setUInt(p, "DLSSNR.Height", height);
    setUInt(p, "CreationNodeMask", 1);
    setUInt(p, "VisibilityNodeMask", 1);
    setUInt(p, "DLSSNR.Hint.Render.Preset", (unsigned int) preset);
    setFloat(p, "DLSSNR.Intensity", intensity);
    setUInt(p, "DLSSNR.Style", (unsigned int) style);
    setFloat(p, "DLSSNR.LocalStructureStrength", localStructure);
    setFloat(p, "DLSSNR.LocalToneStrength", localTone);
    setFloat(p, "DLSSNR.SkinStructureStrength", skinStructure);
    setUInt(p, "DLSSNR.UseAutoMask", (unsigned int) useAutoMask);
    setUInt(p, "DLSSNR.UICorrection", (unsigned int) uiCorrection);
}

// Everything an evaluate writes apart from the four resources, which are the only per-API part. The
// order matches what the D3D12 path has always written, because the block is shared with the game's
// own DLSS and the sequence is load-bearing.
void writeEvaluateParams(void *p, unsigned int width, unsigned int height, unsigned int guideWidth,
                         unsigned int guideHeight, int depthInverted, int reset, float intensity,
                         int style, float localStructure, float localTone, float skinStructure,
                         int useAutoMask, float mvScaleX, float mvScaleY) {
    setUInt(p, "DLSSNR.Enabled", 1);
    setUInt(p, "DLSSNR.Width", width);
    setUInt(p, "DLSSNR.Height", height);
    setUInt(p, "DLSSNR.DepthInverted", (unsigned int) depthInverted);
    setUInt(p, "DLSSNR.Reset", (unsigned int) reset);

    setUInt(p, "DLSSNR.ColorSubrectBaseX", 0);
    setUInt(p, "DLSSNR.ColorSubrectBaseY", 0);
    setUInt(p, "DLSSNR.ColorSubrectWidth", width);
    setUInt(p, "DLSSNR.ColorSubrectHeight", height);
    setUInt(p, "DLSSNR.OutputSubrectBaseX", 0);
    setUInt(p, "DLSSNR.OutputSubrectBaseY", 0);
    setUInt(p, "DLSSNR.OutputSubrectWidth", width);
    setUInt(p, "DLSSNR.OutputSubrectHeight", height);
    setUInt(p, "DLSSNR.DepthSubrectBaseX", 0);
    setUInt(p, "DLSSNR.DepthSubrectBaseY", 0);
    setUInt(p, "DLSSNR.DepthSubrectWidth", guideWidth);
    setUInt(p, "DLSSNR.DepthSubrectHeight", guideHeight);
    setUInt(p, "DLSSNR.MVecSubrectBaseX", 0);
    setUInt(p, "DLSSNR.MVecSubrectBaseY", 0);
    setUInt(p, "DLSSNR.MVecSubrectWidth", guideWidth);
    setUInt(p, "DLSSNR.MVecSubrectHeight", guideHeight);

    setFloat(p, "DLSSNR.MVecScaleX", mvScaleX);
    setFloat(p, "DLSSNR.MVecScaleY", mvScaleY);

    setFloat(p, "DLSSNR.Intensity", intensity);
    setUInt(p, "DLSSNR.Style", (unsigned int) style);
    setFloat(p, "DLSSNR.LocalStructureStrength", localStructure);
    setFloat(p, "DLSSNR.LocalToneStrength", localTone);
    setFloat(p, "DLSSNR.SkinStructureStrength", skinStructure);
    setUInt(p, "DLSSNR.UseAutoMask", (unsigned int) useAutoMask);
}

} // namespace

extern "C" {

// Called once, after the host has worked out which slot this block keeps floats in.
__declspec(dllexport) void dlssnr_call_set_float_slot(int slot) {
    if (slot >= 0 && slot < 8) {
        g_floatSlot = slot;
    }
}

// Writes a float through an arbitrary slot, so the host can find the right one by testing.
__declspec(dllexport) void dlssnr_call_probe_float(void *params, const char *name, float value,
                                                   int slot) {
    if (!params || slot < 0 || slot >= 8) {
        return;
    }
    void **vt = *reinterpret_cast<void ***>(params);
    reinterpret_cast<PFN_SetFloat>(vt[slot])(params, name, value);
}

// Last init and create results, so the add-on can log why a feature never appeared.
__declspec(dllexport) int dlssnr_call_last_init = 0;
__declspec(dllexport) int dlssnr_call_last_create = 0;

// Creates a persistent Neural Rendering feature. The handle records initialisation work into cmd, so it
// must outlive that command list's execution; releasing it early loses the device.
__declspec(dllexport) void *dlssnr_call_create(const wchar_t *snippetPath, const wchar_t *dataPath,
                                               ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
                                               void *capabilityParams, unsigned int width,
                                               unsigned int height, int preset, float intensity,
                                               int style, float localStructure, float localTone,
                                               float skinStructure, int useAutoMask,
                                               int uiCorrection) {
    if (!loadSnippet(snippetPath) || !capabilityParams) {
        return nullptr;
    }
    if (!g_snip.initialised && g_snip.init) {
        dlssnr_call_last_init = g_snip.init(0x4350324Bull, dataPath, device, 0x0000015, capabilityParams);
        g_snip.initialised = (dlssnr_call_last_init == 1);
        if (!g_snip.initialised) {
            return nullptr;
        }
    }
    writeCreateParams(capabilityParams, width, height, preset, intensity, style, localStructure, localTone,
                      skinStructure, useAutoMask, uiCorrection);
    void *handle = nullptr;
    dlssnr_call_last_create = g_snip.create(cmd, 18, capabilityParams, &handle);
    return handle;
}

// Colour and output are display resolution; depth and motion come from the game's own DLSS evaluation and
// may be render resolution, so each resource carries its own subrect and motion scales by the ratio.
__declspec(dllexport) int dlssnr_call_evaluate(ID3D12GraphicsCommandList *cmd, void *feature,
                                               void *capabilityParams, ID3D12Resource *color,
                                               ID3D12Resource *depth, ID3D12Resource *motion,
                                               ID3D12Resource *output, unsigned int width,
                                               unsigned int height, unsigned int guideWidth,
                                               unsigned int guideHeight, int depthInverted, int reset,
                                               float intensity, int style, float localStructure,
                                               float localTone, float skinStructure, int useAutoMask,
                                               float mvScaleX, float mvScaleY) {
    if (!feature || !capabilityParams || !g_snip.evaluate) {
        return 0;
    }
    setResource(capabilityParams, "DLSSNR.Color", color);
    setResource(capabilityParams, "DLSSNR.Depth", depth);
    setResource(capabilityParams, "DLSSNR.MVec", motion);
    setResource(capabilityParams, "DLSSNR.Output", output);

    // The block is shared with the game's own DLSS, which overwrites these between frames, so every
    // value the feature reads is set again here rather than relying on what create left behind.
    writeEvaluateParams(capabilityParams, width, height, guideWidth, guideHeight, depthInverted, reset,
                        intensity, style, localStructure, localTone, skinStructure, useAutoMask, mvScaleX,
                        mvScaleY);

    // The result must not be returned directly. `return f(...)` is a tail call, and the compiler emits a
    // jmp rather than a call, which leaves this module's frame behind: the snippet then resolves its
    // caller to whoever called us and rejects it. Keeping the value in a volatile forces a real call and
    // a return through this module, which is the whole reason this file exists.
    volatile int result = g_snip.evaluate(cmd, feature, capabilityParams, nullptr);
    return result;
}

// Inputs NVIDIA's own Streamline plugin sets that the positional exports predate: the model's global
// tone strength (read at create), and the interface as the game draws it -- its layer, its alpha, and
// the composited back buffer -- which is what the model's UI correction was designed around. Called
// before create and before every evaluate; absent resources are written as null, because the block
// outlives everything and a stale pointer is a freed resource.
__declspec(dllexport) void dlssnr_call_set_extras(void *capabilityParams, float globalTone,
                                                  ID3D12Resource *ui, ID3D12Resource *uiAlpha,
                                                  ID3D12Resource *backbuffer, unsigned int uiWidth,
                                                  unsigned int uiHeight, unsigned int bbWidth,
                                                  unsigned int bbHeight) {
    if (!capabilityParams) {
        return;
    }
    setFloat(capabilityParams, "DLSSNR.GlobalToneStrength", globalTone);
    setResource(capabilityParams, "DLSSNR.UI", ui);
    setResource(capabilityParams, "DLSSNR.UIAlpha", uiAlpha);
    setResource(capabilityParams, "DLSSNR.Backbuffer", backbuffer);
    setUInt(capabilityParams, "DLSSNR.UISubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.UISubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.UISubrectWidth", uiWidth);
    setUInt(capabilityParams, "DLSSNR.UISubrectHeight", uiHeight);
    setUInt(capabilityParams, "DLSSNR.UIAlphaSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.UIAlphaSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.UIAlphaSubrectWidth", uiWidth);
    setUInt(capabilityParams, "DLSSNR.UIAlphaSubrectHeight", uiHeight);
    setUInt(capabilityParams, "DLSSNR.BackbufferSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.BackbufferSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.BackbufferSubrectWidth", bbWidth);
    setUInt(capabilityParams, "DLSSNR.BackbufferSubrectHeight", bbHeight);
}

__declspec(dllexport) void dlssnr_call_release(void *feature) {
    if (feature && g_snip.release) {
        volatile int result = g_snip.release(feature); // not a tail call, for the reason above
        (void) result;
    }
}


// ---------------------------------------------------------------------------------------------------
// D3D11
//
// The snippet's D3D11 surface takes a device context where D3D12 takes a command list; nothing else
// about the sequence changes. A native D3D11 game therefore does not have to be bridged onto D3D12
// just to reach the model.

__declspec(dllexport) void *dlssnr_call_create_d3d11(const wchar_t *snippetPath, const wchar_t *dataPath,
                                                     ID3D11Device *device, ID3D11DeviceContext *ctx,
                                                     void *capabilityParams, unsigned int width,
                                                     unsigned int height, int preset, float intensity,
                                                     int style, float localStructure, float localTone,
                                                     float skinStructure, int useAutoMask,
                                                     int uiCorrection) {
    if (!loadSnippet11(snippetPath) || !capabilityParams) {
        return nullptr;
    }
    if (!g_snip11.initialised && g_snip11.init) {
        dlssnr_call_last_init =
            g_snip11.init(0x4350324Bull, dataPath, device, 0x0000015, capabilityParams);
        g_snip11.initialised = (dlssnr_call_last_init == 1);
        if (!g_snip11.initialised) {
            return nullptr;
        }
    }
    writeCreateParams(capabilityParams, width, height, preset, intensity, style, localStructure, localTone,
                      skinStructure, useAutoMask, uiCorrection);
    void *handle = nullptr;
    dlssnr_call_last_create = g_snip11.create(ctx, 18, capabilityParams, &handle);
    return handle;
}

__declspec(dllexport) int dlssnr_call_evaluate_d3d11(ID3D11DeviceContext *ctx, void *feature,
                                                     void *capabilityParams, ID3D11Resource *color,
                                                     ID3D11Resource *depth, ID3D11Resource *motion,
                                                     ID3D11Resource *output, unsigned int width,
                                                     unsigned int height, unsigned int guideWidth,
                                                     unsigned int guideHeight, int depthInverted,
                                                     int reset, float intensity, int style,
                                                     float localStructure, float localTone,
                                                     float skinStructure, int useAutoMask, float mvScaleX,
                                                     float mvScaleY) {
    if (!feature || !capabilityParams || !g_snip11.evaluate) {
        return 0;
    }
    setResource(capabilityParams, "DLSSNR.Color", color);
    setResource(capabilityParams, "DLSSNR.Depth", depth);
    setResource(capabilityParams, "DLSSNR.MVec", motion);
    setResource(capabilityParams, "DLSSNR.Output", output);

    writeEvaluateParams(capabilityParams, width, height, guideWidth, guideHeight, depthInverted, reset,
                        intensity, style, localStructure, localTone, skinStructure, useAutoMask, mvScaleX,
                        mvScaleY);

    // Volatile for the same reason as the D3D12 path: a tail call would leave this module's frame
    // behind and the snippet would resolve its caller to whoever called us.
    volatile int result = g_snip11.evaluate(ctx, feature, capabilityParams, nullptr);
    return result;
}

__declspec(dllexport) void dlssnr_call_release_d3d11(void *feature) {
    if (feature && g_snip11.release) {
        volatile int result = g_snip11.release(feature);
        (void) result;
    }
}

// ---------------------------------------------------------------------------------------------------
// Vulkan
//
// Resources are NVSDK_NGX_Resource_VK structures rather than bare handles -- the model needs the view,
// format and extent, none of which can be recovered from a VkImage. The host builds them; this module
// only passes the pointers, which go through the same 64-bit setter everything else does.

__declspec(dllexport) void *dlssnr_call_create_vk(const wchar_t *snippetPath, const wchar_t *dataPath,
                                                  VkInstanceH instance, VkPhysicalDeviceH physicalDevice,
                                                  VkDeviceH device, VkCommandBufferH cmd,
                                                  void *capabilityParams, unsigned int width,
                                                  unsigned int height, int preset, float intensity,
                                                  int style, float localStructure, float localTone,
                                                  float skinStructure, int useAutoMask, int uiCorrection) {
    if (!loadSnippetVk(snippetPath) || !capabilityParams) {
        return nullptr;
    }
    if (!g_snipVk.initialised && g_snipVk.init) {
        dlssnr_call_last_init = g_snipVk.init(0x4350324Bull, dataPath, instance, physicalDevice, device,
                                              0x0000015, capabilityParams);
        g_snipVk.initialised = (dlssnr_call_last_init == 1);
        if (!g_snipVk.initialised) {
            return nullptr;
        }
    }
    writeCreateParams(capabilityParams, width, height, preset, intensity, style, localStructure, localTone,
                      skinStructure, useAutoMask, uiCorrection);
    void *handle = nullptr;
    dlssnr_call_last_create = g_snipVk.create(cmd, 18, capabilityParams, &handle);
    return handle;
}

__declspec(dllexport) int dlssnr_call_evaluate_vk(VkCommandBufferH cmd, void *feature,
                                                  void *capabilityParams, const void *color,
                                                  const void *depth, const void *motion,
                                                  const void *output, unsigned int width,
                                                  unsigned int height, unsigned int guideWidth,
                                                  unsigned int guideHeight, int depthInverted, int reset,
                                                  float intensity, int style, float localStructure,
                                                  float localTone, float skinStructure, int useAutoMask,
                                                  float mvScaleX, float mvScaleY) {
    if (!feature || !capabilityParams || !g_snipVk.evaluate) {
        return 0;
    }
    setResource(capabilityParams, "DLSSNR.Color", color);
    setResource(capabilityParams, "DLSSNR.Depth", depth);
    setResource(capabilityParams, "DLSSNR.MVec", motion);
    setResource(capabilityParams, "DLSSNR.Output", output);

    writeEvaluateParams(capabilityParams, width, height, guideWidth, guideHeight, depthInverted, reset,
                        intensity, style, localStructure, localTone, skinStructure, useAutoMask, mvScaleX,
                        mvScaleY);

    volatile int result = g_snipVk.evaluate(cmd, feature, capabilityParams, nullptr);
    return result;
}

__declspec(dllexport) void dlssnr_call_release_vk(void *feature) {
    if (feature && g_snipVk.release) {
        volatile int result = g_snipVk.release(feature);
        (void) result;
    }
}
} // extern "C"
