# Render adapter (N4, restarted)

[← Documentation](README.md) · [Native renderer](NATIVE_RENDERER.md)

**State:** approved on 2026-10-22; step A0 is next. The decisions are at the end.

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
| A0 | Adapter headers; a Vulkan backend that wraps today's objects; `Device` owns instance, queue and VMA | No behavior change; FPS and p90 unchanged in alternating runs |
| A1 | Own vocabulary in the caches and pipeline keys | Shader and pipeline caches still load warm |
| A2 | Buffers, images, views and samplers created and destroyed through the adapter, with deferred release | Metal API validation clean; texture memory unchanged |
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

## Decisions

Taken by the maintainer on 2026-10-22:

1. **The route:** the adapter replaces the interception route chosen on 2026-10-20. shadPS4
   upstream fixes are ported by hand.
2. **Vocabulary:** own enums, with Vulkan's numeric values at first, so the shader and pipeline
   caches stay valid.
3. **FSR 4 and DLSS** stay Vulkan-backend only.

Open until M3: FSR 3.1 on the Metal backend (a FidelityFX port to Metal, as the fork has, or
MetalFX only on that backend).
