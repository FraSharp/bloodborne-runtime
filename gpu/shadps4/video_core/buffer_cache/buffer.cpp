// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <array>
#include <cstdlib>
#include <numeric>

#include "bbport_metal_residency.h"
#include "common/alignment.h"
#include "common/assert.h"
#include "video_core/buffer_cache/buffer.h"
#include "bbport_guest_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_resource_backend.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

std::string_view BufferTypeName(MemoryType type) {
    switch (type) {
    case MemoryType::HostUncached:
        return "HostUncached";
    case MemoryType::HostCached:
        return "HostCached";
    case MemoryType::Stream:
        return "Stream";
    case MemoryType::DeviceLocal:
        return "DeviceLocal";
    case MemoryType::Sparse:
        return "Sparse";
    default:
        return "Invalid";
    }
}

Buffer::Buffer(const Vulkan::Instance& instance, u64 size_bytes_, vk::DeviceMemory memory,
               std::string_view debug_name)
    : size_bytes{size_bytes_}, mem_type{MemoryType::HostCached},
      buffer{instance.GetDevice(), instance.GetAllocator()} {
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = BbGuestMemory::HandleType(),
    };
    // bbport BB_LAYER_MEMORY: what the GPU binds for the game's memory in place (no arena):
    // vertex, index and indirect data and device addresses too (the memory has them).
    const bool layer = BbGuestMemory::LayerMemory();
    vk::BufferUsageFlags usage =
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eUniformBuffer |
        vk::BufferUsageFlagBits::eUniformTexelBuffer | vk::BufferUsageFlagBits::eStorageTexelBuffer;
    if (layer) {
        usage |= vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
                 vk::BufferUsageFlagBits::eIndirectBuffer |
                 vk::BufferUsageFlagBits::eShaderDeviceAddress;
    }
    const vk::BufferCreateInfo buffer_ci = {
        .pNext = &external,
        .size = size_bytes,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const auto device = instance.GetDevice();
    buffer.buffer = Vulkan::Check(device.createBuffer(buffer_ci));
    const auto result = device.bindBufferMemory(buffer.buffer, memory, 0);
    ASSERT_MSG(result == vk::Result::eSuccess, "Binding a guest memory chunk failed: {}",
               vk::to_string(result));
    if (layer) {
        buffer.bda_addr = device.getBufferAddress({.buffer = buffer.buffer});
    }
    Vulkan::SetObjectName(device, Handle(), debug_name);
    is_coherent = true;
}

