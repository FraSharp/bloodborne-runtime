# Render adapter (N4, restarted)

[← Documentation](README.md) · [Native renderer](NATIVE_RENDERER.md)

**State:** approved on 2026-10-22. A0 to A6 are done (the renderer on the adapter with the Vulkan
backend), and M1: with `BB_GFX_BACKEND=metal` buffers and images are made in Metal.
M2 is in progress: step 1, the command stream, is done (everything both backends run records
through the adapter); in step 2, the Metal recorder encodes every command the shared code
records into the Metal frame directly. Step 3 is done: with the Metal frame, shaders and
pipelines are Metal's only. In step 4 the window is presented through Metal, the present is
drawn in Metal, and the upscaler's own passes are encoded in Metal. The stream's skeleton is
done: every submission is committed in Metal; command buffers are still begun and ended in
MoltenVK (empty: it never runs them). Step 5, the acceptance: the automated runs passed (below);
a play session on a copied save by the maintainer is what remains before M2 is closed.
The decisions are at the end.

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
`vk_pipeline_key.h` holds no Vulkan handles already). The shader cache (keys, SPIR-V and
pipeline data) is shared; the Metal backend adds its binary archive. N2 later replaces the SPIR-V input with
shaders converted ahead of time.

Pipelines hold their shaders, so a shader is freed when its caller and its last pipeline have
released it. The Vulkan backend does so since A5. The Metal shadow still keeps every shader for
the session: its PSO cache keeps pipeline states built from shaders compiled into each module.
The Metal backend's pipelines (M2) hold their shaders the same way, which closes that roadmap
item.

### Presentation and native interop

- `Swapchain`: acquire, present, resize, HDR and vsync choices; the backend owns the platform
  surface (CAMetalLayer on macOS).
- `NativeHandle(object)` returns the backend's object (`VkImage`, `id<MTLTexture>`, a command
  buffer) for code that must call a vendor library directly: FSR 3.1 (portable Vulkan
  FidelityFX), FSR 4 and DLSS on the Vulkan backend, MetalFX and FSR 4.1.1 (`vk_fsr4_metal`) on
  the Metal backend, and ImGui's renderer. These are the only places outside a backend that see a native API, each in its own
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
| A3 ✓ | `vk_runtime`'s copies, clears and uploads as commands with access declarations; barriers inside the backend | Same images and frame time; no validation errors |
| A4 ✓ | Rasterizer: passes, draws, bindings, dynamic-state block | GPU-thread recording ≤ 5.6 µs per draw (today 5.1–5.6) |
| A5 ✓ | Pipelines and shaders through the adapter; pipelines own shaders | 550 pipelines build; unused shaders freed |
| A6 ✓ | Presenter, overlay, upscalers through `Swapchain` and `NativeHandle` | Every upscaler choice renders on Vulkan |
| M1 ✓ | Metal backend: device, queue, resources (heaps; the pool without copies), transfers | Loading reaches the Hunter's Dream with `BB_GFX_BACKEND=metal` |
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
- **A3, copy regions:** `BufferCopy` and `BufferImageCopy` are the adapter's; the buffer and
  texture caches build them, and `Runtime` converts them where it records. The readback
  queue and the tile manager (Vulkan code) convert at their calls. A frame grabbed from each
  build in the same scene matched; 37.1 FPS against 36.5 on Metal, 29.5 on MoltenVK.
- **A3, barrier state:** each image allocation's layouts and last accesses (`ImageState`) live
  in the Vulkan backend's record of it (`ImageRecord`, at a fixed address); the backing keeps
  an opaque pointer, so a transition costs no lookup. Computing barriers moved from
  `VideoCore::Image` into `Runtime`. The texture and buffer caches no longer name a layout,
  stage or access mask. The rasterizer still transitions images with Vulkan's vocabulary; its
  access declarations come with A4. Validation was clean; frames grabbed on Metal and MoltenVK
  were correct; alternating Metal pairs 36.7 and 35.6 FPS against 35.9 and 36.3, recording
  4.3–4.6 µs per draw against 4.5–4.9.

- **A4, access declarations:** the rasterizer declares how a draw or dispatch uses each image
  (`ImageAccess`: sampled, storage, sampled while a target, color target with or without
  feedback, depth target written, read-only, or with stencil written) and buffer range
  (`BufferAccess`: graphics or compute, read or read-write; transfer write); `Runtime` maps them
  to the same layouts, stages and accesses as before, with the image's depth and stencil and the
  device's feedback-loop support. The rasterizer no longer names a layout, stage or access mask
  for its transitions (its descriptor writes still give `eGeneral` until the bindings move).
  Validation was clean; frames on Metal and MoltenVK correct; three alternating Metal pairs
  35.1, 35.7, 35.3 FPS against 34.8, 34.1, 35.5, recording 4.2–4.5 µs per draw against 4.2–5.2.

