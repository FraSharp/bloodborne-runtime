// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <xxhash.h>

#include "video_core/texture_cache/image_upload_hash.h"

int main() {
    using VideoCore::ImageUploadHash;
    using VideoCore::NeedsImageUploadHash;
    std::array<unsigned char, 128> guest{}, gpu{};
    ImageUploadHash baseline;
    auto hash = [&] { return XXH3_64bits(guest.data(), guest.size()); };
    auto upload = [&](bool modified, bool dirty) {
        if (baseline.ShouldUpload(hash(), modified, dirty)) {
            gpu = guest;
            return true;
        }
        return false;
    };

    guest.fill(3);
    assert(upload(false, false)); // Initial CPU upload establishes the reference.
    gpu.fill(7);                  // The GPU subsequently changes the image.
    assert(!upload(true, false)); // A write elsewhere in the shared page leaves it intact.
    for (auto value : gpu) {
        assert(value == 7);
    }

    guest.back() = 9; // A real CPU write past the first pixels must refresh the image.
    assert(upload(true, false));
    assert(gpu == guest);
    assert(!upload(true, false));

    // A newer GPU buffer source must be imported even when the CPU baseline is unchanged.
    assert(baseline.ShouldUpload(hash(), true, true));
    gpu.fill(11); // Simulated upload from the GPU buffer, rather than stale guest memory.
    assert(!upload(true, false));
    for (auto value : gpu) {
        assert(value == 11);
    }

    ImageUploadHash first_storage_binding;
    assert(first_storage_binding.ShouldUpload(hash(), false, true));
    assert(!first_storage_binding.ShouldUpload(hash(), true, false));

    // Hash value zero is a valid digest, distinct from an absent baseline.
    ImageUploadHash zero;
    assert(zero.ShouldUpload(0, true, false));
    assert(!zero.ShouldUpload(0, true, false));

    // Mips have independent baselines: changing one does not invalidate another.
    std::array<ImageUploadHash, 2> mips;
    assert(mips[0].ShouldUpload(1, false, false));
    assert(mips[1].ShouldUpload(2, false, false));
    assert(mips[0].ShouldUpload(3, true, false));
    assert(!mips[1].ShouldUpload(2, true, false));

    constexpr std::size_t large = 1024 * 1024;
    assert(NeedsImageUploadHash(64 * 1024, false, false, false));
    assert(!NeedsImageUploadHash(64 * 1024 + 1, false, false, false));
    assert(!NeedsImageUploadHash(large, false, false, false));
    assert(NeedsImageUploadHash(large, false, false, true));
    assert(NeedsImageUploadHash(large, true, false, false));
    assert(!NeedsImageUploadHash(large, true, true, false));
    assert(NeedsImageUploadHash(large, true, true, true));
    std::puts("Image upload hashes: GPU preservation, CPU writes, GPU buffer updates, mips and cost limit passed");
}
