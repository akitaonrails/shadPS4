//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

// Forward-declare VMA's opaque handles the same way vk_instance.h does,
// to keep <vk_mem_alloc.h> out of this public header.
#ifndef VMA_ALLOCATOR_FWD_DECLARED
#define VMA_ALLOCATOR_FWD_DECLARED
VK_DEFINE_HANDLE(VmaAllocator)
#endif
VK_DEFINE_HANDLE(VmaAllocation)

namespace Vulkan::HostPasses {

// Dispatches a small compute pass each frame that reads the game's
// linear framebuffer, computes its log-mean luminance, and updates a
// smoothed exposure value in a persistent storage buffer. The SDR post-
// process shader reads that buffer instead of a fixed push-constant
// exposure, so the scene's apparent brightness stays stable across
// wildly different lighting conditions (dawn, night headlights, bright
// midday) without needing scene-specific tuning.
class AutoExposurePass {
public:
    struct Settings {
        // Arithmetic-mean luma target. 0.18 linear ≈ 0.46 sRGB (mid-grey).
        // The shader clips samples above 0.95 before averaging so saturated
        // HUD whites don't distort the estimate.
        float target_luma{0.18f};
        // Flat exponential smoothing factor per dispatch. 0.15 gives a
        // roughly 0.3 s time constant at 60 fps — slow enough to feel
        // like eye adaptation, fast enough that scene cuts don't leave
        // a visible dim pocket.
        float adaptation_rate{0.15f};
        float min_exposure{0.25f};
        // Max gain. 10x is enough to lift a genuinely dim scene to a
        // readable mid-grey without chasing pure-black frames into the
        // noise floor.
        float max_exposure{10.0f};
    };

    void Create(vk::Device device, VmaAllocator allocator, float initial_exposure);
    void Destroy();

    // Read the current buffer contents from the host-mapped allocation.
    // Used for debug logging of auto-exposure state.
    void ReadCurrentState(float& out_exposure, float& out_scene_luma,
                          u32& out_frame_count, float& out_peak_luma) const;

    // Dispatch the luminance reduction and exposure-update compute. The
    // input view should be the same image that the subsequent post-process
    // pass reads, so exposure and tonemap operate on the same frame.
    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input,
                vk::Extent2D input_size, const Settings& settings);

    // Storage buffer the post-process pass binds in its own descriptor.
    vk::Buffer GetExposureBuffer() const {
        return exposure_buffer;
    }

    // Overwrite the buffer's smoothed_exposure directly (CPU-side). Used
    // when auto-exposure is disabled to pin a manual value, or on
    // initialization.
    void WriteManualExposure(vk::CommandBuffer cmdbuf, float exposure);

private:
    vk::Device device{};
    VmaAllocator allocator{};

    vk::UniqueDescriptorSetLayout descriptor_set_layout{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniquePipeline pipeline{};
    vk::UniqueSampler sampler{};

    // Storage buffer: 16 bytes (float smoothed_exposure, float last_scene_luma,
    // uint frame_count, uint pad). Host-visible for initial write; after that
    // the GPU writes each frame.
    vk::Buffer exposure_buffer{};
    VmaAllocation exposure_allocation{};
    void* mapped_buffer_ptr{nullptr}; // host-visible mapping for debug readback
};

} // namespace Vulkan::HostPasses