- **A4, bindings:** the rasterizer binds through the adapter's flat slots
  (`renderer_backend/binding.h`: `SlotWrite` with a slot, a count, a kind and its elements;
  `BufferBinding`, `ImageBinding`), not Vulkan descriptor writes. The bindings share the layout
  of Vulkan's descriptor infos (checked at compile time), so the Vulkan backend reads them in
  place; it converts each write straight into the recording chunk
  (`Scheduler::AllocateRecordData`). A first version converted into a temporary array and then
  copied it: about 4% slower in three alternating Metal pairs. Without that copy: 33.4, 33.6,
  34.6 FPS against 34.0, 35.5, 33.5, recording 4.2–4.7 µs per draw against 4.3–4.6. Validation
  clean; the frame correct.

- **A4, dynamic state:** viewports, scissors (`Rect2D`), depth compare, stencil operations,
  cull mode, winding and color write masks are set in the adapter's vocabulary
  (`renderer_backend/pass.h`, laid out like Vulkan's where it matters); the Vulkan scheduler
  converts them when it records. Three alternating Metal pairs: 36.6, 38.0, 38.0 FPS against
  37.2, 37.8, 38.5; recording 4.3–5.3 µs per draw against 4.1–4.2, inside the 5.6 µs budget.
- **A4, render-pass targets:** `RenderAttachment` and `RenderState` are the adapter's (the
  backend's view and its own layout of the image; a target with a clear takes it as its load
  action); the scheduler converts them when it begins a pass. Measured alone on a quieter Mac:
  38.5 FPS against 38.4, recording 4.1 µs per draw against 4.2. (Measured together with the
  dynamic state earlier, the slice looked about 3% slower in six pairs; the cost did not
  reproduce once the Mac's background load dropped.)

- **A4, commands:** draws (direct, indirect, indirect count), dispatches (direct, indirect),
  index and vertex buffer binds, end-of-pipe label writes and debug labels go through the
  adapter's `CommandRecorder` (`renderer_backend/commands.h`, with `DrawCommand`,
  `DispatchCommand` and the breadcrumb `Crumb`); the Vulkan backend's recorder records each draw
  or dispatch as one command with its pipeline bind. Recorded commands live in the recording
  chunks, so each form captures only its own fields: capturing the whole 80-byte `DrawCommand`
  raised recording from ~4.2 to ~5.8 µs per draw. Measured: 38.6 and 38.7 FPS against 37.9,
  4.1 µs per draw against 4.2. The rasterizer's only remaining Vulkan call is the vertex input
  state, which moves with the pipelines (A5).

- **A5, shaders:** game shaders are made through `Device::CreateShader` (SPIR-V in, a handle
  out; an empty handle when the driver rejects the code, which the shader-cache loader treats
  as a damaged entry) and freed through `DestroyShader`. The Vulkan backend registers the SPIR-V
  with the Metal path itself. The pipeline cache keeps each module's handle (the probe module
  and `ReplaceShader` free through it). One Metal run: every Metal pipeline built (0 failed, no
  SPIR-V missing), the Vulkan cache warm, 38.5 FPS and 4.2 µs per draw, the frame correct.

- **A5, vertex input:** the vertex input state is described in the adapter's terms
  (`VertexBinding`, `VertexAttribute` in `renderer_backend/commands.h`: slot, stride, per vertex
  or per instance with a divisor; location, slot, format, offset) from the pipeline's guest
  attributes through draw preparation, and set through `CommandRecorder::SetVertexInput`, which
  the Vulkan backend converts straight into the recording chunk. The rasterizer records no
  Vulkan call of its own now. Measured: 38.9 FPS against 38.8, 4.05 µs per draw against 4.1,
  the frame correct.

- **A5, pipelines:** game pipelines are made by `Device::CreateGraphicsPipeline` and
  `CreateComputePipeline` from descriptions (`renderer_backend/pipeline.h`: stages with their
  shader handles, binding slots and push constants, vertex input, topology, rasterization,
  sample counts, color targets with their blending, depth and stencil formats, logic op) and
  freed through `DestroyPipeline`. `GraphicsPipeline` and `ComputePipeline` turn the guest key
  into a description; the Vulkan backend (`vk_backend_pipeline.cpp`) makes the set layout,
  pipeline layout and pipeline, and keeps them in a record the descriptor pushes read. The
  fixed-quad vertex shaders are made through the device too. There is no `VkPipelineCache`: it
  was never saved, and pipelines loaded from the shader cache never had one. Measured: every
  pipeline built (400 Metal pipelines, 0 failed), Metal API validation without new messages,
  37.7 FPS against 37.5 and 4.4 µs per draw against 4.4 (new build first), the frame correct.

