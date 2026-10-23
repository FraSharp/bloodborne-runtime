// SPDX-License-Identifier: GPL-2.0-or-later
// The render adapter's Metal resources (docs/RENDER_ADAPTER.md, M1): with BB_GFX_BACKEND=metal,
// the device makes buffers in Metal and hands them to MoltenVK (VK_EXT_metal_objects imports).
// Elsewhere these are no-ops.

#pragma once

#include "common/types.h"

namespace MetalResources {

enum class Storage : u8 {
    Private,           ///< GPU only
    SharedWriteCombined, ///< CPU writes, GPU reads
    Shared,            ///< CPU reads and writes
};

/// A Metal buffer (id<MTLBuffer>, retained); `contents` is null for private storage.
struct Buffer {
    void* object{};
    void* contents{};
};

#ifdef BB_METAL_BACKEND
/// BB_GFX_BACKEND=metal on the Metal build: buffers are made in Metal.
bool Enabled();
/// MoltenVK's Metal device, from `device` (a VkDevice); before the first NewBuffer.
void Init(void* device);
/// A buffer of `size` bytes; fatal when Metal cannot make it.
Buffer NewBuffer(u64 size, Storage storage);
/// Gives up a reference taken by NewBuffer.
void Release(void* object);
#else
inline bool Enabled() {
    return false;
}
inline void Init(void*) {}
inline Buffer NewBuffer(u64, Storage) {
    return {};
}
inline void Release(void*) {}
#endif

} // namespace MetalResources
