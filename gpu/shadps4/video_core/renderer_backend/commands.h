// The render adapter's draw and dispatch commands (docs/RENDER_ADAPTER.md, A4).
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string>

#include "common/types.h"
#include "video_core/renderer_backend/binding.h"
#include "video_core/renderer_backend/format.h"
#include "video_core/renderer_backend/pass.h"

namespace VideoCore::Backend {

struct PipelineAllocation;

/// What a command was, for naming the command a GPU hang is stuck in (breadcrumbs).
enum class CrumbKind : u8 { Pass, Draw, DrawIndexed, DrawIndirect, Dispatch, DispatchIndirect };

struct Crumb {
    CrumbKind kind = CrumbKind::Pass;
    const char* name = nullptr; ///< our passes: a string literal
    u64 hash[2]{};              ///< guest shaders: vertex and pixel, or compute
    u64 program[2]{};           ///< their code in guest memory
    u64 address = 0;            ///< indirect arguments
    u32 count[3]{};             ///< indices or vertices and instances, or groups
    u32 args_slot = ~0u;        ///< indirect dispatches: where the GPU copies its arguments
};

/// Values as Vulkan's (checked in renderer_vulkan/vk_state.h).
enum class IndexType : s32 { UInt16 = 0, UInt32 = 1 };
enum class VertexInputRate : s32 { Vertex = 0, Instance = 1 };

/// A vertex buffer slot of the vertex input state: its stride, and whether it advances per vertex
/// or per instance (every `divisor` instances).
struct VertexBinding {
    u32 binding{};
    u32 stride{};
    VertexInputRate input_rate{VertexInputRate::Vertex};
    u32 divisor{1};
};

/// A vertex attribute: its shader location, the slot it reads and its format.
struct VertexAttribute {
    u32 location{};
    u32 binding{};
    Format format{Format::Undefined};
    u32 offset{};
};

/// Uses of memory a barrier separates, as bits: what came before and what comes after.
enum class MemoryUse : u32 {
    None = 0,
    ComputeRead = 1u << 0,
    ComputeWrite = 1u << 1,
    VertexRead = 1u << 2, ///< vertex-stage shaders (and vertex and index fetch)
    VertexWrite = 1u << 3,
    FragmentRead = 1u << 4,
    FragmentWrite = 1u << 5,
    IndirectRead = 1u << 6, ///< indirect draw and dispatch arguments
    TransferRead = 1u << 7,
    TransferWrite = 1u << 8,
    HostRead = 1u << 9,
    HostWrite = 1u << 10,
    Any = 1u << 11, ///< every stage, every access
};
constexpr MemoryUse operator|(MemoryUse a, MemoryUse b) noexcept {
    return static_cast<MemoryUse>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr bool Has(MemoryUse uses, MemoryUse use) noexcept {
    return (static_cast<u32>(uses) & static_cast<u32>(use)) != 0;
}

/// A draw with its pipeline (the backend's object, NativeHandle). Direct when `args_buffer` is
/// zero; otherwise its arguments are read from `args_buffer`, and their number from
/// `count_buffer` (at most `max_draws`) when that is not zero.
struct DrawCommand {
    u64 pipeline{};
    bool indexed{};
    u32 count{};     ///< indices or vertices
    u32 instances{};
    u32 first{};     ///< first vertex (vertex offset when indexed)
    u32 first_instance{};
    u64 args_buffer{}, args_offset{};
    u64 count_buffer{}, count_offset{};
    u32 max_draws{}, stride{};
};

/// A dispatch with its pipeline. Indirect when `args_buffer` is not zero; `crumb_args_buffer`
/// and `crumb_args_offset` are the arguments as the game wrote them, copied for breadcrumbs.
struct DispatchCommand {
    u64 pipeline{};
    u32 groups[3]{};
    u64 args_buffer{}, args_offset{};
    u64 crumb_args_buffer{}, crumb_args_offset{};
};

/// Records commands into the backend's current submission, in order.
class CommandRecorder {
public:
    virtual ~CommandRecorder() = default;

    /// Draws go into a pass with `state`'s targets; a pass with the same targets stays open (and
    /// one whose targets were cleared on load continues for a draw that loads them).
    virtual void BeginPass(const RenderState& state) = 0;
    /// Ends the open pass, if any.
    virtual void EndPass() = 0;
    [[nodiscard]] virtual bool InPass() const = 0;
    /// A pass with `state`'s targets is open (BeginPass would continue it).
    [[nodiscard]] virtual bool InPassWith(const RenderState& state) const = 0;
    /// BeginPass(state) would start a new pass.
    [[nodiscard]] virtual bool WillBeginPass(const RenderState& state) const = 0;
    /// The next BeginPass starts a new pass (a deferred clear taken as its load action).
    virtual void ForceNewPass() = 0;
    /// BB_PASS_LABELS=1: the next pass is recorded inside a debug label with this name.
    virtual void SetPassLabel(std::string label) = 0;

    virtual void Draw(const DrawCommand& draw, const Crumb& crumb) = 0;
    virtual void Dispatch(const DispatchCommand& dispatch, const Crumb& crumb) = 0;
    virtual void BindIndexBuffer(u64 buffer, u64 offset, IndexType type) = 0;
    /// Binds `buffers` from slot 0. `sizes` and `strides` empty: the pipeline's strides.
    virtual void BindVertexBuffers(std::span<const u64> buffers, std::span<const u64> offsets,
                                   std::span<const u64> sizes, std::span<const u64> strides) = 0;

    /// Later `after` uses of all memory wait for earlier `before` uses.
    virtual void Barrier(MemoryUse before, MemoryUse after) = 0;
    /// The same for `size` bytes of `buffer` (NativeHandle) from `offset`.
    virtual void BufferBarrier(u64 buffer, u64 offset, u64 size, MemoryUse before,
                               MemoryUse after) = 0;

    /// The resources of the next draws or dispatches with `pipeline`: `writes` into its slots
    /// and `push_constants`. Writes may point into `images` and `buffers`, which are then copied
    /// whole instead of element by element.
    virtual void BindResources(const PipelineAllocation& pipeline, std::span<const SlotWrite> writes,
                               std::span<const u8> push_constants,
                               std::span<const ImageBinding> images = {},
                               std::span<const BufferBinding> buffers = {}) = 0;

    /// The vertex input state of the next draws (pipelines built with dynamic vertex input).
    virtual void SetVertexInput(std::span<const VertexBinding> bindings,
                                std::span<const VertexAttribute> attributes) = 0;

    /// Writes `value` (`num_bytes`: 4 or 8) to the buffer once every earlier command has
    /// finished; a 64-bit value as two 32-bit writes, the high half first.
    virtual void WriteLabel(u64 buffer, u64 offset, u64 value, u32 num_bytes) = 0;

    /// Debug labels for capture tools (`color`: 0xAARRGGBB, 0 for none).
    virtual void BeginLabel(const char* name) = 0;
    virtual void EndLabel() = 0;
    virtual void InsertLabel(const char* name, u32 color) = 0;
};

} // namespace VideoCore::Backend
