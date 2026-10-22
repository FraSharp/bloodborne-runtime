// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_backend_device.h"
#include "video_core/renderer_vulkan/vk_resource_backend.h"
#include "video_core/renderer_vulkan/vk_state.h"

namespace Vulkan {

BackendDevice::BackendDevice(Frontend::WindowSDL& window, s32 physical_device_index,
                             bool enable_validation, bool enable_crash_diagnostic)
    : instance{window, physical_device_index, enable_validation, enable_crash_diagnostic},
      scheduler{instance, true}, capabilities{QueryResourceCapabilities(instance)} {
    VideoCore::Backend::SelectedApi();
}

namespace Backend = VideoCore::Backend;

Backend::SamplerHandle BackendDevice::CreateSampler(const Backend::SamplerDesc& desc) {
    const bool custom = desc.border_color == Backend::BorderColor::FloatCustom;
    const vk::SamplerCustomBorderColorCreateInfoEXT custom_color{
        .customBorderColor = vk::ClearColorValue{.float32 = desc.border_color_value},
        .format = vk::Format::eR32G32B32A32Sfloat,
    };
    const vk::SamplerCreateInfo sampler_ci = {
        .pNext = custom ? &custom_color : nullptr,
        .magFilter = ToVk(desc.mag_filter),
        .minFilter = ToVk(desc.min_filter),
        .mipmapMode = ToVk(desc.mipmap_mode),
        .addressModeU = ToVk(desc.address_u),
        .addressModeV = ToVk(desc.address_v),
        .addressModeW = ToVk(desc.address_w),
        .mipLodBias = desc.lod_bias,
        .anisotropyEnable = desc.max_anisotropy > 1.0f,
        .maxAnisotropy = desc.max_anisotropy,
        .compareEnable = desc.compare_enable,
        .compareOp = ToVk(desc.compare_op),
        .minLod = desc.min_lod,
        .maxLod = desc.max_lod,
        .borderColor = ToVk(desc.border_color),
        .unnormalizedCoordinates = false,
    };
    auto [result, sampler] = instance.GetDevice().createSamplerUnique(sampler_ci);
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create sampler: {}",
               vk::to_string(result));
    std::scoped_lock lock{objects_mutex};
    return samplers.Insert(std::move(sampler));
}

void BackendDevice::DestroySampler(Backend::SamplerHandle sampler) {
    if (!sampler) {
        return;
    }
    vk::UniqueSampler object;
    {
        std::scoped_lock lock{objects_mutex};
        vk::UniqueSampler* entry = samplers.Get(sampler);
        if (!entry) {
            return;
        }
        object = std::move(*entry);
        samplers.Erase(sampler);
    }
    DeferRelease([object = std::move(object)] {});
}

u64 BackendDevice::NativeHandle(Backend::SamplerHandle sampler) {
    std::scoped_lock lock{objects_mutex};
    const vk::UniqueSampler* entry = samplers.Get(sampler);
    return entry ? reinterpret_cast<u64>(static_cast<VkSampler>(**entry)) : 0;
}

} // namespace Vulkan
