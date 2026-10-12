// Project-owned resource descriptions shared by Vulkan and future backends.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace VideoCore::Backend {

// These are identities, not native API handles. The backend will issue them as
// allocation and deferred destruction move behind this boundary; zero is never
// a live generation.
template <typename Tag> struct Handle {
  u32 slot{};
  u32 generation{};

  explicit operator bool() const noexcept { return generation != 0; }
  friend bool operator==(Handle, Handle) = default;
};
struct BufferTag;
struct ImageTag;
struct ImageViewTag;
using BufferHandle = Handle<BufferTag>;
using ImageHandle = Handle<ImageTag>;
using ImageViewHandle = Handle<ImageViewTag>;

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

struct Capabilities {
  bool sparse_buffers{};
  bool imported_host_memory{};
  bool exported_guest_memory{};
  bool buffer_device_address{};
};

} // namespace VideoCore::Backend
