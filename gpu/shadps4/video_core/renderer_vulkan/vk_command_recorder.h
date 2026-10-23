// The render adapter's Vulkan backend: its draw and dispatch commands (docs/RENDER_ADAPTER.md, A4).
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/renderer_backend/commands.h"

namespace Vulkan {

class Scheduler;

/// Records into the scheduler's current submission; each draw or dispatch is one recorded
/// command with its pipeline bind, as before the adapter.
class CommandRecorder final : public VideoCore::Backend::CommandRecorder {
public:
    explicit CommandRecorder(Scheduler& scheduler) : scheduler{scheduler} {}

    void Draw(const VideoCore::Backend::DrawCommand& draw,
              const VideoCore::Backend::Crumb& crumb) override;
    void Dispatch(const VideoCore::Backend::DispatchCommand& dispatch,
                  const VideoCore::Backend::Crumb& crumb) override;
    void BindIndexBuffer(u64 buffer, u64 offset, VideoCore::Backend::IndexType type) override;
    void BindVertexBuffers(std::span<const u64> buffers, std::span<const u64> offsets,
                           std::span<const u64> sizes, std::span<const u64> strides) override;
    void SetVertexInput(std::span<const VideoCore::Backend::VertexBinding> bindings,
                        std::span<const VideoCore::Backend::VertexAttribute> attributes) override;
    void WriteLabel(u64 buffer, u64 offset, u64 value, u32 num_bytes) override;
    void BeginLabel(const char* name) override;
    void EndLabel() override;
    void InsertLabel(const char* name, u32 color) override;

    /// Labels as AMD buffer markers (end of pipe, inside render passes too) instead of
    /// portable transfer updates, which need no render pass open.
    void SetBufferMarkers(bool enabled) noexcept {
        buffer_markers = enabled;
    }

private:
    Scheduler& scheduler;
    bool buffer_markers{};
};

} // namespace Vulkan
