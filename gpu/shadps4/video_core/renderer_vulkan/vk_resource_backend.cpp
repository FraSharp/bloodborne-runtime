// Vulkan allocation behind the N4 resource-description boundary.
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_resource_backend.h"
#include "bbport_guest_memory.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"

#include <array>
#include <cstdio>
#include <vk_mem_alloc.h>

namespace VideoCore {

[[nodiscard]] static VkMemoryPropertyFlags
MemoryUsagePreferredVmaFlags(MemoryType type) {
  return type != MemoryType::DeviceLocal ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                         : VkMemoryPropertyFlagBits{};
}

[[nodiscard]] static VmaAllocationCreateFlags
MemoryUsageVmaFlags(MemoryType type) {
  switch (type) {
  case MemoryType::HostUncached:
  case MemoryType::Stream:
    return VMA_ALLOCATION_CREATE_MAPPED_BIT |
           VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
  case MemoryType::HostCached:
    return VMA_ALLOCATION_CREATE_MAPPED_BIT |
           VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
  case MemoryType::DeviceLocal:
  default:
    return {};
  }
}

[[nodiscard]] static VmaMemoryUsage MemoryUsageVma(MemoryType type) {
  switch (type) {
  case MemoryType::DeviceLocal:
  case MemoryType::Stream:
    return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  case MemoryType::HostUncached:
  case MemoryType::HostCached:
    return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
  default:
    return VMA_MEMORY_USAGE_UNKNOWN;
  }
}

UniqueBuffer::UniqueBuffer(vk::Device device_, VmaAllocator allocator_)
    : device{device_}, allocator{allocator_} {}

UniqueBuffer::~UniqueBuffer() { Destroy(); }

void UniqueBuffer::Destroy() {
  if (allocation) {
    vmaDestroyBuffer(allocator, buffer, allocation);
  } else if (buffer) {
    device.destroyBuffer(buffer);
  }
  buffer = VK_NULL_HANDLE;
  allocation = VK_NULL_HANDLE;
  bda_addr = 0;
}

void UniqueBuffer::Create(vk::BufferCreateInfo &buffer_ci, MemoryType mem_type,
                          VmaAllocationInfo *out_alloc_info) {
  const bool with_bda =
      bool(buffer_ci.usage & vk::BufferUsageFlagBits::eShaderDeviceAddress);
  if (mem_type != MemoryType::Sparse) {
    const VmaAllocationCreateFlags bda_flag =
        with_bda ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | bda_flag |
                 MemoryUsageVmaFlags(mem_type),
        .usage = MemoryUsageVma(mem_type),
        .requiredFlags = 0,
        .preferredFlags = MemoryUsagePreferredVmaFlags(mem_type),
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    const VkBufferCreateInfo buffer_ci_unsafe =
        static_cast<VkBufferCreateInfo>(buffer_ci);
    VkBuffer unsafe_buffer{};
    VkResult result =
        vmaCreateBuffer(allocator, &buffer_ci_unsafe, &alloc_ci, &unsafe_buffer,
                        &allocation, out_alloc_info);
    ASSERT_MSG(result == VK_SUCCESS, "Failed allocating buffer with error {}",
               vk::to_string(vk::Result{result}));
    buffer = vk::Buffer{unsafe_buffer};
  } else {
    buffer_ci.flags |= vk::BufferCreateFlagBits::eSparseBinding |
                       vk::BufferCreateFlagBits::eSparseResidency;
    // Bound to exported guest memory chunks (BB_GUEST_IN_PLACE) as well as VRAM
    // blocks.
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = BbGuestMemory::HandleType(),
    };
    if (GuestInPlace()) {
      buffer_ci.pNext = &external;
      if (const auto [result, created] = device.createBuffer(buffer_ci);
          result == vk::Result::eSuccess) {
        buffer = created;
        // bbport: VRAM blocks are bound into it too: the external memory type
        // must not keep device-local memory out (a driver may restrict it).
        const auto reqs = device.getBufferMemoryRequirements(created);
        const VkPhysicalDeviceMemoryProperties *props = nullptr;
        vmaGetMemoryProperties(allocator, &props);
        bool vram = false;
        for (u32 i = 0; i < props->memoryTypeCount; ++i) {
          vram |= (reqs.memoryTypeBits & (1u << i)) &&
                  (props->memoryTypes[i].propertyFlags &
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        if (!vram) {
          std::printf("Guest memory: the arena for guest memory cannot take "
                      "VRAM; the GPU "
                      "uses copies in VRAM as before\n");
          device.destroyBuffer(created);
          buffer = vk::Buffer{};
          DisableGuestInPlace();
        }
      } else {
        std::printf(
            "Guest memory: the arena cannot take exported memory (%s); the GPU "
            "uses copies in VRAM as before\n",
            vk::to_string(result).c_str());
        DisableGuestInPlace();
      }
      buffer_ci.pNext = nullptr;
    }
    if (!buffer) {
      buffer = Vulkan::Check(device.createBuffer(buffer_ci));
    }
  }

  if (with_bda) {
    const vk::BufferDeviceAddressInfo bda_info = {
        .buffer = buffer,
    };
    auto bda_result = device.getBufferAddress(bda_info);
    ASSERT_MSG(bda_result != 0, "Failed to get buffer device address");
    bda_addr = bda_result;
  }
}

} // namespace VideoCore

namespace Vulkan {
namespace Backend = VideoCore::Backend;

Backend::Capabilities QueryResourceCapabilities(const Instance &instance) {
  return {
      .sparse_buffers = instance.IsSparseBufferSupported(),
      .imported_host_memory = instance.IsHostMemoryImportSupported(),
      .exported_guest_memory = instance.IsGuestMemoryExportSupported(),
      .buffer_device_address = instance.IsBufferDeviceAddressSupported(),
      .custom_border_color = instance.IsCustomBorderColorSupported(),
      .max_sampler_anisotropy = instance.IsAnisotropicFilteringSupported()
                                    ? instance.MaxSamplerAnisotropy()
                                    : 1.0f,
      .max_sampler_lod_bias = instance.MaxSamplerLodBias(),
  };
}

static vk::BufferUsageFlags ToVulkanBufferUsage(Backend::BufferUse uses) {
  vk::BufferUsageFlags flags{};
  if (Backend::Has(uses, Backend::BufferUse::TransferSource))
    flags |= vk::BufferUsageFlagBits::eTransferSrc;
  if (Backend::Has(uses, Backend::BufferUse::TransferDestination))
    flags |= vk::BufferUsageFlagBits::eTransferDst;
  if (Backend::Has(uses, Backend::BufferUse::Uniform))
    flags |= vk::BufferUsageFlagBits::eUniformBuffer;
  if (Backend::Has(uses, Backend::BufferUse::Storage))
    flags |= vk::BufferUsageFlagBits::eStorageBuffer;
  if (Backend::Has(uses, Backend::BufferUse::Vertex))
    flags |= vk::BufferUsageFlagBits::eVertexBuffer;
  if (Backend::Has(uses, Backend::BufferUse::Index))
    flags |= vk::BufferUsageFlagBits::eIndexBuffer;
  if (Backend::Has(uses, Backend::BufferUse::Indirect))
    flags |= vk::BufferUsageFlagBits::eIndirectBuffer;
  if (Backend::Has(uses, Backend::BufferUse::DeviceAddress))
    flags |= vk::BufferUsageFlagBits::eShaderDeviceAddress;
  return flags;
}

void CreateBufferFromDesc(const Instance &instance,
                          VideoCore::UniqueBuffer &buffer,
                          const Backend::BufferDesc &desc,
                          VmaAllocationInfo *out_alloc_info) {
  const auto capabilities = QueryResourceCapabilities(instance);
  ASSERT_MSG(desc.memory != Backend::MemoryClass::Sparse ||
                 capabilities.sparse_buffers,
             "Sparse buffers are not supported by this Vulkan device");
  ASSERT_MSG(!Backend::Has(desc.uses, Backend::BufferUse::DeviceAddress) ||
                 capabilities.buffer_device_address,
             "Buffer device addresses are not supported by this Vulkan device");
  const std::array<u32, 2> families = {instance.GetGraphicsQueueFamilyIndex(),
                                       instance.GetReadbackQueueFamilyIndex()};
  vk::BufferCreateInfo create_info{
      .size = desc.size_bytes,
      .usage = ToVulkanBufferUsage(desc.uses),
      .sharingMode = vk::SharingMode::eExclusive,
  };
  if (desc.memory == Backend::MemoryClass::Sparse && desc.concurrent_readback &&
      instance.GetReadbackQueue()) {
    create_info.sharingMode = vk::SharingMode::eConcurrent;
    create_info.queueFamilyIndexCount = static_cast<u32>(families.size());
    create_info.pQueueFamilyIndices = families.data();
  }
  buffer.Create(create_info, desc.memory, out_alloc_info);
}

} // namespace Vulkan
