// The render adapter's Vulkan backend: objects of the renderer's own passes (A6).
// SPDX-License-Identifier: GPL-2.0-or-later

#include <utility>

#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_backend_device.h"
#include "video_core/renderer_vulkan/vk_host_objects.h"

namespace Vulkan {

namespace Backend = VideoCore::Backend;

HostPipeline::~HostPipeline() {
    if (device) {
        device->DestroyPipeline(allocation.handle);
    }
}

HostPipeline::HostPipeline(HostPipeline&& other) noexcept
    : device{std::exchange(other.device, nullptr)}, allocation{std::exchange(other.allocation, {})} {}

HostPipeline& HostPipeline::operator=(HostPipeline&& other) noexcept {
    if (this != &other) {
        if (device) {
            device->DestroyPipeline(allocation.handle);
        }
        device = std::exchange(other.device, nullptr);
        allocation = std::exchange(other.allocation, {});
    }
    return *this;
}

vk::PipelineLayout HostPipeline::Layout() const noexcept {
    const auto* record = static_cast<const BackendDevice::PipelineRecord*>(allocation.binding);
    return record ? *record->layout : vk::PipelineLayout{};
}

HostPipeline MakeHostCompute(BackendDevice& device, std::span<const u32> spirv,
                             std::span<const Backend::BindingSlot> slots, u32 push_constant_size,
                             std::string_view name) {
    const auto shader = device.CreateShader(spirv, name);
    ASSERT_MSG(shader.handle, "Failed to compile the {} shader", name);
    const auto allocation = device.CreateComputePipeline(
        {.shader = shader.handle,
         .layout = {.slots = slots, .push_constant_size = push_constant_size},
         .host_pass = true},
        name);
    device.DestroyShader(shader.handle); // the pipeline holds it
    ASSERT_MSG(allocation.handle, "Failed to create the {} pipeline", name);
    return {device, allocation};
}

HostSampler::HostSampler(BackendDevice& device_, const Backend::SamplerDesc& desc)
    : device{&device_}, sampler{device_.CreateSampler(desc)},
      native{NativeSampler(device_.NativeHandle(sampler))} {}

HostSampler::~HostSampler() {
    if (device) {
        device->DestroySampler(sampler);
    }
}

HostSampler::HostSampler(HostSampler&& other) noexcept
    : device{std::exchange(other.device, nullptr)}, sampler{std::exchange(other.sampler, {})},
      native{std::exchange(other.native, {})} {}

HostSampler& HostSampler::operator=(HostSampler&& other) noexcept {
    if (this != &other) {
        if (device) {
            device->DestroySampler(sampler);
        }
        device = std::exchange(other.device, nullptr);
        sampler = std::exchange(other.sampler, {});
        native = std::exchange(other.native, {});
    }
    return *this;
}

} // namespace Vulkan
