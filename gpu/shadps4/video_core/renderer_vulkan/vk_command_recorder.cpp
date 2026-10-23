// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_breadcrumbs.h"
#include "video_core/renderer_vulkan/vk_command_recorder.h"
#include "video_core/renderer_vulkan/vk_gpu_labels.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state.h"

#include <memory>

namespace Vulkan {

namespace Backend = VideoCore::Backend;

static_assert(s32(vk::IndexType::eUint16) == s32(Backend::IndexType::UInt16) &&
              s32(vk::IndexType::eUint32) == s32(Backend::IndexType::UInt32));
static_assert(s32(vk::VertexInputRate::eVertex) == s32(Backend::VertexInputRate::Vertex) &&
              s32(vk::VertexInputRate::eInstance) == s32(Backend::VertexInputRate::Instance));

namespace {
vk::Buffer AsBuffer(u64 native) {
    return vk::Buffer{reinterpret_cast<VkBuffer>(native)};
}
vk::Pipeline AsPipeline(u64 native) {
    return vk::Pipeline{reinterpret_cast<VkPipeline>(native)};
}
} // namespace

// Each form captures only what it records: recorded commands live in the recording chunks, and
// larger captures fill them sooner (an 80-byte capture per draw raised recording from ~4.2 to
// ~5.8 us a draw).
void CommandRecorder::Draw(const Backend::DrawCommand& d, const Backend::Crumb& crumb) {
    const vk::Pipeline pipeline = AsPipeline(d.pipeline);
    if (!d.args_buffer) {
        scheduler.RecordCrumb(crumb, [pipeline, indexed = d.indexed, count = d.count,
                                      instances = d.instances, first = d.first,
                                      first_instance = d.first_instance](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
            if (indexed) {
                cmdbuf.drawIndexed(count, instances, 0, s32(first), first_instance);
            } else {
                cmdbuf.draw(count, instances, first, first_instance);
            }
        });
        return;
    }
    scheduler.RecordCrumb(crumb, [pipeline, indexed = d.indexed, args = AsBuffer(d.args_buffer),
                                  args_offset = d.args_offset, counts = AsBuffer(d.count_buffer),
                                  counts_offset = d.count_offset, max_draws = d.max_draws,
                                  stride = d.stride](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        if (counts) {
            if (indexed) {
                cmdbuf.drawIndexedIndirectCount(args, args_offset, counts, counts_offset, max_draws,
                                                stride);
            } else {
                cmdbuf.drawIndirectCount(args, args_offset, counts, counts_offset, max_draws, stride);
            }
        } else if (indexed) {
            cmdbuf.drawIndexedIndirect(args, args_offset, max_draws, stride);
        } else {
            cmdbuf.drawIndirect(args, args_offset, max_draws, stride);
        }
    });
}

void CommandRecorder::Dispatch(const Backend::DispatchCommand& d, const Backend::Crumb& crumb) {
    const vk::Pipeline pipeline = AsPipeline(d.pipeline);
    if (!d.args_buffer) {
        scheduler.RecordCrumb(crumb, [pipeline, x = d.groups[0], y = d.groups[1],
                                      z = d.groups[2]](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
            cmdbuf.dispatch(x, y, z);
        });
        return;
    }
    scheduler.RecordCrumb(crumb, [pipeline, args = AsBuffer(d.args_buffer),
                                  args_offset = d.args_offset,
                                  raw = AsBuffer(d.crumb_args_buffer),
                                  raw_offset = d.crumb_args_offset,
                                  slot = crumb.args_slot](vk::CommandBuffer cmdbuf) {
        Breadcrumbs::CopyArgs(cmdbuf, raw, raw_offset, slot); // what the game wrote
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        cmdbuf.dispatchIndirect(args, args_offset);
    });
}

void CommandRecorder::BindIndexBuffer(u64 buffer, u64 offset, Backend::IndexType type) {
    scheduler.Record([handle = AsBuffer(buffer), offset, type](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindIndexBuffer(handle, offset, static_cast<vk::IndexType>(type));
    });
}

void CommandRecorder::BindVertexBuffers(std::span<const u64> buffers, std::span<const u64> offsets,
                                        std::span<const u64> sizes, std::span<const u64> strides) {
    const u32 count = static_cast<u32>(buffers.size());
    // Handles and sizes are 64-bit like u64: read in place.
    const auto vk_buffers = scheduler.RecordData(
        std::span<const vk::Buffer>{reinterpret_cast<const vk::Buffer*>(buffers.data()), count});
    const auto vk_offsets = scheduler.RecordData(std::span<const vk::DeviceSize>{offsets});
    if (sizes.empty()) {
        scheduler.Record([count, vk_buffers, vk_offsets](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindVertexBuffers(0, count, vk_buffers.data(), vk_offsets.data());
        });
        return;
    }
    const auto vk_sizes = scheduler.RecordData(std::span<const vk::DeviceSize>{sizes});
    const auto vk_strides = scheduler.RecordData(std::span<const vk::DeviceSize>{strides});
    scheduler.Record([count, vk_buffers, vk_offsets, vk_sizes, vk_strides](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindVertexBuffers2(0, count, vk_buffers.data(), vk_offsets.data(), vk_sizes.data(),
                                  vk_strides.data());
    });
}

void CommandRecorder::SetVertexInput(std::span<const Backend::VertexBinding> bindings,
                                     std::span<const Backend::VertexAttribute> attributes) {
    // Converted straight into the recording chunk (as the descriptor writes).
    const size_t num_bindings = bindings.size();
    const size_t num_attributes = attributes.size();
    scheduler.ReserveRecordData(num_bindings * sizeof(vk::VertexInputBindingDescription2EXT) +
                                num_attributes * sizeof(vk::VertexInputAttributeDescription2EXT) +
                                64);
    auto* vk_bindings =
        scheduler.AllocateRecordData<vk::VertexInputBindingDescription2EXT>(num_bindings);
    for (size_t i = 0; i < num_bindings; ++i) {
        const auto& b = bindings[i];
        std::construct_at(&vk_bindings[i], vk::VertexInputBindingDescription2EXT{
                                               .binding = b.binding,
                                               .stride = b.stride,
                                               .inputRate = static_cast<vk::VertexInputRate>(b.input_rate),
                                               .divisor = b.divisor,
                                           });
    }
    auto* vk_attributes =
        scheduler.AllocateRecordData<vk::VertexInputAttributeDescription2EXT>(num_attributes);
    for (size_t i = 0; i < num_attributes; ++i) {
        const auto& a = attributes[i];
        std::construct_at(&vk_attributes[i], vk::VertexInputAttributeDescription2EXT{
                                                 .location = a.location,
                                                 .binding = a.binding,
                                                 .format = ToVk(a.format),
                                                 .offset = a.offset,
                                             });
    }
    scheduler.Record([b = std::span{vk_bindings, num_bindings},
                      a = std::span{vk_attributes, num_attributes}](vk::CommandBuffer cmdbuf) {
        cmdbuf.setVertexInputEXT(b, a);
    });
}

void CommandRecorder::WriteLabel(u64 buffer, u64 offset, u64 value, u32 num_bytes) {
    scheduler.Record([buffer = AsBuffer(buffer), offset, value, num_bytes,
                      marker = buffer_markers](vk::CommandBuffer cmdbuf) {
        if (!marker) {
            RecordPortableGpuLabel(cmdbuf, buffer, offset, value, num_bytes);
            return;
        }
        // High half first: whoever sees the new low half (the guest polls that) sees the new
        // high half too.
        if (num_bytes == 8) {
            cmdbuf.writeBufferMarkerAMD(vk::PipelineStageFlagBits::eBottomOfPipe, buffer,
                                        offset + 4, static_cast<u32>(value >> 32));
        }
        cmdbuf.writeBufferMarkerAMD(vk::PipelineStageFlagBits::eBottomOfPipe, buffer, offset,
                                    static_cast<u32>(value));
    });
}

void CommandRecorder::BeginLabel(const char* name) {
    scheduler.CommandBuffer().beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{.pLabelName = name});
}

void CommandRecorder::EndLabel() {
    scheduler.CommandBuffer().endDebugUtilsLabelEXT();
}

void CommandRecorder::InsertLabel(const char* name, u32 color) {
    vk::DebugUtilsLabelEXT label{.pLabelName = name};
    if (color != 0) {
        label.color = std::array<f32, 4>({f32((color >> 16) & 0xff) / 255.0f,
                                          f32((color >> 8) & 0xff) / 255.0f,
                                          f32(color & 0xff) / 255.0f,
                                          f32((color >> 24) & 0xff) / 255.0f});
    }
    scheduler.CommandBuffer().insertDebugUtilsLabelEXT(label);
}

} // namespace Vulkan
