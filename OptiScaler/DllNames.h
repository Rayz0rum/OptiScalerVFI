#pragma once

#include "SysUtils.h"

#include <proxies/KernelBase_Proxy.h>

#include <cwctype> // for std::towlower

#define DEFINE_NAME_VECTORS(varName, ...)                                                                              \
    inline std::vector<std::string> varName##Names = []                                                                \
    {                                                                                                                  \
        std::vector<std::string> v;                                                                                    \
        const char* libs[] = { __VA_ARGS__ };                                                                          \
        for (auto lib : libs)                                                                                          \
        {                                                                                                              \
            v.emplace_back(std::string(lib) + ".dll");                                                                 \
            v.emplace_back(std::string(lib));                                                                          \
        }                                                                                                              \
        return v;                                                                                                      \
    }();                                                                                                               \
    inline std::vector<std::wstring> varName##NamesW = []                                                              \
    {                                                                                                                  \
        std::vector<std::wstring> v;                                                                                   \
        const char* libs[] = { __VA_ARGS__ };                                                                          \
        for (auto lib : libs)                                                                                          \
        {                                                                                                              \
            std::string narrow(lib);                                                                                   \
            std::wstring wide(narrow.begin(), narrow.end());                                                           \
            v.emplace_back(wide + L".dll");                                                                            \
            v.emplace_back(wide);                                                                                      \
        }                                                                                                              \
        return v;                                                                                                      \
    }();

inline std::vector<std::string> dllNames;
inline std::vector<std::wstring> dllNamesW;

//"rtsshooks64.dll", "rtsshooks64", "rtsshooks.dll", "rtsshooks",

inline std::vector<std::string> overlayNames = { "eosovh-win32-shipping.dll",
                                                 "eosovh-win32-shipping",
                                                 "eosovh-win64-shipping.dll",
                                                 "eosovh-win64-shipping", // Epic
                                                 "gameoverlayrenderer64",
                                                 "gameoverlayrenderer64.dll",
                                                 "gameoverlayrenderer",
                                                 "gameoverlayrenderer.dll", // Steam
                                                 "socialclubd3d12renderer",
                                                 "socialclubd3d12renderer.dll", // Rockstar
                                                 "owutils.dll",
                                                 "owutils", // Overwolf
                                                 "galaxy.dll",
                                                 "galaxy",
                                                 "galaxy64.dll",
                                                 "galaxy64", // GOG Galaxy
                                                 "discordoverlay.dll",
                                                 "discordoverlay",
                                                 "discordoverlay64.dll",
                                                 "discordoverlay64", // Discord
                                                 "overlay64",
                                                 "overlay64.dll",
                                                 "overlay",
                                                 "overlay.dll" }; // Ubisoft

inline std::vector<std::wstring> overlayNamesW = { L"eosovh-win32-shipping.dll",
                                                   L"eosovh-win32-shipping",
                                                   L"eosovh-win64-shipping.dll",
                                                   L"eosovh-win64-shipping",
                                                   L"gameoverlayrenderer64",
                                                   L"gameoverlayrenderer64.dll",
                                                   L"gameoverlayrenderer",
                                                   L"gameoverlayrenderer.dll",
                                                   L"socialclubd3d12renderer",
                                                   L"socialclubd3d12renderer.dll",
                                                   L"owutils.dll",
                                                   L"owutils",
                                                   L"galaxy.dll",
                                                   L"galaxy",
                                                   L"galaxy64.dll",
                                                   L"galaxy64",
                                                   L"discordoverlay.dll",
                                                   L"discordoverlay",
                                                   L"discordoverlay64.dll",
                                                   L"discordoverlay64",
                                                   L"overlay64",
                                                   L"overlay64.dll",
                                                   L"overlay",
                                                   L"overlay.dll" };

