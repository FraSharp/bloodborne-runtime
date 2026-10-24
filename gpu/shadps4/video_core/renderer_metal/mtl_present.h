// SPDX-License-Identifier: GPL-2.0-or-later
// The render adapter's Metal swapchain (docs/RENDER_ADAPTER.md, M2 step 4): with the Metal frame
// (BB_METAL_FRAME=1) the window's CAMetalLayer is presented through Metal, not a MoltenVK
// swapchain. The presenter draws into offscreen images as before; each present copies one into
// the layer's next drawable on MoltenVK's queue (after the presenter's own submission, so in
// order) and presents it. Plain C++: Metal objects cross as void*.

#pragma once

#include <functional>
#include <string>
#include <utility>

#include "common/types.h"

namespace MetalPresent {

struct LayerConfig {
    bool hdr{};           ///< HDR10 (RGB10A2, BT.2100 PQ); else BGRA8 in sRGB
    bool display_sync{true};
    u32 max_drawables{3};
};

/// Sets up the window's CAMetalLayer (void* = CAMetalLayer*) for presents on MoltenVK's device.
void Configure(void* layer, const LayerConfig& config);
/// The layer's size in pixels (its bounds times its scale); sets it as the drawable size.
[[nodiscard]] std::pair<u32, u32> FitDrawableSize(void* layer);
/// Whether the display can show HDR (extended dynamic range) content.
[[nodiscard]] bool DisplaySupportsHdr(void* layer);

/// Copies `image` (a VkImage, width x height, the layer's pixel format) into the layer's next
/// drawable and presents it, on MoltenVK's queue; `done` runs once the GPU has read the image.
/// False when there was no drawable (the frame is not shown; `done` runs at once).
/// `dump_path` not empty: the drawable as presented is also written there (raw, 4 bytes a
/// pixel in the layer's order; tools/grab.sh --screen), the game's own picture.
bool Present(void* layer, void* image, u32 width, u32 height, std::function<void()> done,
             const std::string& dump_path = {});

} // namespace MetalPresent
