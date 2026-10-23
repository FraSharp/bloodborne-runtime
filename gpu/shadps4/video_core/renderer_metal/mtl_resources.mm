// SPDX-License-Identifier: GPL-2.0-or-later
// The render adapter's Metal resources (M1); see mtl_resources.h.

#include "video_core/renderer_metal/mtl_resources.h"

#import <Metal/Metal.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/assert.h"
#include "video_core/renderer_backend/device.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include <vulkan/vulkan_metal.h>

namespace MetalResources {

namespace {
id<MTLDevice> mtl_device = nil;
} // namespace

bool Enabled() {
    static const bool enabled =
        VideoCore::Backend::SelectedApi() == VideoCore::Backend::Api::Metal;
    return enabled;
}

void Init(void* device) {
    if (!Enabled() || mtl_device) {
        return;
    }
    const VkDevice vk_device = static_cast<VkDevice>(device);
    const auto export_objects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(vk_device, "vkExportMetalObjectsEXT"));
    ASSERT_MSG(export_objects, "BB_GFX_BACKEND=metal needs VK_EXT_metal_objects");
    VkExportMetalDeviceInfoEXT device_info{VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
    VkExportMetalObjectsInfoEXT info{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &device_info};
    export_objects(vk_device, &info);
    mtl_device = device_info.mtlDevice;
    ASSERT_MSG(mtl_device, "MoltenVK gave no Metal device");
    std::printf("Render adapter: buffers made in Metal on %s (BB_GFX_BACKEND=metal)\n",
                mtl_device.name.UTF8String);
}

Buffer NewBuffer(u64 size, Storage storage) {
    // Metal's own hazard tracking, as MoltenVK's buffers have.
    MTLResourceOptions options = MTLResourceHazardTrackingModeDefault;
    switch (storage) {
    case Storage::Private:
        options |= MTLResourceStorageModePrivate;
        break;
    case Storage::SharedWriteCombined:
        options |= MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
        break;
    case Storage::Shared:
        options |= MTLResourceStorageModeShared;
        break;
    }
    id<MTLBuffer> buffer = [mtl_device newBufferWithLength:size options:options];
    ASSERT_MSG(buffer, "Metal could not make a buffer of {} bytes", size);
    return {(__bridge_retained void*)buffer,
            storage == Storage::Private ? nullptr : buffer.contents};
}

void Release(void* object) {
    if (object) {
        id<MTLBuffer> buffer = (__bridge_transfer id<MTLBuffer>)object;
        buffer = nil;
    }
}

} // namespace MetalResources
