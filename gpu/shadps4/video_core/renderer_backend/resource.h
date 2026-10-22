// Project-owned resource descriptions shared by Vulkan and future backends.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "common/types.h"
#include "video_core/renderer_backend/state.h"

namespace VideoCore::Backend {

// These are identities, not native API handles. The backend will issue them as
// allocation and deferred destruction move behind this boundary; zero is never
// a live generation.
template <typename Tag>
struct Handle {
    u32 slot{};
    u32 generation{};

    explicit operator bool() const noexcept {
        return generation != 0;
    }
    friend bool operator==(Handle, Handle) = default;
};
struct BufferTag;
struct ImageTag;
struct ImageViewTag;
struct SamplerTag;
using BufferHandle = Handle<BufferTag>;
using ImageHandle = Handle<ImageTag>;
using ImageViewHandle = Handle<ImageViewTag>;
using SamplerHandle = Handle<SamplerTag>;

enum class MemoryClass : u8 {
    DeviceLocal,
    HostUncached, // Host-visible memory optimized for uploads.
    HostCached,   // Host-visible memory optimized for readback.
    Stream,       // Prefer device-local host-visible memory, with a host-memory fallback.
    Sparse,       // Unbacked resource; the backend binds its memory separately.
};

enum class BufferUse : u32 {
    None = 0,
    TransferSource = 1u << 0,
    TransferDestination = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,
    Vertex = 1u << 4,
    Index = 1u << 5,
    Indirect = 1u << 6,
    DeviceAddress = 1u << 7,
};

constexpr BufferUse operator|(BufferUse a, BufferUse b) noexcept {
    return static_cast<BufferUse>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr bool Has(BufferUse uses, BufferUse use) noexcept {
    return (static_cast<u32>(uses) & static_cast<u32>(use)) != 0;
}

struct BufferDesc {
    u64 size_bytes{};
    BufferUse uses{BufferUse::None};
    MemoryClass memory{MemoryClass::DeviceLocal};
    bool concurrent_readback{};
};

struct SamplerDesc {
    Filter mag_filter{Filter::Nearest};
    Filter min_filter{Filter::Nearest};
    MipmapMode mipmap_mode{MipmapMode::Nearest};
    AddressMode address_u{AddressMode::Repeat};
    AddressMode address_v{AddressMode::Repeat};
    AddressMode address_w{AddressMode::Repeat};
    float lod_bias{};
    float min_lod{};
    float max_lod{};
    float max_anisotropy{1.0f}; ///< 1: anisotropic filtering off
    bool compare_enable{};
    CompareOp compare_op{CompareOp::Never};
    BorderColor border_color{BorderColor::FloatTransparentBlack};
    std::array<float, 4> border_color_value{}; ///< for BorderColor::FloatCustom
};

struct Capabilities {
    bool sparse_buffers{};
    bool imported_host_memory{};
    bool exported_guest_memory{};
    bool buffer_device_address{};
    bool custom_border_color{};
    float max_sampler_anisotropy{1.0f}; ///< 1: anisotropic filtering unsupported
    float max_sampler_lod_bias{};
};

} // namespace VideoCore::Backend