Buffer::Buffer(const Vulkan::Instance& instance, void* host_memory, u64 size_bytes_,
               std::string_view debug_name)
    : size_bytes{size_bytes_}, mem_type{MemoryType::HostCached},
      buffer{instance.GetDevice(), instance.GetAllocator()} {
    const auto device = instance.GetDevice();
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
    };
    const vk::BufferCreateInfo buffer_ci = {
        .pNext = &external,
        .size = size_bytes,
        .usage = AllFlags | vk::BufferUsageFlagBits::eUniformTexelBuffer |
                 vk::BufferUsageFlagBits::eStorageTexelBuffer,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    buffer.buffer = Vulkan::Check(device.createBuffer(buffer_ci));
    const auto [props_result, props] = device.getMemoryHostPointerPropertiesEXT(
        vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, host_memory);
    ASSERT_MSG(props_result == vk::Result::eSuccess && props.memoryTypeBits != 0,
               "Host memory cannot be imported: {}", vk::to_string(props_result));
    const auto requirements = device.getBufferMemoryRequirements(buffer.buffer);
    const u32 types = props.memoryTypeBits & requirements.memoryTypeBits;
    ASSERT_MSG(types != 0, "No memory type for imported host memory");
    const vk::ImportMemoryHostPointerInfoEXT import_info{
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
        .pHostPointer = host_memory,
    };
    const vk::MemoryAllocateFlagsInfo flags_info{
        .pNext = &import_info,
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    const auto [memory_result, memory] = device.allocateMemory({
        .pNext = &flags_info,
        .allocationSize = size_bytes,
        .memoryTypeIndex = static_cast<u32>(std::countr_zero(types)),
    });
    ASSERT_MSG(memory_result == vk::Result::eSuccess, "Importing host memory failed: {}",
               vk::to_string(memory_result));
    const auto bind_result = device.bindBufferMemory(buffer.buffer, memory, 0);
    ASSERT_MSG(bind_result == vk::Result::eSuccess, "Binding imported host memory failed: {}",
               vk::to_string(bind_result));
    buffer.bda_addr = device.getBufferAddress({.buffer = buffer.buffer});
    ASSERT_MSG(buffer.bda_addr != 0, "Failed to get buffer device address");
    mapped_data = std::span<u8>{static_cast<u8*>(host_memory), size_bytes};
    Vulkan::SetObjectName(device, Handle(), debug_name);
    // bbport: wired once instead of for every command buffer that uses it (BB_RESIDENCY_SET).
    if (BbMetalResidency::AddMemory(memory)) {
        std::printf("GPU: %s (%llu MiB) kept resident (Metal residency set)\n",
                    std::string{debug_name}.c_str(),
                    static_cast<unsigned long long>(size_bytes >> 20));
    }
    is_coherent = true; // the memory lives for the process: not freed with the buffer
}

Buffer::Buffer(const Vulkan::Instance& instance, VAddr cpu_addr_, u64 size_bytes_,
               MemoryType mem_type_, std::string_view debug_name)
    : cpu_addr{cpu_addr_}, size_bytes{size_bytes_}, mem_type{mem_type_},
      buffer{instance.GetDevice(), instance.GetAllocator()} {
    VmaAllocationInfo alloc_info{};
    static const bool use_resource_desc = [] {
        const char* value = std::getenv("BB_N4_BUFFER_DESC");
        return value && value[0] == '1';
    }();
    if (use_resource_desc) {
        using Backend::BufferUse;
        constexpr auto uses = BufferUse::TransferSource | BufferUse::TransferDestination |
                              BufferUse::Uniform | BufferUse::Storage | BufferUse::Vertex |
                              BufferUse::Index | BufferUse::Indirect | BufferUse::DeviceAddress;
        const Backend::BufferDesc desc{.size_bytes = size_bytes,
                                       .uses = uses,
                                       .memory = mem_type,
                                       .concurrent_readback = mem_type == MemoryType::Sparse};
        Vulkan::CreateBufferFromDesc(instance, buffer, desc, &alloc_info);
    } else {
        vk::BufferCreateInfo buffer_ci = {
            .size = size_bytes,
            .usage = AllFlags,
            .sharingMode = vk::SharingMode::eExclusive,
        };
        // bbport: arenas are also read by the readback queue (BufferCache::DownloadMemory).
        const std::array<u32, 2> families = {instance.GetGraphicsQueueFamilyIndex(),
                                             instance.GetReadbackQueueFamilyIndex()};
        if (mem_type == MemoryType::Sparse && instance.GetReadbackQueue()) {
            buffer_ci.sharingMode = vk::SharingMode::eConcurrent;
            buffer_ci.queueFamilyIndexCount = static_cast<u32>(families.size());
            buffer_ci.pQueueFamilyIndices = families.data();
        }
        buffer.Create(buffer_ci, mem_type, &alloc_info);
    }

    const auto device = instance.GetDevice();
    if (!debug_name.empty()) {
        Vulkan::SetObjectName(device, Handle(), debug_name);
    } else {
        Vulkan::SetObjectName(device, Handle(), "Buffer {:#x}:{:#x}", cpu_addr, size_bytes);
    }

    if (mem_type != MemoryType::Sparse) {
        VkMemoryPropertyFlags property_flags{};
        vmaGetAllocationMemoryProperties(instance.GetAllocator(), buffer.allocation,
                                         &property_flags);
        if (alloc_info.pMappedData) {
            mapped_data = std::span<u8>{std::bit_cast<u8*>(alloc_info.pMappedData), size_bytes};
        }
        is_coherent = property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
}

void Buffer::Flush(u64 offset, u64 size) {
    if (mapped_data.empty() || is_coherent) {
        return;
    }
    vmaFlushAllocation(buffer.allocator, buffer.allocation, offset, size);
}

void Buffer::Invalidate(u64 offset, u64 size) {
    if (mapped_data.empty() || is_coherent) {
        return;
    }
    vmaInvalidateAllocation(buffer.allocator, buffer.allocation, offset, size);
}

constexpr u64 WATCHES_INITIAL_RESERVE = 0x100;
constexpr u64 WATCHES_RESERVE_CHUNK = 0x100;

StreamBuffer::StreamBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler_,
                           MemoryType mem_type, u64 size_bytes)
    : Buffer{instance, 0, size_bytes, mem_type}, scheduler{scheduler_},
      non_coherent_atom_size{instance.NonCoherentAtomSize()} {
    ReserveWatches(current_watches, WATCHES_INITIAL_RESERVE);
    ReserveWatches(previous_watches, WATCHES_INITIAL_RESERVE);
    Vulkan::SetObjectName(instance.GetDevice(), Handle(), "StreamBuffer({}):{:#x}",
                          BufferTypeName(mem_type), size_bytes);
}

bool StreamBuffer::PrepareMap(u64 size, u64 alignment, bool allow_wait) {
    if (!mapped_data.empty() && !is_coherent) {
        size = Common::AlignUp(size, non_coherent_atom_size);
        alignment =
            alignment > 0 ? std::lcm(alignment, non_coherent_atom_size) : non_coherent_atom_size;
    }

    if (size > this->size_bytes) {
        return false;
    }

    if (alignment > 0) {
        offset = Common::AlignUp(offset, alignment);
    }

    if (offset + size > this->size_bytes) {
        // The buffer would overflow, save the amount of used watches and reset the state.
        invalidation_mark = current_watch_cursor;
        current_watch_cursor = 0;
        offset = 0;

        // Swap watches and reset waiting cursors.
        std::swap(previous_watches, current_watches);
        wait_cursor = 0;
        wait_bound = 0;
    }

    if (!WaitPendingOperations(offset + size, allow_wait)) {
        return false;
    }

    mapped_size = size;
    return true;
}

std::pair<u8*, u64> StreamBuffer::Map(u64 size, u64 alignment, bool allow_wait) {
    if (!PrepareMap(size, alignment, allow_wait)) {
        return {nullptr, 0};
    }
    u8* const data = mapped_data.empty() ? nullptr : mapped_data.data() + offset;
    return {data, offset};
}

void StreamBuffer::Commit() {
    if (mem_type == MemoryType::HostCached) {
        Invalidate(offset, mapped_size);
    } else {
        Flush(offset, mapped_size);
    }
    AdvanceAndWatch();
}

std::optional<u64> StreamBuffer::Reserve(u64 size, u64 alignment, bool allow_wait) {
    if (!PrepareMap(size, alignment, allow_wait)) {
        return std::nullopt;
    }
    const u64 reserved_offset = offset;
    AdvanceAndWatch();
    return reserved_offset;
}

void StreamBuffer::AdvanceAndWatch() {
    offset += mapped_size;
    const u64 tick = scheduler.CurrentTick();
    last_tick = tick;

    // Extend the last watch if it belongs to the same tick.
    if (current_watch_cursor != 0 && current_watches[current_watch_cursor - 1].tick == tick) {
        current_watches[current_watch_cursor - 1].upper_bound = offset;
        return;
    }

    if (current_watch_cursor + 1 >= current_watches.size()) {
        // Ensure that there are enough watches.
        ReserveWatches(current_watches, WATCHES_RESERVE_CHUNK);
    }

    auto& watch = current_watches[current_watch_cursor++];
    watch.upper_bound = offset;
    watch.tick = tick;
}

void StreamBuffer::ReserveWatches(std::vector<Watch>& watches, std::size_t grow_size) {
    watches.resize(watches.size() + grow_size);
}

bool StreamBuffer::WaitPendingOperations(u64 requested_upper_bound, bool allow_wait) {
    if (!invalidation_mark) {
        return true;
    }
    while (requested_upper_bound > wait_bound && wait_cursor < *invalidation_mark) {
        auto& watch = previous_watches[wait_cursor];
        if (!scheduler.IsFree(watch.tick) && !allow_wait) {
            return false;
        }
        scheduler.Wait(watch.tick);
        wait_bound = watch.upper_bound;
        ++wait_cursor;
    }
    return true;
}

} // namespace VideoCore
