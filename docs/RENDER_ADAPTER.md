# Render adapter (N4, restarted)

[← Documentation](README.md) · [Native renderer](NATIVE_RENDERER.md)

**State:** approved on 2026-10-22. A0, A1 and A2 are done; A3 is in progress
(the caches follow the GPU through the device; commands next). The decisions are at the end.

The render adapter is the project's own GPU interface. The renderer asks it for resources,
passes, pipelines and commands; a backend per graphics API carries them out. Vulkan (Linux,
Windows) and Metal (macOS) are the two backends. MoltenVK and the Metal interception layer
(`renderer_metal/mtl_vk_layer`) go away on macOS.

## Why, and where it leads

The goal is a native port: the game's code recompiled ahead of time, its shaders converted
ahead of time, and its graphics drawn by our own renderer, with nothing emulated while it runs.
Today the renderer is shadPS4's, and it names Vulkan types and calls ~4,700 times (`vk::` uses across `video_core/` and `shim/`).

| Layer | Today | End state | Step |
|---|---|---|---|
| GPU API | Vulkan everywhere; MoltenVK, or interception, on the Mac | **This adapter**, with native Vulkan and Metal backends | N4, N5 |
| Shaders | GCN recompiled while the game runs (shadPS4's recompiler) | Converted once on the player's machine, loaded as Metal libraries or SPIR-V | N2 |
| Textures | shadPS4's texture cache infers images from GPU memory | The engine's own texture creation and release, hooked | N3 |
| Graphics commands | The game writes PS4 packets (PM4); shadPS4's reader turns them into draws | The engine's render calls hooked at their source | N6 (needs R4) |
| Game code | bbcpu translator, or the recompiled library | The recompiled library only | R3 |

The adapter comes first because every later step needs a renderer API that is not shadPS4's
Vulkan code. N6's hooked engine calls will talk to the adapter directly. So the adapter is
designed around what a renderer needs, and shadPS4's code is only its first client, retired
piece by piece as N2, N3 and N6 land.

One limit stays: no game code, shaders or assets in any repository. Everything native is
generated on the player's machine from their own copy of the game.

## What stays, what moves

| Part | With the adapter |
|---|---|
| Packet reader (`amdgpu/`), bbport's parallel draw preparation | Stays as is |
| Shader recompiler (GCN → IR → SPIR-V) | Stays; its output becomes an adapter shader source |
| Texture cache logic (detiling, overlaps, invalidation, collection) | Stays. Its ~490 Vulkan uses go through the adapter (180 are `vk::Format`) |
| Buffer cache, page tracking, the guest pool | Stays. Its ~220 Vulkan uses go through the adapter |
| Rasterizer (draw state, pipeline keys, two-stage draw pipeline) | Logic stays; it records adapter commands instead of `vk::CommandBuffer` calls |
| `vk_runtime` (copies, clears, uploads, `Transit`, pending clears) | Becomes the core's command helper; barriers and layouts move into the Vulkan backend |
| Instance, device, queues, swapchain, VMA, descriptors, barriers | Move into the Vulkan backend |
| `mtl_frame`, `mtl_pso_cache`, `mtl_shader`, the SPIR-V rewrites | Become the Metal backend |
| `mtl_vk_layer`, `VK_EXT_metal_objects` lookups, MoltenVK on macOS | Removed |

The cost: shadPS4 upstream fixes no longer merge cleanly, because these files will no longer
look like theirs. Fixes are ported by hand, as bbport's already are.

## Contract

This keeps N4's approved resource contract and adds commands and synchronization.

### Objects

- One `Device` per process, with one backend chosen at startup (`BB_GFX_BACKEND=vulkan|metal`).
  No frame mixes backends.
- Opaque handles with a slot and a generation (today's `renderer_backend/resource.h`):
  `Buffer`, `Image`, `ImageView`, `Sampler`, `Shader`, `GraphicsPipeline`, `ComputePipeline`,
  `QueryPool`. A stale handle is caught, not dereferenced.
- `Destroy` is deferred: the backend releases the object after the last submission that could
  use it has completed. Callers never wait for the GPU to free something.
- The guest pool is imported from host memory (`ImportHostMemory`: Vulkan's external host
  memory, Metal's `newBufferWithBytesNoCopy`). The backend splits it where its API needs to
  (MoltenVK's 4 GiB offset limit stays the Vulkan backend's concern).

### Vocabulary

The adapter has its own enums: formats, blend factors and operations, compare and stencil
operations, topology, cull and winding, filters and address modes, sample counts, usages. Each
backend translates them with a table. To keep the move mechanical, the first version gives
`Format` and the state enums the same numeric values as Vulkan's, so pipeline keys and the
shader cache stay valid across the change. No Vulkan header is included above the backends.

### Commands

A `CommandList` records, then is submitted as a whole:

- **Passes:** `BeginRenderPass` with color, depth and stencil attachments, each with a load
  action (load, clear with a value, don't care) and a store action; `EndRenderPass`. Deferred
  clears become this load action in the core, as `BB_DEFER_CLEARS` does today.
- **Draws:** direct, indexed, indirect and indirect-count; dispatch and indirect dispatch.
- **Transfers:** buffer copy, fill and inline update; buffer↔image copies; image copy, blit
  and resolve; clears outside a pass.
- **State:** the pipeline; a dynamic-state block (viewports, scissors, depth, stencil, bias,
  blend constants, cull, winding, color write masks), set as a whole and compared against the
  last one; vertex and index buffers; push constants.
- **Bindings:** flat, typed slots per stage (buffers with offset and size, textures, samplers,
  storage images), as Vulkan's push descriptors and Metal's direct slots. No descriptor sets,
  tables or argument buffers.
- **Diagnostics:** labels, timestamps, occlusion queries.

### Synchronization

The core declares what each pass and command reads and writes (resource, range, and how:
sampled, storage, attachment, vertex, index, indirect, transfer). It never names a layout,
stage mask or barrier. The Vulkan backend turns declarations into barriers and layouts; today's
`vk_barrier_tracker` and `Runtime::Transit` move there. The Metal backend uses them to place
encoder boundaries and fences. Writes from shaders into the guest pool through the page table
(`BB_LAYER_MEMORY`) are declared as a global write.

Submission returns a tick; `Wait(tick)` and `IsComplete(tick)` replace the scheduler's Vulkan
semaphores above the backend.

### Shaders and pipelines

A shader is created from SPIR-V plus its binding layout (the recompiler's `Info`). The Metal
backend converts it with SPIRV-Cross and applies the rect-list, tessellation and geometry
rewrites, as today. Pipelines are created from backend-neutral descriptions (today's
`vk_pipeline_key.h` holds no Vulkan handles already). Each backend keeps its own on-disk cache:
Vulkan's pipeline cache, Metal's binary archive. N2 later replaces the SPIR-V input with
shaders converted ahead of time.

Pipelines own their shaders, so a shader is freed when its last pipeline is freed. That closes
the roadmap item about the shadow layer keeping every shader for the session.

### Presentation and native interop

- `Swapchain`: acquire, present, resize, HDR and vsync choices; the backend owns the platform
  surface (CAMetalLayer on macOS).
- `NativeHandle(object)` returns the backend's object (`VkImage`, `id<MTLTexture>`, a command
  buffer) for code that must call a vendor library directly: FSR 3.1 (portable Vulkan
  FidelityFX), FSR 4 and DLSS on the Vulkan backend, MetalFX on the Metal backend, and ImGui's
  renderer. These are the only places outside a backend that see a native API, each in its own
  file.

## Order of work

Each step is behind a switch or keeps behavior identical, keeps Linux building in CI, and is
measured against the step before it. The current renderer stays playable throughout. The
existing Metal path (`BB_METAL_FRAME=1`) stays the launcher's Metal option until the adapter's
Metal backend draws everything; then the interception layer is removed.

| Step | Work | Accepted when |
|---|---|---|
| A0 ✓ | Adapter headers; a Vulkan backend that wraps today's objects; `Device` owns instance, queue and VMA | No behavior change; FPS and p90 unchanged in alternating runs |
| A1 ✓ | Own vocabulary in the caches and pipeline keys | Shader and pipeline caches still load warm |
| A2 ✓ | Buffers, images, views and samplers created and destroyed through the adapter, with deferred release | Metal API validation clean; texture memory unchanged |
| A3 | `vk_runtime`'s copies, clears and uploads as commands with access declarations; barriers inside the backend | Same images and frame time; no validation errors |
| A4 | Rasterizer: passes, draws, bindings, dynamic-state block | GPU-thread recording ≤ 5.6 µs per draw (today 5.1–5.6) |
| A5 | Pipelines and shaders through the adapter; pipelines own shaders | 550 pipelines build; unused shaders freed |
| A6 | Presenter, overlay, upscalers through `Swapchain` and `NativeHandle` | Every upscaler choice renders on Vulkan |
| M1 | Metal backend: device, queue, resources (heaps; the pool without copies), transfers | Loading reaches the Hunter's Dream with `BB_GFX_BACKEND=metal` |
| M2 | Metal backend: passes, draws, tessellation, pipelines (from `mtl_frame` and `mtl_pso_cache`) | A copied-save play session with no faults; FPS ≥ today's Metal path (65.5/61.7) |
| M3 | MetalFX without input and output copies; FSR 3.1 on Metal or left out on that backend (decision) | Upscalers measured against today |
| M4 | Metal by default on macOS; MoltenVK and `mtl_vk_layer` removed from the macOS build | Release-candidate play tests pass |

Wooloomooloo2's fork reached its Metal-only renderer in a similar order (device and queue,
command recording and caches, pipelines and rasterizer, presenter, upscalers and overlay, then
FSR 3.1). Their code is taken where it fits and credited in the commit that brings it.

### Done

- **A0:** `renderer_backend/device.h` (the `Device` interface: API, capabilities, ticks,
  deferred release), `renderer_backend/handle_table.h` (generation-checked handles, tested by
  `handle-table-test`) and the Vulkan backend's `BackendDevice` (`vk_backend_device.h`), which
  owns the Vulkan instance and the draw scheduler; the presenter holds it. Deferred release
  waits past the next submission, as the draw pipeline needs. `BB_GFX_BACKEND` is read; only
  `vulkan` exists. Same frame time as main in a copied-save run on MoltenVK (29.6 against 29.5
  FPS, p90 35.0 against 35.1 ms, with other GPU work running on the Mac).
- **A1, formats:** `renderer_backend/format.h` lists every format the renderer names (153),
  with Vulkan's values; `renderer_vulkan/vk_format.h` checks each value and the size at compile
  time and converts with `ToVk`/`FromVk`. The texture cache (`ImageInfo::pixel_format`,
  `ImageViewInfo::format`), the PS4 format tables (`LiverpoolToVK::SurfaceFormat`,
  `DepthFormat`) and the pipeline key's vertex formats use it; Vulkan-side code converts where
  it calls Vulkan. Both caches stayed warm: the Vulkan pipeline cache loaded with no compiles,
  and Metal's binary archive served every pipeline in a second run (35.5 FPS, p90 30.3 ms,
  against 36.1 FPS and 29.3 ms before).
- **A1, pipeline state:** `renderer_backend/state.h` (blend factors and operations, compare and
  stencil operations, cull mode, winding, topology, polygon mode, logic operations, color write
  masks), checked against Vulkan in `vk_state.h`. The pipeline key's write masks, the shared
  blend rules (`AttachmentBlend`) and the PS4-state conversions use it. Same warm caches and
  frame times. Sampler and view settings, usages, aspects and layouts move with A2, where
  sampler, view and image creation go through the adapter.
- **A2, samplers:** `SamplerDesc` and the sampler vocabulary (filters, mipmap modes, address
  modes, border colors), `Device::CreateSampler`/`DestroySampler` (deferred) and
  `NativeHandle`. The texture cache builds each description from the S# with the device's
  capabilities and keeps the native handle, so binding needs no lookup; the Vulkan backend
  keeps the samplers in a handle table. Metal API validation was clean on MoltenVK and Metal;
  28.9 and 34.3 FPS in plain runs.
- **A2, buffers:** `Device::CreateBuffer`, `ImportHostMemory` (the macOS guest pool),
  `ImportGuestMemoryChunk` (Linux, BB_GUEST_IN_PLACE), deferred `DestroyBuffer`, and
  `FlushBuffer`/`InvalidateBuffer`. The VMA allocation (`UniqueBuffer`) lives in the Vulkan
  backend; `VideoCore::Buffer` keeps its interface and its native handle for Vulkan code that
  still records directly (until A3/A4). Every buffer is created from a `BufferDesc`. Metal API
  validation was clean on both renderers; 29.8 FPS (MoltenVK) and 36.4 (Metal) in plain runs.
- **A2, images:** `ImageDesc` (type, format, extent, levels, layers, samples, usage, and whether
  3D images are viewed as 2D slices or compressed images as uncompressed),
  `Device::CreateImage`, deferred `DestroyImage`, `SupportedFormat` and `SupportedSampleCounts`.
  Each of the texture cache's backing images keeps its handle and the description it was
  created from (the backing for another sample count is the same description); Vulkan code
  reads that description instead of a `vk::ImageCreateInfo`. `UniqueImage` lives in the Vulkan
  backend, where the upscalers and scene proxies keep using it for their own images. The
  aspect, usage and format-feature fields the barriers use stay until A3. Validation was clean;
  image memory and frame time were unchanged (Metal, alternating with the previous commit:
  34.6 and 36.0 FPS against 33.8 and 37.0).
- **A2, views:** `ImageViewDesc` (view type, format, aspect, levels, layers, component mapping,
  usage, minimum LOD) and the view vocabulary (view types, aspects, swizzles),
  `Device::CreateImageView` and deferred `DestroyImageView`. `ImageViewInfo::mapping` is the
  adapter's; `ImageView::Handle()` gives Vulkan code the native view. Validation was clean; in
  four alternating Metal pairs the views build averaged 35.0 FPS against 35.8 (one run slowed
  late, with the whole machine's per-draw time up; it did not recur), and the GPU thread's
  recording was 4.3–4.4 µs per draw against 4.2.
- **A3, ticks:** the texture and buffer caches follow the GPU through the device
  (`CurrentTick`, `IsComplete`, `Wait`) and free through `DeferRelease`, not the scheduler.
  The blit helper, tile manager and fault manager are Vulkan code inside the caches' folders
  and keep the scheduler until their work moves into the backend. Same frame time
  (alternating Metal pairs: 35.9 and 36.2 FPS against 36.3 and 36.1).

## Decisions

Taken by the maintainer on 2026-10-22:

1. **The route:** the adapter replaces the interception route chosen on 2026-10-20. shadPS4
   upstream fixes are ported by hand.
2. **Vocabulary:** own enums, with Vulkan's numeric values at first, so the shader and pipeline
   caches stay valid.
3. **FSR 4 and DLSS** stay Vulkan-backend only.

Open until M3: FSR 3.1 on the Metal backend (a FidelityFX port to Metal, as the fork has, or
MetalFX only on that backend).
