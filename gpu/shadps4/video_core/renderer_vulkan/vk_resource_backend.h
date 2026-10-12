// Vulkan adapter for the N4 resource descriptions.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/renderer_backend/resource.h"

struct VmaAllocationInfo;

namespace VideoCore {
struct UniqueBuffer;
}

namespace Vulkan {

class Instance;

VideoCore::Backend::Capabilities
QueryResourceCapabilities(const Instance &instance);
void CreateBufferFromDesc(const Instance &instance,
                          VideoCore::UniqueBuffer &buffer,
                          const VideoCore::Backend::BufferDesc &desc,
                          VmaAllocationInfo *out_alloc_info);

} // namespace Vulkan
