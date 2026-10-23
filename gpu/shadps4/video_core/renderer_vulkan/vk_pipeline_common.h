// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_backend/binding.h"
#include "video_core/renderer_backend/pipeline.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <boost/container/small_vector.hpp>

namespace Shader {
struct Info;
struct PushData;
} // namespace Shader

namespace Vulkan {

static constexpr auto AllGraphicsStageBits =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry |
    vk::ShaderStageFlagBits::eFragment;

class Instance;
class Scheduler;
class DescriptorHeap;

/// A guest pipeline, made by the render adapter's device from its description (A5).
class Pipeline {
public:
    Pipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
             const Shader::Profile& profile, bool is_compute = false);
    virtual ~Pipeline();

    /// Empty when the driver rejected the pipeline: its draws are skipped.
    vk::Pipeline Handle() const noexcept {
        return pipeline;
    }

    vk::PipelineLayout GetLayout() const noexcept {
        return pipeline_layout;
    }

    auto GetStages() const {
        static_assert(static_cast<u32>(Shader::SwStage::Compute) == Shader::MaxStageTypes - 1);
        if (is_compute) {
            return std::span{stages.cend() - 1, stages.cend()};
        } else {
            return std::span{stages.cbegin(), stages.cend() - 1};
        }
    }

    const Shader::Info& GetStage(Shader::SwStage stage) const noexcept {
        return *stages[u32(stage)];
    }

    bool IsCompute() const {
        return is_compute;
    }

    /// The render adapter's slot writes (docs/RENDER_ADAPTER.md, A4), pushed as descriptors.
    using DescriptorWrites = std::vector<VideoCore::Backend::SlotWrite>;
    /// `image_infos` and `buffer_infos`: the arrays the writes point into, if known (their
    /// descriptors are then copied for the recording thread in one piece each).
    void BindResources(DescriptorWrites& set_writes, const Shader::PushData& push_data,
                       std::span<const VideoCore::Backend::ImageBinding> image_infos = {},
                       std::span<const VideoCore::Backend::BufferBinding> buffer_infos = {}) const;

protected:
    using BindingSlots = boost::container::small_vector<VideoCore::Backend::BindingSlot, 32>;

    [[nodiscard]] std::string GetDebugString() const;
    /// The binding slots of a stage's resources, in the order the recompiler numbers them:
    /// buffers, images, samplers.
    static void AppendBindingSlots(const Shader::Info& info, VideoCore::Backend::ShaderStage stage,
                                   BindingSlots& slots);
    /// Takes the pipeline the device made (nothing for an empty allocation).
    void Adopt(const VideoCore::Backend::PipelineAllocation& allocation);

    const Instance& instance;
    Scheduler& scheduler;
    DescriptorHeap& desc_heap;
    const Shader::Profile& profile;
    VideoCore::Backend::PipelineHandle handle;
    // The device's objects (BackendDevice::PipelineRecord), valid while `handle` is.
    vk::Pipeline pipeline;
    vk::PipelineLayout pipeline_layout;
    vk::DescriptorSetLayout desc_layout;
    std::array<const Shader::Info*, Shader::MaxStageTypes> stages{};
    bool uses_push_descriptors{};
    bool is_compute;
};

} // namespace Vulkan
