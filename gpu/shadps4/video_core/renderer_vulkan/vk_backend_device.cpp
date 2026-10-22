// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_backend_device.h"
#include "video_core/renderer_vulkan/vk_resource_backend.h"

namespace Vulkan {

BackendDevice::BackendDevice(Frontend::WindowSDL& window, s32 physical_device_index,
                             bool enable_validation, bool enable_crash_diagnostic)
    : instance{window, physical_device_index, enable_validation, enable_crash_diagnostic},
      scheduler{instance, true}, capabilities{QueryResourceCapabilities(instance)} {
    VideoCore::Backend::SelectedApi();
}

} // namespace Vulkan
