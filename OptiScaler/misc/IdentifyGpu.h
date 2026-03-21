#pragma once
#include <nvapi.h>

// vkd3d-proton
MIDL_INTERFACE("39da4e09-bd1c-4198-9fae-86bbe3be41fd")
ID3D12DXVKInteropDevice : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetDXGIAdapter(REFIID iid, void** ppvObject) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetInstanceExtensions(UINT * pExtensionCount, const char** ppExtensions) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetDeviceExtensions(UINT * pExtensionCount, const char** ppExtensions) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetDeviceFeatures(const VkPhysicalDeviceFeatures2** ppFeatures) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetVulkanHandles(VkInstance * pVkInstance, VkPhysicalDevice * pVkPhysicalDevice,
                                                       VkDevice * pVkDevice) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetVulkanQueueInfo(ID3D12CommandQueue * pCommandQueue, VkQueue * pVkQueue,
                                                         UINT32 * pVkQueueFamily) = 0;

    virtual void STDMETHODCALLTYPE GetVulkanImageLayout(ID3D12Resource * pResource, D3D12_RESOURCE_STATES State,
                                                        VkImageLayout * pVkLayout) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetVulkanResourceInfo(ID3D12Resource * pResource, UINT64 * pVkHandle,
                                                            UINT64 * pBufferOffset) = 0;

    virtual HRESULT STDMETHODCALLTYPE LockCommandQueue(ID3D12CommandQueue * pCommandQueue) = 0;

    virtual HRESULT STDMETHODCALLTYPE UnlockCommandQueue(ID3D12CommandQueue * pCommandQueue) = 0;
};

// dxvk
MIDL_INTERFACE("907bf281-ea3c-43b4-a8e4-9f231107b4ff")
IDXGIDXVKAdapter : public IDXGIAdapter4
{
    virtual void* STDMETHODCALLTYPE GetDXVKAdapter() = 0;

    virtual void* STDMETHODCALLTYPE GetDXVKInstance() = 0;
};

struct GpuInformation
{
    LUID luid {}; // Unique id to be able to reference the exact GPU, VkPhysicalDeviceIDProperties
    std::string name {};
    VendorId::Value vendorId = VendorId::Invalid;
    uint32_t deviceId = 0x0;
    uint32_t subsystemId = 0x0;
    uint32_t revisionId = 0x0;
    size_t dedicatedVramInBytes = 0;
    bool usesDxvk = false;
    bool usesVkd3dProton = false;
    bool softwareAdapter = false;

    bool fsr4Capable = false;
    ID3D12Device* d3d12device = nullptr;

    bool dlssCapable = false;
    NV_GPU_ARCH_INFO nvidiaArchInfo {};
    bool noDisplayConnected = false;
};

// - Check if FSR 4 is supported by using amdxc64, watch out for linux memes on proton experimental that don't check
// vulkan caps
// - Check Nvidia Arch, watch out for fakenvapi
// - Check vram amount
// - Check if dxgi uses dxvk, some dxvk specific call?
// - Check if vkd3d-proton is being used? Could be helpful to display in menu
// - Check vulkan driver? radv vs amdvlk etc
// - Look at removing state.DeviceAdapterNames
// - Look into IFeature_Dx11wDx12::getHardwareAdapter
//
// - Check hags support? watch out for linux
// - Opti in many spots assumes a single GPU and that all handles are coming from that gpu,
// might need to always check if LUID of the held device matches the one provided by this class
// before trying to use any info from here. Could also create a method to query GpuInformation based on LUID
// - Consider adding resetCache in case primary GPU has changed somehow + some way to tell IdentifyGpu
// which GPU is the primary one. Seems mostly useful in cases where the game would manually chose a different GPU.
// Would require removing "static" from calls to getPrimaryGpu().

inline constexpr bool IsEqualLUID(LUID luid1, LUID luid2)
{
    return luid1.HighPart == luid2.HighPart && luid1.LowPart == luid2.LowPart;
}

class IdentifyGpu
{
    static std::vector<GpuInformation> checkGpuInfo();
    static std::vector<GpuInformation> checkGpuInfoVulkan();
    static void queryNvapi(GpuInformation& gpuInfo);

  public:
    static void getHardwareAdapter(IDXGIFactory* InFactory, IDXGIAdapter** InAdapter,
                                   D3D_FEATURE_LEVEL requiredFeatureLevel);

    // Sorted by priority, the first one should be treated as the primary one
    static std::vector<GpuInformation> getAllGpus();
    static GpuInformation getPrimaryGpu();
    static std::vector<GpuInformation> getAllGpusVulkan();
    static GpuInformation getPrimaryGpuVulkan();
};
