//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/host_passes/auto_exposure_pass.h"

#include <array>
#include <cstring>

#include <vk_mem_alloc.h>

#include "common/assert.h"
#include "video_core/host_shaders/auto_exposure_comp.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan::HostPasses {

namespace {

// Layout of the storage buffer the compute shader writes to. Matches the
// struct declared in auto_exposure.comp. Keep in sync if either side
// changes.
struct ExposureBufferContents {
    float smoothed_exposure;
    float last_scene_luma;
    u32 frame_count;
    float last_peak_luma;
};
static_assert(sizeof(ExposureBufferContents) == 16,
              "auto_exposure storage buffer layout drifted");

struct PushConstants {
    float target_luma;
    float adaptation_rate;
    float min_exposure;
    float max_exposure;
};

} // namespace

void AutoExposurePass::Create(vk::Device device_, VmaAllocator allocator_,
                              float initial_exposure) {
    device = device_;
    allocator = allocator_;

    // --- Storage buffer -----------------------------------------------------
    const vk::BufferCreateInfo buffer_ci = {
        .size = sizeof(ExposureBufferContents),
        .usage = vk::BufferUsageFlagBits::eStorageBuffer |
                 vk::BufferUsageFlagBits::eTransferDst,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VkBuffer raw_buffer{};
    VkBufferCreateInfo raw_buffer_ci = static_cast<VkBufferCreateInfo>(buffer_ci);
    VmaAllocationInfo alloc_info{};
    const auto vma_result = vmaCreateBuffer(allocator, &raw_buffer_ci, &alloc_ci, &raw_buffer,
                                            &exposure_allocation, &alloc_info);
    ASSERT_MSG(vma_result == VK_SUCCESS, "vmaCreateBuffer for auto-exposure: {}",
               static_cast<int>(vma_result));
    exposure_buffer = vk::Buffer(raw_buffer);

    mapped_buffer_ptr = alloc_info.pMappedData;
    // Seed with the user's manual exposure and zero frame_count so the
    // compute shader's bootstrap branch snaps to the real scene target on
    // the very first dispatch instead of fading in from the manual value.
    if (mapped_buffer_ptr != nullptr) {
        ExposureBufferContents init{
            .smoothed_exposure = initial_exposure > 0.0f ? initial_exposure : 1.0f,
            .last_scene_luma = 0.0f,
            .frame_count = 0u,
            .last_peak_luma = 0.0f,
        };
        std::memcpy(mapped_buffer_ptr, &init, sizeof(init));
        vmaFlushAllocation(allocator, exposure_allocation, 0, VK_WHOLE_SIZE);
    }

    // --- Descriptor set layout (pushed, not pooled) -------------------------
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    descriptor_set_layout =
        Check<"create auto-exposure descriptor layout">(
            device.createDescriptorSetLayoutUnique(desc_layout_ci));

    // --- Pipeline layout ----------------------------------------------------
    const vk::PushConstantRange push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    const vk::PipelineLayoutCreateInfo pl_layout_ci = {
        .setLayoutCount = 1,
        .pSetLayouts = &*descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    };
    pipeline_layout = Check<"create auto-exposure pipeline layout">(
        device.createPipelineLayoutUnique(pl_layout_ci));

    // --- Compute pipeline ---------------------------------------------------
    const auto shader_module =
        Compile(HostShaders::AUTO_EXPOSURE_COMP, vk::ShaderStageFlagBits::eCompute, device);
    ASSERT(shader_module);
    const vk::PipelineShaderStageCreateInfo stage_ci = {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = shader_module,
        .pName = "main",
    };
    const vk::ComputePipelineCreateInfo pipeline_ci = {
        .stage = stage_ci,
        .layout = *pipeline_layout,
    };
    pipeline = Check<"create auto-exposure compute pipeline">(
        device.createComputePipelineUnique({}, pipeline_ci));
    device.destroyShaderModule(shader_module);

    // --- Sampler for the image read ----------------------------------------
    const vk::SamplerCreateInfo sampler_ci = {
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    sampler = Check<"create auto-exposure sampler">(device.createSamplerUnique(sampler_ci));
}

void AutoExposurePass::Destroy() {
    if (exposure_buffer) {
        vmaDestroyBuffer(allocator, static_cast<VkBuffer>(exposure_buffer), exposure_allocation);
        exposure_buffer = VK_NULL_HANDLE;
        exposure_allocation = VK_NULL_HANDLE;
        mapped_buffer_ptr = nullptr;
    }
    sampler.reset();
    pipeline.reset();
    pipeline_layout.reset();
    descriptor_set_layout.reset();
}

void AutoExposurePass::ReadCurrentState(float& out_exposure, float& out_scene_luma,
                                        u32& out_frame_count, float& out_peak_luma) const {
    if (mapped_buffer_ptr == nullptr) {
        out_exposure = -1.0f;
        out_scene_luma = -1.0f;
        out_frame_count = 0u;
        out_peak_luma = -1.0f;
        return;
    }
    ExposureBufferContents contents{};
    std::memcpy(&contents, mapped_buffer_ptr, sizeof(contents));
    out_exposure = contents.smoothed_exposure;
    out_scene_luma = contents.last_scene_luma;
    out_frame_count = contents.frame_count;
    out_peak_luma = contents.last_peak_luma;
}

void AutoExposurePass::Render(vk::CommandBuffer cmdbuf, vk::ImageView input,
                              vk::Extent2D /*input_size*/, const Settings& settings) {
    const vk::DescriptorImageInfo image_info = {
        .sampler = *sampler,
        .imageView = input,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::DescriptorBufferInfo buffer_info = {
        .buffer = exposure_buffer,
        .offset = 0,
        .range = VK_WHOLE_SIZE,
    };
    const std::array<vk::WriteDescriptorSet, 2> writes{{
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &image_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &buffer_info,
        },
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);

    const PushConstants pc{
        .target_luma = settings.target_luma,
        .adaptation_rate = settings.adaptation_rate,
        .min_exposure = settings.min_exposure,
        .max_exposure = settings.max_exposure,
    };
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(pc), &pc);

    // Single 16x16 workgroup, one dispatch per frame.
    cmdbuf.dispatch(1, 1, 1);

    // Compute write -> fragment shader read synchronization. The pp_pass
    // fragment shader will bind this buffer as a uniform/storage and read
    // the updated smoothed_exposure.
    const vk::MemoryBarrier2 barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}

void AutoExposurePass::WriteManualExposure(vk::CommandBuffer cmdbuf, float exposure) {
    // Use vkCmdUpdateBuffer for the first 4 bytes. Cheap, avoids a
    // map/flush dance and is ordered with subsequent draws on this queue.
    cmdbuf.updateBuffer(exposure_buffer, 0, sizeof(float), &exposure);
    const vk::MemoryBarrier2 barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}

} // namespace Vulkan::HostPasses
