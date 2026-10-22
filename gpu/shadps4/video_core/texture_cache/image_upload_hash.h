// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace VideoCore {

// Adapted from Supermedo/bloodborne_pc 7e8bd6c1 (devilismyfriend) and
// 136a72d0 (Supermedo): record upload baselines without hashing large streamed textures.
constexpr bool NeedsImageUploadHash(std::size_t mip_size, bool gpu_modified, bool gpu_dirty,
                                    bool writable) noexcept {
    return (gpu_modified && !gpu_dirty) || mip_size <= 64 * 1024 || writable;
}

struct ImageUploadHash {
    std::uint64_t value{};
    bool valid{};

    // The guest-memory baseline is distinct from the image's GPU contents. A newer GPU
    // buffer source must still be uploaded, even when guest memory has not changed.
    bool ShouldUpload(std::uint64_t guest_hash, bool gpu_modified, bool gpu_dirty) noexcept {
        const bool unchanged = valid && value == guest_hash;
        value = guest_hash;
        valid = true;
        return !gpu_modified || gpu_dirty || !unchanged;
    }
};

} // namespace VideoCore