inline std::vector<std::string> blockOverlayNames = { "eosovh-win32-shipping.dll",
                                                      "eosovh-win32-shipping",
                                                      "eosovh-win64-shipping.dll",
                                                      "eosovh-win64-shipping",
                                                      "gameoverlayrenderer64",
                                                      "gameoverlayrenderer64.dll",
                                                      "gameoverlayrenderer",
                                                      "gameoverlayrenderer.dll",
                                                      "owclient.dll",
                                                      "owclient"
                                                      "galaxy.dll",
                                                      "galaxy",
                                                      "galaxy64.dll",
                                                      "galaxy64",
                                                      "discordoverlay.dll",
                                                      "discordoverlay",
                                                      "discordoverlay64.dll",
                                                      "discordoverlay64",
                                                      "overlay64",
                                                      "overlay64.dll",
                                                      "overlay",
                                                      "overlay.dll" };

inline std::vector<std::wstring> blockedDllNamesW = { L"windhawk.dll", L"mactype.dll", L"mactype64.dll" };

inline std::vector<std::wstring> blockOverlayNamesW = { L"eosovh-win32-shipping.dll",
                                                        L"eosovh-win32-shipping",
                                                        L"eosovh-win64-shipping.dll",
                                                        L"eosovh-win64-shipping",
                                                        L"gameoverlayrenderer64",
                                                        L"gameoverlayrenderer64.dll",
                                                        L"gameoverlayrenderer",
                                                        L"gameoverlayrenderer.dll",
                                                        L"owclient.dll",
                                                        L"owclient",
                                                        L"galaxy.dll",
                                                        L"galaxy",
                                                        L"galaxy64.dll",
                                                        L"galaxy64",
                                                        L"discordoverlay.dll",
                                                        L"discordoverlay",
                                                        L"discordoverlay64.dll",
                                                        L"discordoverlay64",
                                                        L"overlay64",
                                                        L"overlay64.dll",
                                                        L"overlay",
                                                        L"overlay.dll" };

inline std::vector<std::string> skipDxgiWrappingNames = { "eosovh-win32-shipping.dll",
                                                          "eosovh-win64-shipping.dll",
                                                          "gameoverlayrenderer64",
                                                          "gameoverlayrenderer64.dll",
                                                          "gameoverlayrenderer.dll",
                                                          "socialclubd3d12renderer.dll",
                                                          "owutils.dll",
                                                          "galaxy.dll",
                                                          "galaxy64.dll",
                                                          "discordoverlay.dll",
                                                          "discordoverlay64.dll",
                                                          "overlay64.dll",
                                                          "overlay.dll", // Overlays ended
                                                          "d3d11.dll",
                                                          "d3d12.dll",
                                                          "d3d12core.dll" }; // directx ended
/*
                                                          "fakenvapi.dll", // fakenvapi
                                                          "libxell.dll",
                                                          "libxess.dll",
                                                          "libxess_dx11.dll",
                                                          "igxess.dll",
                                                          "igxess2.dll",
                                                          "libxess_fg.dll",
                                                          "igxess_fg.dll", // xess ended
                                                          "intelcontrollib.dll",
                                                          "igdext64.dll",
                                                          "igdgmm64.dll", // intel drivers ended
                                                          "ffx_fsr2_api_x64.dll",
                                                          "ffx_fsr2_api_dx12_x64.dll",
                                                          "ffx_fsr3upscaler_x64.dll",
                                                          "ffx_backend_dx12_x64.dll",
                                                          "amd_fidelityfx_dx12.dll",
                                                          "amd_fidelityfx_loader_dx12.dll",
                                                          "amd_fidelityfx_upscaler_dx12.dll",
                                                          "amd_fidelityfx_framegeneration_dx12.dll", // fsr ended
                                                          "nvcamera64.dll",                          // nvcamera?
*/

DEFINE_NAME_VECTORS(dx11, "d3d11");
DEFINE_NAME_VECTORS(dx12, "d3d12");
DEFINE_NAME_VECTORS(dx12agility, "d3d12core");
DEFINE_NAME_VECTORS(dxgi, "dxgi");
DEFINE_NAME_VECTORS(vk, "vulkan-1");