- **A5, shaders held by pipelines:** a pipeline holds its shaders until it is destroyed;
  `DestroyShader` releases the caller's hold, and the module is freed (deferred) when no hold is
  left. The auxiliary shaders a pipeline makes for itself (rect- and quad-list tessellation, the
  clip-distance discard fragment), never freed before, are released once their pipeline is
  made and go with it. `ReplaceShader` can no longer free a module a live pipeline was made
  with. Measured: 400 pipelines built (0 failed), Metal API validation without new messages,
  4.38 µs per draw against 4.37, 37.8 FPS against 37.7, the frame correct. Every pipeline of
  the test run builds; the 550 of the acceptance were a longer session's count.

- **A6, resource binding:** a draw's or dispatch's resources are bound through
  `CommandRecorder::BindResources` (the pipeline's allocation, its slot writes and push
  constants). `PipelineAllocation::binding` carries the backend's state for it, so binding takes
  no lookup; the Vulkan backend reads its pipeline record there (layout, bind point, push stages,
  push or allocated set) and owns the descriptor heap. Guest pipelines no longer record anything
  themselves. Measured (new build second): 4.24 µs per draw against 4.34, 37.7 FPS both, the
  frame correct.

- **A6, host pass objects:** the renderer's own passes make their pipelines, shaders, samplers
  and images through the device: post-processing, FSR 1, camera motion and its debug overlay,
  and the upscalers' helper passes (merge, reactive mask, TAA and its sharpening, FSR 4's
  colour decode, encode and reactive blend). A description marked `host_pass` is made as those
  passes always were: one viewport and one scissor per draw, no depth or stencil test, its own
  culling, and no guest subgroup size; a binding layout can name its push-constant stages, and
  slots can hold a combined image and sampler. `HostPipeline` and `HostSampler`
  (`vk_host_objects.h`) own them for code that still records itself; camera motion dispatches
  through the recorder. FSR 1 (`BB_FSR1=1`) was not run: it needs a window larger than the
  game's output. Measured: no faults with the upscaler off, FSR 3.1 (MoltenVK), TAA and MetalFX
  (Metal), frames correct; frame times as the base build (FSR 3.1 35.4 against 35.2 ms median,
  TAA 31.5 both, MetalFX 29.9 both); Metal API validation (upscaler off) without new messages.

- **A6, frames:** the presenter's frame images and their views are made and freed through the
  device. The view of the upscaler's display image is no longer made and freed every frame: the
  upscaler keeps it with the image (Metal API validation had reported it destroyed while a Metal
  path command buffer still needed it, 225–463 times in 75 s below native render size; now 0).
  Measured: 38.6 FPS and 4.15 µs per draw, the frames correct.

- **A6, swapchain:** `renderer_backend/swapchain.h` is the adapter's swapchain (resize, acquire,
  present, presentable, image format, size and count, the acquired image as native handles,
  HDR); the present mode comes from the settings when it is made. The Vulkan swapchain
  implements it, keeping its semaphores for the presenter's submissions, and the presenter
  calls it through those names. Measured: 38.5 FPS, 4.15 µs per draw, the frame correct.

