# Renderer changes from bbport

From N4 onward, the shared renderer core and Vulkan backend are maintained here. For each bbport
release, merge non-renderer changes as usual. Review changes to `renderer_vulkan/`,
`buffer_cache/`, `texture_cache/` and `shader_recompiler/` individually. Record what was ported,
skipped or deferred, why, and the build and game measurements that checked a port.

| bbport release | Renderer changes | Decision |
|---|---|---|
| 0.51 (2026-10-11) | Buffer-cache handling of GPU-written copies and low-VRAM allocation; texture-cache copy and eviction changes; swapchain retirement on Wayland; DLSS and temporal-upscaler changes and diagnostics. No shader-recompiler files changed from 0.5 in the four tracked directories. | Full release merge completed before N4. These changes are already in the Vulkan reference path; no hand port was needed. |

The 2026-10-12 pre-N4 check found upstream HEAD at the 0.51 release commit
`3c57da4198ad62039f63f33bfd205b9916518acf`. There was no newer release to merge.
