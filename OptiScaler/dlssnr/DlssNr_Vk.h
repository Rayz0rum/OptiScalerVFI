#pragma once

#include <vulkan/vulkan.h>
#include <nvsdk_ngx.h>

// DLSS 5 Neural Rendering on Vulkan.
//
// The same pass as the D3D modules, on a Vulkan game's own device. The snippet exports a complete
// Vulkan surface, so nothing here is bridged.
//
// The instance, physical device and device are passed in rather than read from a global: the Vulkan
// input path already holds the ones the game initialised NGX with, and those are the only ones the
// model's feature is valid against.
namespace DlssNr
{
void EvaluateAfterUpscaleVk(VkCommandBuffer cmd, NVSDK_NGX_Parameter* params, VkInstance instance,
                            VkPhysicalDevice physicalDevice, VkDevice device);

bool IsRunningVk();
const char* FailureReasonVk();

// Clears the session failure latch, so a failure caused by transient thrash does not cost a restart.
void RetryAfterFailureVk();

void ShutdownVk();
} // namespace DlssNr