- **A6, upscaler images:** the images of the upscalers, scene resolution and object motion are
  made through the device (`HostImage`, `vk_host_objects.h`, from their Vulkan create infos)
  and freed once the GPU is past them; FidelityFX, MetalFX and FSR 4 receive their Vulkan
  objects. `ImageDesc::mutable_format` lets an image refuse views in other formats: made
  mutable, the upscaler images cost MetalFX about 2.5% (30.9 against 30.1 ms median); kept as
  they were, 30.1 against 29.9. Measured: no faults with FSR 3.1 (35.6 ms median against
  35.2–35.4), TAA (with Metal API validation, no new messages) and MetalFX, frames correct.

  A6 is done: every upscaler this Mac offers renders (off, FSR 3.1, TAA, MetalFX; FSR 4 and DLSS
  need AMD and NVIDIA GPUs, FSR 4.1.1 AMD's initializers). Still recorded in Vulkan outside the
  backend, for the Metal backend's presenter (M2) to replace: the presenter's barriers, blits,
  clears, fences and semaphores; the upscalers' and host passes' own recording (their per-frame
  views included); MetalFX's shared buffer and semaphore; the overlay's ImGui renderer, which
  stays native interop.

- **M1, the route:** until the Metal backend records (M2), it makes resources in Metal and hands
  them to MoltenVK through `VK_EXT_metal_objects` imports, so the Vulkan recording and the
  Metal path keep working (decided 2026-10-23). `BB_GFX_BACKEND=metal` selects it on macOS;
  `renderer_metal/mtl_resources.{h,mm}` holds the Metal side.
- **M1, buffers:** every buffer but sparse ones and imported host memory is an `MTLBuffer`
  (private storage for device-local memory, shared write-combined for upload and stream
  memory, shared for readback), imported as the `VkBuffer`'s memory and added to the residency
  set; shared ones are mapped through their contents. The guest pool was already used where it
  is (imported host memory). Measured with `BB_METAL_FRAME=1`: 38.6 FPS, 4.0 µs per draw, the
  frame correct; Metal API validation without new messages, 400 Metal pipelines built; on
  MoltenVK's recording with FSR 3.1, 35.5 ms median (35.2–35.6 with Vulkan-made buffers).
- **M1, images:** images are `MTLTexture`s (private storage) imported into MoltenVK
  (`VkImportMetalTextureInfoEXT`; MoltenVK binds no memory to them), made as MoltenVK would make
  them: 1D images as 2D textures, views in other formats or of other types allowed for mutable
  and layered images, render targets for attachments and for transfer destinations of
  uncompressed formats (MoltenVK clears through render passes). Formats Metal substitutes or
  swizzles, sparse, cube-compatible, 2D-of-3D and block-view images stay MoltenVK's; in the
  test run none were. Measured with `BB_METAL_FRAME=1`: 25.9 ms median against 26.0 with
  MoltenVK's images, 4.1 against 4.2 µs per draw; Metal API validation without new messages,
  400 Metal pipelines built; on MoltenVK's recording with FSR 3.1, 35.6 ms median; frames
  correct.

  M1 is done: the game loads a save and plays with `BB_GFX_BACKEND=metal`. Two parts of its
  plan move to M2, which records in Metal: transfers in Metal (MoltenVK records the copies
  until then) and heaps (nothing measured needs them before Metal manages its own memory).

- **M2, the plan:** (1) the command stream: what the scheduler and runtime record today
  (passes, draw state, access declarations, copies and clears, submissions) goes through the
  adapter's `CommandRecorder`, and code outside the backend stops recording Vulkan itself;
  (2) a Metal command stream from `mtl_frame`'s encoders; (3) Metal pipelines and shaders from
  `mtl_pso_cache` and `mtl_shader`, holding their shaders; (4) the presenter and upscalers on
  Metal; (5) the acceptance run. `BB_METAL_FRAME=1` keeps working until then.
- **M2, passes:** the recorder begins and ends passes (`BeginPass`, `EndPass`, `InPassWith`,
  `WillBeginPass`, `ForceNewPass`, `SetPassLabel`); the rasterizer's passes go through it.
  Measured (new build second): 4.22 µs per draw against 4.20, the frame correct.
- **M2, barriers and host dispatches:** the recorder places barriers between uses of memory
  (`Barrier`, `BufferBarrier`, with `MemoryUse`: compute, vertex and fragment shader reads and
  writes, indirect arguments, transfers, the host, or any). Compute pipelines take
  specialization constants. The tiling and detiling dispatches, the fault buffer pass, the
  indirect-argument guard and the copy shader's compute path make their pipelines through the
  device and record through the recorder (the copies hoisted before a pass are next, with the
  transfers). Measured (new build second): 4.24 µs per draw against 4.27, 25.9 ms median both,
  the frame correct; the fault buffer pass did not run (no faults).
- **M2, image accesses:** `ImageAccess` names the renderer's own uses too (`ComputeRead`,
  `ComputeReadWrite`, `TransferSource`, `TransferDestination`); camera motion, the upscalers,
  scene resolution, the tiling dispatches, the copy shader and the presenter declare their
  accesses in those terms instead of Vulkan layouts and stages (the present-frame dumps, a
  diagnostic, still name theirs). Measured: MetalFX 30.1 ms median, TAA 31.7, FSR 3.1 35.7
  (29.9–30.1, 31.5 and 35.2–35.6 before), no faults, frames correct.
- **M2, clears and copy regions:** colour clears carry their value as the adapter's
  `ClearColor` (32-bit words), deferred clears included; the copy shader's copies, the
  rasterizer's buffer copies and the tiling downloads pass the adapter's regions. Measured (new
  build second): 4.15 µs per draw against 4.22, 25.9 ms median both, the frame correct.
- **M2, the backends' own presenters and upscalers:** the presenter and the temporal upscaler
  stay the Vulkan backend's: their passes are tied to vendor libraries that exist only there
  (FidelityFX on Vulkan, FSR 4, DLSS) and to their submissions. The Metal backend gets its own
  (step 4), with MetalFX and FSR 4.1.1 on Metal; TAA and the sharpening and merge passes, whose
  shaders both use, are dispatched by each. The command stream (step 1) covers the code both
  backends run.
