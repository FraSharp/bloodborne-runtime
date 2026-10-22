// Vulkan adapter for the N4 resource descriptions.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <utility>

#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_backend/resource.h"
#include "video_core/renderer_vulkan/vk_common.h"

VK_DEFINE_HANDLE(VmaAllocation)
VK_DEFINE_HANDLE(VmaAllocator)

struct VmaAllocationInfo;

namespace Vulkan {

/// A VkBuffer and its VMA allocation, owned by the render adapter's Vulkan backend.
struct UniqueBuffer {
    explicit UniqueBuffer(vk::Device device, VmaAllocator allocator);
    ~UniqueBuffer();

    UniqueBuffer(const UniqueBuffer&) = delete;
    UniqueBuffer& operator=(const UniqueBuffer&) = delete;

    UniqueBuffer(UniqueBuffer&& other) noexcept
        : device{other.device}, buffer{std::exchange(other.buffer, VK_NULL_HANDLE)},
          bda_addr{std::exchange(other.bda_addr, 0)},
          allocator{std::exchange(other.allocator, VK_NULL_HANDLE)},
          allocation{std::exchange(other.allocation, VK_NULL_HANDLE)} {}

    UniqueBuffer& operator=(UniqueBuffer&& other) noexcept {
        if (this != &other) {
            Destroy();
            device = other.device;
            buffer = std::exchange(other.buffer, VK_NULL_HANDLE);
            bda_addr = std::exchange(other.bda_addr, 0);
            allocator = std::exchange(other.allocator, VK_NULL_HANDLE);
            allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
        }
        return *this;
    }

    void Create(vk::BufferCreateInfo& buffer_ci, VideoCore::MemoryType mem_type,
                VmaAllocationInfo* out_alloc_info);

    void Destroy();

    vk::Device device;
    vk::Buffer buffer{};
    vk::DeviceAddress bda_addr{};
    VmaAllocator allocator;
    VmaAllocation allocation{};
};


class Instance;

VideoCore::Backend::Capabilities
QueryResourceCapabilities(const Instance &instance);
void CreateBufferFromDesc(const Instance &instance,
                          UniqueBuffer &buffer,
                          const VideoCore::Backend::BufferDesc &desc,
                          VmaAllocationInfo *out_alloc_info);

} // namespace Vulkan
