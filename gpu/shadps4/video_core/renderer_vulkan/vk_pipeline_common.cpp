// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <span>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state.h"

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;


void Pipeline::BindResources(DescriptorWrites& set_writes, const Shader::PushData& push_data,
                             std::span<const VideoCore::Backend::ImageBinding> image_infos,
                             std::span<const VideoCore::Backend::BufferBinding> buffer_infos) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = *pipeline_layout;
    scheduler.Record([layout, stage_flags, push_data](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }
    // The adapter's bindings are laid out like Vulkan's descriptor infos (vk_state.h).
    const std::span<const vk::DescriptorImageInfo> vk_images{
        reinterpret_cast<const vk::DescriptorImageInfo*>(image_infos.data()), image_infos.size()};
    const std::span<const vk::DescriptorBufferInfo> vk_buffers{
        reinterpret_cast<const vk::DescriptorBufferInfo*>(buffer_infos.data()),
        buffer_infos.size()};

    if (uses_push_descriptors && scheduler.IsRecordingDeferred()) {
        // Writes and the infos they point to are laid out in the recording chunk, with the
        // pointers already aimed at those copies: the command only carries a span. The writes
        // are converted straight into the chunk.
        // bbport: when every write points into the caller's info arrays (draws and
        // dispatches), each array is copied once and the pointers are rebased: a copy per
        // descriptor (two per write) cost the draw recording thread ~3% of its time.
        const auto within = [](const auto* p, u32 count, const auto& array) {
            return p >= array.data() && p + count <= array.data() + array.size();
        };
        bool bulk = !image_infos.empty() || !buffer_infos.empty();
        for (const auto& write : set_writes) {
            if (!bulk) {
                break;
            }
            bulk = (!write.images || within(write.images, write.count, image_infos)) &&
                   (!write.buffers || within(write.buffers, write.count, buffer_infos));
        }
        const size_t count = set_writes.size();
        if (bulk) {
            scheduler.ReserveRecordData(count * sizeof(vk::WriteDescriptorSet) +
                                        vk_images.size_bytes() + vk_buffers.size_bytes() + 256);
            const auto images = scheduler.RecordData(vk_images);
            const auto buffers = scheduler.RecordData(vk_buffers);
            auto* writes = scheduler.AllocateRecordData<vk::WriteDescriptorSet>(count);
            for (size_t i = 0; i < count; ++i) {
                const auto& write = set_writes[i];
                std::construct_at(&writes[i], ToVk(write));
                if (write.images) {
                    writes[i].pImageInfo = images.data() + (write.images - image_infos.data());
                }
                if (write.buffers) {
                    writes[i].pBufferInfo = buffers.data() + (write.buffers - buffer_infos.data());
                }
            }
            scheduler.Record([bind_point, layout, writes = std::span{writes, count}](
                                 vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
            });
            return;
        }
        size_t bytes = count * sizeof(vk::WriteDescriptorSet) + 64;
        for (const auto& write : set_writes) {
            bytes += write.count * (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo)) +
                     32;
        }
        scheduler.ReserveRecordData(bytes);
        auto* writes = scheduler.AllocateRecordData<vk::WriteDescriptorSet>(count);
        for (size_t i = 0; i < count; ++i) {
            std::construct_at(&writes[i], ToVk(set_writes[i]));
            auto& write = writes[i];
            if (write.pBufferInfo) {
                write.pBufferInfo =
                    scheduler.RecordData(std::span{write.pBufferInfo, write.descriptorCount}).data();
            }
            if (write.pImageInfo) {
                write.pImageInfo =
                    scheduler.RecordData(std::span{write.pImageInfo, write.descriptorCount}).data();
            }
        }
        scheduler.Record([bind_point, layout, writes = std::span{writes, count}](
                             vk::CommandBuffer cmdbuf) {
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
        });
        return;
    }

    boost::container::small_vector<vk::WriteDescriptorSet, 64> vk_writes;
    vk_writes.reserve(set_writes.size());
    for (const auto& write : set_writes) {
        vk_writes.push_back(ToVk(write));
    }
    if (uses_push_descriptors) {
        scheduler.Record([&](vk::CommandBuffer cmdbuf) {
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, vk_writes);
        });
        return;
    }

    // Descriptor set updates are device calls: they stay on this thread.
    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : vk_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(vk_writes, {});
    scheduler.Record([bind_point, layout, desc_set](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
    });
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
