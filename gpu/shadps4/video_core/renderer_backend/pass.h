// The render adapter's draw state (docs/RENDER_ADAPTER.md, A4).
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_backend/state.h"

namespace VideoCore::Backend {

/// Laid out like VkViewport (checked in renderer_vulkan/vk_state.h).
struct Viewport {
    float x{}, y{}, width{}, height{}, min_depth{}, max_depth{};

    bool operator==(const Viewport&) const = default;
};

/// Laid out like VkRect2D.
struct Rect2D {
    s32 x{}, y{};
    u32 width{}, height{};

    bool operator==(const Rect2D&) const = default;
};

struct StencilOps {
    StencilOp fail_op{};
    StencilOp pass_op{};
    StencilOp depth_fail_op{};
    CompareOp compare_op{};

    bool operator==(const StencilOps&) const = default;
};

} // namespace VideoCore::Backend