- **M2, image transitions:** the recorder transitions images the device made by declared
  access (`TransitImage`, from the barrier state the backend keeps for each image); the access
  mapping is shared with the runtime's. Object motion's target uses it. Measured: MetalFX 30.1
  ms median, FSR 3.1 35.7, object motion active, frames correct.
- **M2, queries and fills:** occlusion query pools are device objects
  (`CreateOcclusionQueries`); the recorder begins, ends and resets queries, copies their
  counts into buffers and fills buffers. The translation of the game's occlusion queries uses
  them. It needs `BB_OCCLUSION_QUERIES=1` and guest memory used in place (Linux), so on the Mac
  this is built, not run. Measured (new build second): 4.13 µs per draw against 4.30, frame
  correct.
- **M2, timestamps:** timestamp query pools are device objects too (`CreateTimestampQueries`,
  `ResetQueries` and `ReadQueryResults` from the CPU; the timestamp bits and period are
  capabilities), and the recorder writes timestamps. The game's GPU clock reads and the GPU
  profile (`BB_GPU_PROFILE=1`) use them. The profile reports on MoltenVK's recording (13
  reports in 75 s); on the Metal path it reports nothing, as before. The game took no GPU
  clock reads in the test scene.
- **M2, draw state:** the draw state (viewports, scissors, depth, stencil, culling, depth bias,
  blend constants, write masks) is the adapter's `DrawState` (`renderer_backend/draw_state.h`):
  tracked with its changes, set through `CommandRecorder::GetDrawState` and recorded with
  `CommitDrawState`; the Vulkan backend's `DynamicState` adds the commit as dynamic state. The
  rasterizer uses the recorder for it. Measured, new build first: 4.24 µs per draw against
  4.23, 26.1 ms median both (a base-first pair read 4.60 against 4.07, with that new run's
  frame rate low throughout; the reversed pair did not repeat it).
- **M2, transfers:** `renderer_backend/transfers.h` is the adapter's transfers and declared
  uses on the caches' objects (buffer and image copies, uploads and downloads, fills and
  updates, clears with their deferral, image and buffer accesses, barrier flushes, the staging
  pool). The Vulkan `Runtime` implements it (`final`: calls through it stay direct); the
  texture cache, its images, the tile manager and the buffer cache hold the interface. Measured,
  new build first: 4.11 µs per draw against 4.17 (a base-first pair read 4.61 against 4.18
  again; reversed, nothing), the frame correct. The rasterizer holds the interface too
  (virtual calls on the draw path, its accesses per bound image): 4.34 µs per draw against
  4.58, 26.5 ms median both, new build second. It still makes the Vulkan backend's upscaler
  parts with the Vulkan runtime.
