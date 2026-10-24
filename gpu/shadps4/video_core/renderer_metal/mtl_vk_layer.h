// SPDX-License-Identifier: GPL-2.0-or-later
// From Wooloomooloo2/bloodborne_mac_port (78b59f5, GPL-2.0-or-later), adapted to bloodborne-runtime.
// bbport: the Metal back end's entry into the Vulkan renderer (docs/DESIGN_METAL_BACKEND.md, M2).
// The renderer records its GPU work as vk::CommandBuffer calls, which go through vulkan.hpp's
// dynamic dispatcher. Install() replaces the command-buffer functions in that dispatcher with ours;
// each forwards to MoltenVK unless the Metal path owns the command buffer (later steps).
// Step A: forwarding plus counts (BB_METAL_TRACE=1 prints the command stream every 5 s).

#pragma once

namespace MetalFrame {
struct Owned;
}

namespace MetalLayer {

/// `dispatcher`: VULKAN_HPP_DEFAULT_DISPATCHER, right after its init(device). Installs only when
/// BB_METAL_TRACE=1 or a Metal path is on (BB_METAL_PRESENT=1); otherwise it stays untouched.
void Install(void* dispatcher);

/// The device and queue (handles as void*: VkDevice, VkQueue) whose Metal objects the layer uses.
void SetDevice(void* device, void* queue);

/// id<MTLDevice> / id<MTLCommandQueue> of MoltenVK's queue (null while the Metal path is off).
void* MtlDevice();
void* MtlQueue();

/// Step C (BB_METAL_FRAME=1): every command buffer is encoded in Metal.
bool FrameEnabled();

/// The Metal encoding state of an owned command buffer (a VkCommandBuffer), or null: for the
/// render adapter's Metal recorder (mtl_recorder.mm), which encodes without Vulkan calls.
MetalFrame::Owned* OwnedFor(void* command_buffer);

/// With the Metal frame: a pipeline only Metal builds (MoltenVK never sees it). Returns a
/// placeholder VkPipeline the Metal frame finds the Metal pipeline by: built here from the
/// create info (a VkGraphicsPipelineCreateInfo or VkComputePipelineCreateInfo; one is null),
/// except for game pipelines (built by the pipeline cache with their keys, GameBuildScope) and
/// geometry-shader ones (likewise).
void* MetalOnlyPipeline(const void* graphics_info, const void* compute_info);

/// The id<MTLCommandBuffer> an owned command buffer is encoded into, with every encoder ended,
/// for work encoded outside the layer in order (MetalFX); null when it is not owned.
void* MetalCommandBuffer(void* command_buffer);

/// Step B (BB_METAL_PRESENT=1): the presenter's command buffer is encoded in Metal.
bool PresentEnabled();

/// From now until it is submitted, `command_buffer` (a VkCommandBuffer, already begun) is encoded
/// by the Metal path; its commands do not reach MoltenVK.
void Own(void* command_buffer);

} // namespace MetalLayer
