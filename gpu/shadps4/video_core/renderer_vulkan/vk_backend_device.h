// The render adapter's Vulkan backend: its device (docs/RENDER_ADAPTER.md, step A0).
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/renderer_backend/device.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Frontend {
class WindowSDL;
}

namespace Vulkan {

/// Owns the Vulkan instance (device, queues, VMA) and the draw scheduler, whose submissions
/// are the adapter's ticks. Code not yet moved onto the adapter reaches them through
/// GetInstance() and GetScheduler().
class BackendDevice final : public VideoCore::Backend::Device {
public:
    BackendDevice(Frontend::WindowSDL& window, s32 physical_device_index, bool enable_validation,
                  bool enable_crash_diagnostic);

    [[nodiscard]] VideoCore::Backend::Api GetApi() const noexcept override {
        return VideoCore::Backend::Api::Vulkan;
    }
    [[nodiscard]] const VideoCore::Backend::Capabilities& GetCapabilities() const noexcept override {
        return capabilities;
    }

    [[nodiscard]] u64 CurrentTick() const noexcept override {
        return scheduler.CurrentTick();
    }
    [[nodiscard]] bool IsComplete(u64 tick) noexcept override {
        return scheduler.IsFree(tick);
    }
    void Wait(u64 tick) override {
        scheduler.Wait(tick);
    }
    void DeferRelease(Common::UniqueFunction<void>&& release) override {
        // Past the next submission: the draw pipeline records draws prepared earlier.
        scheduler.DeferOperationPastNext(std::move(release));
    }

    [[nodiscard]] Instance& GetInstance() noexcept {
        return instance;
    }
    [[nodiscard]] Scheduler& GetScheduler() noexcept {
        return scheduler;
    }

private:
    Instance instance;
    Scheduler scheduler;
    VideoCore::Backend::Capabilities capabilities;
};

} // namespace Vulkan