- **M2, ticks:** the device reports the last tick known complete (`CompletedTick`), asks the GPU
  (`RefreshTicks`) and waits with a timeout (`WaitFor`: complete, timed out or device lost). The
  rasterizer (its GPU signal thread, labels, idle flushes, the residency set's ticks) and the
  buffer cache's write checks use the device instead of the scheduler's semaphore. Measured in
  three pairs (base first, reversed, base first): 4.29/4.28/4.17 µs per draw against
  4.19/4.14/4.10, median 26.7/26.2/26.4 ms against 26.2/26.0/25.9 (about 1.5%). No call on the
  draw path changed (the tick reads are now virtual calls of a few a frame to a few thousand).
  Measured against M1 (`4ca9561`), M2 so far costs nothing: median 26.3 against 26.4 ms (M1
  first) and 26.0 against 26.1 (reversed), 4.22 against 4.25 µs per draw reversed.
- **M2, stream control:** the device has a command stream (`Device::Recorder`, the draw
  scheduler's), and the recorder submits, finishes, kicks the recording threads, runs the work
  whose submissions are done and queues the host copies (guest-memory copies the CPU makes in
  stream order). The rasterizer and the caches use it instead of the scheduler.
- **M2, the rasterizer without Vulkan types:** buffers, views, pipelines and samplers are
  native handles (`Native()`; slot bindings through `BindingOf`, null bindings through
  `NullBufferBinding`); image bindings and attachments carry the layout the transfers
  interface reports (`BindingLayout`) or `GeneralLayout`; index types, topologies and formats
  are the adapter's (formats named with `FormatName`); the vertex input comes in the adapter's
  terms without a separate divisor list. The upscaler's and scene resolution's targets are
  read through native accessors. `vk_rasterizer.cpp` names no Vulkan type. Measured: 4.42 µs
  per draw against 4.21 (new second) and 4.22 against 4.11 (new first), median 26.3 against
  26.0 and 26.8 against 26.7; MetalFX 30.1 ms median as before; frames correct.
- **M2, caches without Vulkan services:** the readback queue's copies are a device call
  (`ReadBackAfter`, the Vulkan backend's `vk_backend_readback`); the shader recompiler's
  profile comes from the device (`ShaderProfile`), with the game's and the memory model's
  fields set by the pipeline cache; programs, selections and the shader cache's loader
  identify shaders by `ShaderHandle` (the Metal shadow and the shader collector look up the
  Vulkan modules); device addresses are `u64`; the import usage flags are the backend's.
  Measured: 400 pipelines built, 0 failed, the stale count as the base build's (59); new
  build second 4.68 µs per draw against 4.48, reversed 4.31 against 4.52; frame correct.

- **M2, capabilities and memory:** the device's limits and features the renderer asks for
  (binding alignments, an integrated GPU, tessellation, geometry shaders, primitive restart,
  null descriptors, sample-count mixing, depth range and clip control, buffer markers,
  viewport, framebuffer and sampler limits, workgroup counts, exact occlusion counts, vertex
  shader stores) are `Capabilities`; `Device::ReportMemory` gives usage, budget and total from
  one driver query, and `Capabilities::memory_report` says whether it can. The first version
  asked `ReportMemory().available` on every guest submission (four driver queries where there
  had been one) and cost 5% of the GPU thread's time per draw in both orders (4.24 against
  4.49 µs, 4.03 against 4.23); with the capability, 4.08 against 4.14 (new second). The
  rasterizer records through `Device::Recorder`, and the copy shader's copies hoisted before a
  pass are `Transfers::CopyBufferBeforePass`.
- **M2, the renderer's own passes:** `renderer_backend/host_pipeline.h` makes a host pass's
  compute pipeline on any device (`MakeCompute`, `OwnedPipeline`). The indirect-argument
  guard, the constant ring, GPU timestamps, occlusion queries, the tile manager and the copy
  shader's multi-copy use it and the device's recorder. Work that may not span passes or
  submissions registers as the recorder's `CarriedScope` (occlusion queries; Metal's visibility
  results end with each encoder too). `Device::AfterCompletion` runs the texture cache's
  downloads once the GPU is done. Measured: pairs read 3–6% apart in both orders and a build
  without the per-draw changes the same, while the same new build alone read like the base
  (4.06–4.14 against 4.05–4.09 µs): the spread was between runs.
- **M2, null stand-ins and motion vectors:** the null-descriptor stand-ins, object motion and
  camera motion (its pass and the `BB_DEBUG_MOTION` overlay) are made by the device and record
  through the recorder (`ClearImage`, `StateOf` for a device-made image's state value). The
  overlay made two image views a frame and freed them after its dispatch; through the
  recorder the dispatch can land in a later command buffer, and Metal API validation found 78
  views destroyed while still required. It uses the texture cache's views now: none. Measured
  (new second): 4.00–4.04 against 4.08–4.15 µs; with MetalFX, object motion and the overlay,
  25.5 against 25.3 FPS and the same frame.
- **M2, the caches:** the texture cache takes no Vulkan object; stream buffers wait on the
  device's ticks; the recorder says whether it records later on another thread
  (`DefersRecording`). The texture cache's unused blit helper is gone; the Vulkan runtime keeps
  its own for multisample copies (part of its transfers). Measured: 4.04–4.17 µs both.
- **M2, scene resolution:** reduced proxies are device-made images and views, moved between
  uses with `TransitImage` and copied with `CommandRecorder::BlitImage` (nearest, whole
  levels). Depth formats without blits are resampled by `Transfers::ResampleDepth` (the Vulkan
  runtime's `DepthResampler`, the former fullscreen draw). `Device::CanBlit` keeps its answers
  (asked for every attachment of every draw). Transfers gives scene resolution its hook on
  native accesses and the GPU profiler's transfer marks. Measured with MetalFX at Quality
  (1280x720): 4.36–4.49 against 4.41–4.49 µs, 24.8 against 24.7 FPS; Metal API validation
  clean.
- **M2, pipelines and stream waits:** guest pipelines and the pipeline cache take the device;
  the rasterizer's host-copy waits and deferred signals go through the recorder; the buffer
  cache sets the submit hook (pending barriers, its sparse binds); the GPU profiler starts with
  the Vulkan device; `Device::InstallGuestMemory` sets up guest memory used in place. Measured
  (new second): 4.05–4.20 against 4.02–4.13 µs.
- **M2, step 1 done:** everything both backends run records through the adapter. Measured
  against M1 (`4ca9561`, M1 first): 4.04–4.17 against 4.05–5.01 µs per draw, 29.8 against 29.9
  FPS.

  Left in shared code for the Vulkan backend: the buffer cache's sparse guest arena and its
  fault manager (sparse buffers, Linux only; guarded by the capability), the guest-memory
  chunks used in place (`BB_GUEST_IN_PLACE`, Linux), the staging pool's type in the transfers
  interface, and the Vulkan flags `VideoCore::Image` keeps (aspects, usage, format features).
  The rasterizer holds the Vulkan instance and scheduler only to make the Vulkan backend's
  parts (the sparse arena, the upscaler). These move behind the device before M4 drops Vulkan
  from the Mac build.
- **M2 step 2, the Metal recorder:** with the Metal frame (`BB_METAL_FRAME=1`) the device's
  recorder is the Metal recorder (`renderer_metal/mtl_recorder.mm`). The Metal frame already
  owns every command buffer and encodes each Vulkan call into Metal (`mtl_vk_layer`); the
  Metal recorder encodes its commands into the owned command buffer's Metal state itself
  (`MetalLayer::OwnedFor`), so they are no longer Vulkan calls translated back. It covers the
  draw state (one recorded command per commit with the changed fields, where the Vulkan
  recorder records one per field), draws and dispatches with their pipelines, vertex and index
  buffers, push constants and descriptors, barriers and image transitions, clears, blits,
  fills and GPU-written labels. The Vulkan runtime's transfers and the scheduler's passes
  encode through a `StreamEncoder` (`vk_stream_encoder.h`): Vulkan commands, or the Metal
  frame's encodings. Measured with the Metal frame, each slice against the one before it
  (base first): the draw state 37.5 against 36.8 FPS; draws and bindings 37.0 against 36.4;
  barriers and clears 38.7 against 35.6; transfers 37.2 (its base run was slow throughout)
  and 38.2 in a second run; passes 38.2 against 37.8; labels 37.9 against 38.4 (new second).
  Frames correct throughout, with MetalFX, scene resolution and object motion too.

  Still Vulkan on the Metal frame: the command buffers' begin and end and the submissions
  (how the layer makes and commits its Metal command buffers; MoltenVK's empty submissions
  signal the scheduler's ticks), debug labels and breadcrumb markers, draws whose count the GPU
  reads and image resolves (the Metal frame encodes neither), and the Vulkan backend's own
  presenter and upscaler (step 4).
- **M2 step 3, pipelines and shaders:** with the Metal frame every draw and dispatch binds the
  Metal pipeline, so MoltenVK's builds were unused work. The device asks the layer for a
  Metal-only pipeline (`MetalLayer::MetalOnlyPipeline`, a placeholder handle MoltenVK never
  sees) and builds the Metal pipeline itself: guest pipelines from their own state, which
  their description carries (`GuestGraphics`, `GuestCompute`: the pipeline cache's key, guest
  state only, and the shaders by guest stage; the Metal builder emulates rect and quad lists,
  tessellation and geometry shaders from it), the renderer's own from the create info the
  device makes. Shaders are their SPIR-V registered with the Metal builder under placeholder
  handles, without MoltenVK modules; pipelines hold their shaders, and the last release drops
  the builder's copy by handle (the compiled forms stay with the content cache the session's
  Metal pipelines point into). The pipeline cache no longer calls the Metal builder itself.
  Measured with the Metal frame: the same 400 Metal pipelines, 0 failed, no draw without one;
  FPS as before each slice (38.6 against 34.0 with a slow base run, 37.9 against 38.0, 36.9
  against 36.3); 400 shaders with 6.7 MiB of SPIR-V no longer copied into MoltenVK modules.
  The pipeline cache keeps its programs for the session, so no shader is released early in
  play.
- **M2 step 4, the window through Metal:** with the Metal frame the swapchain presents the
  window's `CAMetalLayer` itself (`renderer_metal/mtl_present.mm`), with no MoltenVK surface or
  swapchain. Its images are offscreen ones the device makes; the presenter draws into them as
  before (the letterboxed frame, the settings overlay, the FPS counter), and each present
  copies one into the layer's next drawable on MoltenVK's queue, after the presenter's
  submission, and presents it. No swapchain semaphores (the copy is ordered on the queue); an
  image is reused once its present's copy completed; vsync unless the present mode is
  Immediate; HDR10 as the Vulkan swapchain had it. A screen grab (`tools/grab.sh --screen`)
  writes the drawable as presented. Measured with the Metal frame: 36.4 against 37.9 FPS (new
  second), 36.5 against 36.6 (new first); the GPU thread's time per draw read higher for the
  new build in both orders (4.26–5.19 against 4.10–4.28 µs new first), unexplained (nothing in
  the change runs on that thread). Metal API validation clean; the drawable grab shows the
  frame letterboxed with the FPS counter. Not yet tried: window resizing, full screen,
  minimising, HDR output and Immediate mode.
- **M2 step 4, the present drawn in Metal:** the presenter no longer draws into an offscreen
  image for the swapchain to copy. With the Metal frame one Metal command buffer, after the
  frame's work on the queue, clears the drawable to black, draws the frame letterboxed into it
  (a full-screen triangle sampling it linearly, as the Vulkan blit filtered), draws the
  settings overlay and FPS counter with Dear ImGui's Metal backend
  (`shim/bbport_overlay_metal.mm`; the overlay's frame logic is shared), and presents it. A
  frame is reused once its present has read it; the swapchain keeps no images. The present
  scheduler's submission stays, empty but for the diagnostics' copies (the frame dumps), for
  the frame's fence until Metal submissions replace it. Measured: 36.5 against 37.0 FPS (new
  second); validation clean; the drawable grab as before.
- **M2 step 4, the upscaler's passes:** the upscaler stays the Vulkan backend's own class, but
  what it records goes through the device: TAA and its sharpening (barriers, the histories'
  transitions by declared use, bindings, dispatches) and its other compute passes (the scene
  merge, the reactive mask, the extra sharpening after MetalFX and FSR 4, FSR 4's encode,
  decode and reactive passes) through the recorder; its barriers, blits, copies, depth clear
  and diagnostic download through the stream encoder (`ActiveStreamEncoder`, the device's).
  With the Metal frame all of it is encoded in Metal directly. Still recorded as Vulkan by
  the upscaler: FSR 3.1 (FidelityFX's library) and DLSS. Measured with the Metal frame: TAA
  30.2 against 30.4 FPS, FSR 3.1 32.6 both, MetalFX at Quality 29.8 against 30.3 (new
  second each); MoltenVK as before; frames correct.
- **M2, the skeleton: submissions committed in Metal:** with the Metal frame the layer commits
  the owned command buffers of a submission itself (`SubmitInMetal` in `mtl_vk_layer.mm`)
  instead of handing it to MoltenVK. Timeline semaphores (the scheduler's ticks) are created
  with their Metal shared event exported, and a submission's signals are encoded on its last
  command buffer (`encodeSignalEvent`); a submission with no command buffers commits an empty
  one for them. Its waits are already ordered by the single queue, so a submission goes to
  MoltenVK only when it has a fence, a binary semaphore, a semaphore without a shared event,
  or waits for a value not yet committed. `BB_METAL_SUBMIT=0` hands every submission to
  MoltenVK. The presenter's submission signals no fence with the Metal frame: a frame records
  the present scheduler's tick and waits on it. Measured with the Metal frame (new second):
  36.3 FPS both with the presenter still fenced (~335 submissions a second in Metal, ~35
  through MoltenVK; Metal API validation clean), then 39.6 against 38.7 with its tick (~433 a
  second in Metal, none through MoltenVK; 4.06–4.20 µs per draw against 4.13–4.63); frames
  correct. Debug labels, timestamps and occlusion queries on these command buffers are not
  run: nothing submits MoltenVK's side of them. Not taken: leaving MoltenVK's command buffers
  unbegun (the layer answering begin and end) was slower in both orders, 38.6 against 39.5
  and 38.2 against 39.4 FPS (4.31–4.73 against 4.11–4.28 µs per draw), cause unknown; they
  stay until MoltenVK leaves (M4).
- **M2 step 5, the acceptance (automated part):** at the test save's place (not the Hunter's
  Dream of the 65.5/61.7 measurement), 1080p, FSR 3.1 Balanced: the Metal frame 36.2 FPS
  (median 27.3 ms, p90 28.8) against MoltenVK's 27.6 (35.5, 38.1); a 10-minute Metal-frame
  session, 21,904 frames, no faults, 36.6 FPS (median 27.0 ms, p90 28.5), ~395 submissions a
  second in Metal and none through MoltenVK; frames correct.

## Decisions

Taken by the maintainer on 2026-10-22:

1. **The route:** the adapter replaces the interception route chosen on 2026-10-20. shadPS4
   upstream fixes are ported by hand.
2. **Vocabulary:** own enums, with Vulkan's numeric values at first, so the shader and pipeline
   caches stay valid.
3. **DLSS and FSR 4's Vulkan path** stay Vulkan-backend only. FSR 4.1.1 on macOS runs in Metal
   (`vk_fsr4_metal`, merged 2026-10-23), reaching the images through `VK_EXT_metal_objects`;
   on the Metal backend it takes them through `NativeHandle`.

Taken by the maintainer on 2026-10-24:

4. **FSR 3.1 on the Metal backend:** ported, not dropped. AMD's FidelityFX host code and shaders
   stay; a Metal back end for its interface (as Wooloomooloo2's fork has, `gpu/shim/ffx_metal/`,
   credited when taken) runs them. It lands in M3, after M2's Metal submissions.
