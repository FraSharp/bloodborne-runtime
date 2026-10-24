# bloodborne-runtime

What runs Bloodborne (PS4, CUSA03173 1.09) outside a PS4: it loads the game's executable, provides
the PS4 system libraries the game calls (threads, memory, files, saves, pad, audio, video out),
and draws its graphics through Vulkan with a renderer derived from shadPS4, with temporal
upscaling (FSR 3.1, FSR 4, FSR 4.1.1). On macOS that Vulkan runs on MoltenVK, or, experimentally,
the renderer encodes Metal itself ([native renderer](docs/NATIVE_RENDERER.md), `BB_METAL_FRAME=1`).

It is **built on [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28**, the
native Linux port this work started from as a fork, whose releases keep being merged or ported
here (0.51, and parts of 0.6: [Upstream](docs/UPSTREAM.md)). On Apple Silicon the game's code runs through bbcpu, the CPU layer of
[bloodborne-recomp](https://github.com/bloodborne-native/bloodborne-recomp) (the `recomp/` submodule),
or as recompiled native code it generates.

| Repository | |
|---|---|
| [bloodborne-recomp](https://github.com/bloodborne-native/bloodborne-recomp) | The recompiler and bbcpu: portable |
| **bloodborne-runtime** (this one) | Loads the game: the PS4 libraries, the renderer |
| [bloodborne-launcher](https://github.com/bloodborne-native/bloodborne-launcher) | The independent launcher: profiles, saves and settings |
| [bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac) | Packages the launcher as the macOS app, with releases and player guides |

**Players:** download the app from [bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac/releases/latest).

**Community:** the project's [Discord](https://discord.gg/EyyQ23sTBg) (`#bloodborne-runtime` for this repository).

## Build and run

```bash
git clone --recursive https://github.com/bloodborne-native/bloodborne-runtime.git
cd bloodborne-runtime
bash build.sh                                  # macOS: the first run also builds deps/
BB_GAME_DIR=/path/to/your/game bash run.sh     # your own dump, with the 1.09 update
```

[Building](docs/BUILDING.md) · [Architecture](docs/ARCHITECTURE.md) ·
[Documentation](docs/README.md) · [Credits and licenses](docs/CREDITS.md)

## Credits

The loader, the PS4 runtime, the renderer's extensions, the upscalers and the game-specific fixes
come from [bbport](https://github.com/deadinside28/bloodborne_pc); the video core and shader
recompiler from [shadPS4](https://github.com/shadps4-emu/shadPS4). The Metal backend adapts
shader and pipeline work from [Huax-1839/bloodborne-macos](https://github.com/Huax-1839/bloodborne-macos)
through [Wooloomooloo2/bloodborne_mac_port](https://github.com/Wooloomooloo2/bloodborne_mac_port),
which developed the command layer and frame encoder further. The macOS platform layer and arm64
path were added here. Full list:
[Credits and licenses](docs/CREDITS.md).

Licensed under the [GNU GPL v2 or later](LICENSE). Not affiliated with shadPS4, Sony Interactive
Entertainment, FromSoftware or AMD. *Bloodborne* is a trademark of Sony Interactive Entertainment.
No game files are included.