DEFINE_NAME_VECTORS(nvngx, "nvngx", "_nvngx");
DEFINE_NAME_VECTORS(nvngxDlss, "nvngx_dlss");
DEFINE_NAME_VECTORS(nvapi, "nvapi64");
DEFINE_NAME_VECTORS(slInterposer, "sl.interposer");
DEFINE_NAME_VECTORS(slDlss, "sl.dlss");
DEFINE_NAME_VECTORS(slDlssg, "sl.dlss_g");
DEFINE_NAME_VECTORS(slReflex, "sl.reflex");
DEFINE_NAME_VECTORS(slPcl, "sl.pcl");
DEFINE_NAME_VECTORS(slCommon, "sl.common");

DEFINE_NAME_VECTORS(xess, "libxess");
DEFINE_NAME_VECTORS(xessDx11, "libxess_dx11");

DEFINE_NAME_VECTORS(fsr2, "ffx_fsr2_api_x64");
DEFINE_NAME_VECTORS(fsr2BE, "ffx_fsr2_api_dx12_x64");

DEFINE_NAME_VECTORS(fsr3, "ffx_fsr3upscaler_x64");
DEFINE_NAME_VECTORS(fsr3BE, "ffx_backend_dx12_x64");

DEFINE_NAME_VECTORS(ffxDx12, "amd_fidelityfx_dx12", "amd_fidelityfx_loader_dx12");
DEFINE_NAME_VECTORS(ffxDx12Upscaler, "amd_fidelityfx_upscaler_dx12");
DEFINE_NAME_VECTORS(ffxDx12FG, "amd_fidelityfx_framegeneration_dx12");
DEFINE_NAME_VECTORS(ffxDx12Denoiser, "amd_fidelityfx_denoiser_dx12");
DEFINE_NAME_VECTORS(ffxVk, "amd_fidelityfx_vk");

inline static bool CompareFileName(std::string* first, std::string* second)
{
    if (first->size() < second->size())
        return false;

    auto start = first->size() - second->size();

    bool match = true;
    for (size_t j = 0; j < second->size(); ++j)
    {
        if (std::tolower(static_cast<unsigned char>((*first)[start + j])) !=
            std::tolower(static_cast<unsigned char>((*second)[j])))
        {
            return false;
        }
    }

    return true;
}

inline static bool CompareFileNameW(std::wstring* first, std::wstring* second)
{
    if (first->size() < second->size())
        return false;

    auto start = first->size() - second->size();

    bool match = true;
    for (size_t j = 0; j < second->size(); ++j)
    {
        if (std::towlower((*first)[start + j]) != std::towlower((*second)[j]))
        {
            return false;
        }
    }

    return true;
}

inline static bool CheckDllName(std::string* dllName, std::vector<std::string>* namesList)
{
    for (auto& name : *namesList)
    {
        if (CompareFileName(dllName, &name))
            return true;
    }

    return false;
}

inline static bool CheckDllNameW(std::wstring* dllName, std::vector<std::wstring>* namesList)
{
    for (auto& name : *namesList)
    {
        if (CompareFileNameW(dllName, &name))
            return true;
    }

    return false;
}

inline static HMODULE GetDllNameModule(std::vector<std::string>* namesList)
{
    for (size_t i = 0; i < namesList->size(); i++)
    {
        auto name = namesList->at(i);
        auto module = KernelBaseProxy::GetModuleHandleA_()(name.c_str());

        if (module != nullptr)
            return module;
    }

    return nullptr;
}

inline static HMODULE GetDllNameWModule(std::vector<std::wstring>* namesList)
{
    for (size_t i = 0; i < namesList->size(); i++)
    {
        auto name = namesList->at(i);
        auto module = KernelBaseProxy::GetModuleHandleW_()(name.c_str());

        if (module != nullptr)
            return module;
    }

    return nullptr;
}
