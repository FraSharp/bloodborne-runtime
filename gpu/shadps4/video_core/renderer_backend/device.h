// The render adapter's device: one per process, one graphics API (docs/RENDER_ADAPTER.md).
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "common/unique_function.h"
#include "video_core/renderer_backend/resource.h"

namespace VideoCore::Backend {

enum class Api : u8 {
    Vulkan,
    Metal,
};

/// The API the process renders with: BB_GFX_BACKEND=vulkan|metal, read once. Only Vulkan has a
/// backend so far (step A0); asking for Metal prints a note and returns Vulkan.
Api SelectedApi();

class Device {
public:
    virtual ~Device() = default;

    [[nodiscard]] virtual Api GetApi() const noexcept = 0;
    [[nodiscard]] virtual const Capabilities& GetCapabilities() const noexcept = 0;

    /// The tick of the submission now being recorded.
    [[nodiscard]] virtual u64 CurrentTick() const noexcept = 0;
    /// The GPU has completed every submission up to `tick`.
    [[nodiscard]] virtual bool IsComplete(u64 tick) noexcept = 0;
    virtual void Wait(u64 tick) = 0;

    /// Runs `release` once no submission that could use an object released now is pending,
    /// including work prepared before this call and recorded into a later submission. Callers
    /// never wait for the GPU to free something.
    virtual void DeferRelease(Common::UniqueFunction<void>&& release) = 0;
};

} // namespace VideoCore::Backend
