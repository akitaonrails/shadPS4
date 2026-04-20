//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Frame;
}

namespace Vulkan::HostPasses {

class PostProcessingPass {
public:
    struct Settings {
        float gamma = 1.0f;
        float exposure = 1.0f;
        u32 hdr = 0;
        u32 tonemap_mode = 0; // 0 = per-channel ACES, 1 = luma-preserving ACES
        u32 bypass = 0;       // 1 = skip exposure/tonemap; output raw sRGB-encoded input
    };

    void Create(vk::Device device, vk::Format surface_format);

    // `auto_exposure_buffer` is a storage buffer containing the smoothed
    // auto-exposure value the shader multiplies with `settings.exposure`.
    // See AutoExposurePass.
    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                Frame& output, Settings settings, vk::Buffer auto_exposure_buffer);

private:
    vk::UniquePipeline pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
};

} // namespace Vulkan::HostPasses
