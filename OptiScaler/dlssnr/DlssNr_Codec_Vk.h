#pragma once

// The colour codec, on Vulkan.
//
// Third and last binding of the same shader. D3D11 and D3D12 compile codec::kShaderSource at runtime;
// Vulkan cannot, so it is compiled ahead of time into DlssNr_Codec_Vk_Bytecode.h by
// shaders/dlssnr/build_vk_shader.pl. That script extracts the string from DlssNr_Codec.h rather than
// keeping a copy, so there is still exactly one shader and no way for this backend to drift.
//
// The shader source needs no Vulkan-specific markup. It declares plain `register(t0)` bindings, and
// dxc's register shifts lay each class out in its own run:
//
//     binding 0      constants          (cbuffer  b0)
//     binding 1..4   source, model, original, motion   (Texture2D    t0..t3)
//     binding 5..6   target, keep       (RWTexture2D  u0..u1)
//     binding 7      linear sampler     (SamplerState s0)
//
// The layout below must match that table; the script's header comment carries it too, because a
// mismatch here produces no validation error, just wrong pixels.

#include <vulkan/vulkan.h>

#include "DlssNr_Codec.h"
#include "DlssNr_Codec_Vk_Bytecode.h"

namespace codec
{

class Codec_Vk
{
  public:
    // Descriptor sets are cycled rather than rewritten, for the same reason the D3D12 codec rings its
    // descriptor heap: the previous dispatch may still be reading the set it was given.
    static constexpr uint32_t kRingSlots = 8;

    bool ensure(VkDevice device, VkPhysicalDevice physicalDevice)
    {
        if (pipeline_ != VK_NULL_HANDLE)
            return true;

        if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE)
            return false;

        device_ = device;

        VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        moduleInfo.codeSize = sizeof(kCodecSpirv);
        moduleInfo.pCode = kCodecSpirv;

        if (vkCreateShaderModule(device_, &moduleInfo, nullptr, &module_) != VK_SUCCESS)
            return false;

        VkDescriptorSetLayoutBinding bindings[8] = {};

        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        for (uint32_t i = 1; i <= 4; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        for (uint32_t i = 5; i <= 6; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        bindings[7].binding = 7;
        bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[7].descriptorCount = 1;
        bindings[7].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.bindingCount = 8;
        layoutInfo.pBindings = bindings;

        if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &setLayout_) != VK_SUCCESS)
        {
            release();
            return false;
        }

        VkPipelineLayoutCreateInfo pipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &setLayout_;

        if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS)
        {
            release();
            return false;
        }

        VkComputePipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module_;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = pipelineLayout_;

        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_) !=
            VK_SUCCESS)
        {
            release();
            return false;
        }

        VkDescriptorPoolSize sizes[4] = {};
        sizes[0] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kRingSlots };
        sizes[1] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kRingSlots * 4 };
        sizes[2] = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kRingSlots * 2 };
        sizes[3] = { VK_DESCRIPTOR_TYPE_SAMPLER, kRingSlots };

        VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets = kRingSlots;
        poolInfo.poolSizeCount = 4;
        poolInfo.pPoolSizes = sizes;

        if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_) != VK_SUCCESS)
        {
            release();
            return false;
        }

        VkDescriptorSetLayout layouts[kRingSlots];

        for (auto& layout : layouts)
            layout = setLayout_;

        VkDescriptorSetAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool = pool_;
        allocInfo.descriptorSetCount = kRingSlots;
        allocInfo.pSetLayouts = layouts;

        if (vkAllocateDescriptorSets(device_, &allocInfo, sets_) != VK_SUCCESS)
        {
            release();
            return false;
        }

        VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;

        if (vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS)
        {
            release();
            return false;
        }

        // One host-visible buffer, carved into a slot per ring entry. Each slot has to start on the
        // device's uniform-buffer alignment, which is why the stride is not simply sizeof(Params).
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(physicalDevice, &props);

        const VkDeviceSize align = props.limits.minUniformBufferOffsetAlignment;
        constantStride_ = align != 0 ? ((sizeof(Params) + align - 1) / align) * align : sizeof(Params);

        VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = constantStride_ * kRingSlots;
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device_, &bufferInfo, nullptr, &constants_) != VK_SUCCESS)
        {
            release();
            return false;
        }

        VkMemoryRequirements requirements = {};
        vkGetBufferMemoryRequirements(device_, constants_, &requirements);

        VkPhysicalDeviceMemoryProperties memoryProps = {};
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProps);

        uint32_t typeIndex = UINT32_MAX;
        const VkMemoryPropertyFlags wanted =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        for (uint32_t i = 0; i < memoryProps.memoryTypeCount; ++i)
        {
            if ((requirements.memoryTypeBits & (1u << i)) == 0)
                continue;

            if ((memoryProps.memoryTypes[i].propertyFlags & wanted) == wanted)
            {
                typeIndex = i;
                break;
            }
        }

        if (typeIndex == UINT32_MAX)
        {
            release();
            return false;
        }

        VkMemoryAllocateInfo memoryInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        memoryInfo.allocationSize = requirements.size;
        memoryInfo.memoryTypeIndex = typeIndex;

        if (vkAllocateMemory(device_, &memoryInfo, nullptr, &constantsMemory_) != VK_SUCCESS ||
            vkBindBufferMemory(device_, constants_, constantsMemory_, 0) != VK_SUCCESS ||
            vkMapMemory(device_, constantsMemory_, 0, VK_WHOLE_SIZE, 0, &constantsMapped_) != VK_SUCCESS)
        {
            release();
            return false;
        }

        return true;
    }

    // Every image must already be in VK_IMAGE_LAYOUT_GENERAL. That is a requirement for the storage
    // images and merely permitted for the sampled ones, so using it throughout keeps the caller from
    // having to track two layouts for surfaces that swap roles between passes.
    void dispatch(VkCommandBuffer cmd, const Params& constants, VkImageView source, VkImageView model,
                  VkImageView original, VkImageView motion, VkImageView target, VkImageView keep)
    {
        if (pipeline_ == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE || source == VK_NULL_HANDLE ||
            target == VK_NULL_HANDLE)
            return;

        const uint32_t slot = ring_;
        ring_ = (ring_ + 1) % kRingSlots;

        memcpy((char*) constantsMapped_ + (size_t) slot * constantStride_, &constants, sizeof(Params));

        // Absent optional inputs resolve to `source`. The shader indexes all four unconditionally, and
        // an unbound descriptor is undefined behaviour rather than a convenient zero.
        VkDescriptorImageInfo sampled[4] = {};
        sampled[0].imageView = source;
        sampled[1].imageView = model != VK_NULL_HANDLE ? model : source;
        sampled[2].imageView = original != VK_NULL_HANDLE ? original : source;
        sampled[3].imageView = motion != VK_NULL_HANDLE ? motion : source;

        for (auto& info : sampled)
            info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo storage[2] = {};
        storage[0].imageView = target;
        storage[1].imageView = keep != VK_NULL_HANDLE ? keep : target;

        for (auto& info : storage)
            info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo samplerInfo = {};
        samplerInfo.sampler = sampler_;

        VkDescriptorBufferInfo bufferInfo = {};
        bufferInfo.buffer = constants_;
        bufferInfo.offset = (VkDeviceSize) slot * constantStride_;
        bufferInfo.range = sizeof(Params);

        VkWriteDescriptorSet writes[8] = {};

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = sets_[slot];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &bufferInfo;

        for (uint32_t i = 0; i < 4; ++i)
        {
            writes[1 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1 + i].dstSet = sets_[slot];
            writes[1 + i].dstBinding = 1 + i;
            writes[1 + i].descriptorCount = 1;
            writes[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[1 + i].pImageInfo = &sampled[i];
        }

        for (uint32_t i = 0; i < 2; ++i)
        {
            writes[5 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[5 + i].dstSet = sets_[slot];
            writes[5 + i].dstBinding = 5 + i;
            writes[5 + i].descriptorCount = 1;
            writes[5 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[5 + i].pImageInfo = &storage[i];
        }

        writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[7].dstSet = sets_[slot];
        writes[7].dstBinding = 7;
        writes[7].descriptorCount = 1;
        writes[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[7].pImageInfo = &samplerInfo;

        vkUpdateDescriptorSets(device_, 8, writes, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &sets_[slot],
                                0, nullptr);
        vkCmdDispatch(cmd, (constants.width + 7) / 8, (constants.height + 7) / 8, 1);
    }

    void release()
    {
        if (device_ == VK_NULL_HANDLE)
            return;

        if (constantsMapped_ != nullptr)
        {
            vkUnmapMemory(device_, constantsMemory_);
            constantsMapped_ = nullptr;
        }

        auto drop = [&](auto& handle, auto fn)
        {
            if (handle != VK_NULL_HANDLE)
            {
                fn(device_, handle, nullptr);
                handle = VK_NULL_HANDLE;
            }
        };

        drop(constants_, vkDestroyBuffer);
        drop(constantsMemory_, vkFreeMemory);
        drop(sampler_, vkDestroySampler);
        // The sets go with the pool; freeing them individually needs a flag the pool was not made with.
        drop(pool_, vkDestroyDescriptorPool);
        drop(pipeline_, vkDestroyPipeline);
        drop(pipelineLayout_, vkDestroyPipelineLayout);
        drop(setLayout_, vkDestroyDescriptorSetLayout);
        drop(module_, vkDestroyShaderModule);

        for (auto& set : sets_)
            set = VK_NULL_HANDLE;

        ring_ = 0;
        device_ = VK_NULL_HANDLE;
    }

    ~Codec_Vk() { release(); }

  private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkShaderModule module_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkBuffer constants_ = VK_NULL_HANDLE;
    VkDeviceMemory constantsMemory_ = VK_NULL_HANDLE;
    void* constantsMapped_ = nullptr;
    VkDeviceSize constantStride_ = 0;

    VkDescriptorSet sets_[kRingSlots] = {};
    uint32_t ring_ = 0;
};

} // namespace codec
