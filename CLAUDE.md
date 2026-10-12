# bloodborne-runtime

Runs Bloodborne (PS4, CUSA03173 1.09) outside a PS4: the loader, the PS4 system libraries
(`src/runtime_*.c`), and the renderer (`gpu/`, derived from shadPS4, Vulkan through MoltenVK on
macOS). Built on bbport, whose releases are merged here. The game's code runs through bbcpu from
the `recomp/` submodule (bloodborne-recomp), or as its recompiled library (`BB_RECOMP_LIB`).

## Layout

- `src/probe.c` (the program, `out/bb-probe`), `src/runtime*.c`, `src/platform.{c,h}` (Linux and
  macOS differences: `bb_shared_memory`, the `BB_UC_*` signal-context macros, the image base).
- `gpu/`: `shadps4/` (video core, shader recompiler), `shim/` (window, overlay menu, guest hooks,
  Metal residency), `layer/`, `cp/`, `games/` (game profile). Built by CMake into `libbbgpu`.
- `scripts/`: `prepare.py`, `link_modules.py` (boot image), `patches.py`, `game_check.py`, `mods.py`,
  `launcher_api.py`; `scripts/macos/build-deps.sh` (MoltenVK, SDL3, FFmpeg, ... into `deps/`).
- `run.sh` (runs the game; the app calls it), `build.sh`, `patches/Bloodborne.xml`.
- `tools/`: `mac_bench.sh` (background run, frame statistics; `SAMPLE=1`, `TRACE=1`),
  `test_run.sh` (the same with a copy of the player's save: use this one), `grab.sh`, `press.sh`
  (pad input), the Metal trace readers (`mst_*.py`), `fsr4cap/`.
- `docs/`: `guide/building.md`, `guide/how-it-works.md`, `MACOS_PERFORMANCE.md`, `CREDITS.md`,
  upstream's notes (many in Russian).

## Build, run, test

```sh
bash build.sh                                   # macOS: builds deps/ the first time (~1 h)
BB_GAME_DIR=~/Downloads/PS4/Bloodborne/CUSA03173 bash run.sh
tools/test_run.sh NAME [VAR=VALUE...]           # background run with a copy of the save
cd tests && python3 -m unittest discover -p 'test_*.py'
```

The user's game dump is `~/Downloads/PS4/Bloodborne/CUSA03173`; `deps/` takes about an hour to
build and records absolute paths (a moved or copied `deps/` must be rebuilt). Metal
System Traces need `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`; what helped, what
did not and what made things worse is in `docs/MACOS_PERFORMANCE.md` (read it before trying a
performance idea).

`test_probe.LoaderTests` fails on macOS (its images use 4 KiB-aligned segments; Macs have 16 KiB
pages): known, not a regression. A healthy run in the Hunter's Dream is ~58–62 FPS on the user's
M3 Pro; FPS is bound by rendering, so CPU-side changes are measured in CPU time.

## Command buffers

The game's main thread and GXRenderThread both record into the main graphics command buffer while
an area loads; `src/runtime_cmdbuf.c` holds a per-buffer lock across the game's command buffer
methods (bbcpu guards; `BB_CB_GUARD=0`: off). `BB_CB_PROBE=1` reports any buffer two threads use at
once, with their names and callers: the first thing to run on a "PM4 type 0" or "Bad PM4 packet".

## Native renderer

`docs/NATIVE_RENDERER.md`: milestone R5, the engine's graphics on each platform's own API (steps
N0–N6, N0 and N1 done). Multiplatform: shared core, Vulkan and Metal backends; nothing Metal-only.
N4's design is approved and Linux CI passes. The first buffer-resource slice is opt-in
(`BB_N4_BUFFER_DESC=1`); uncapped and capped A/B game gates pass in Central Yharnam. Handle
issuance, completion-based retirement and imported backing are next.

## Rendering bugs

Bisect bbport's optimizations first: `BB_TOGGLE_FILE` holds a mask of switches (bits 0–19, 30,
32–50; re-read every 250 ms); run with all off, then halve groups with one fixed mask per run.
The user watches and says what they see. Example: the cutscene flashing was bit 42
(`WRITE_DATA` pipelined on the recording thread).

If a game run hangs in an indirect dispatch, first retry with `BB_INDIRECT_GUARD=0`:
Bloodborne-Android reported a hang after a few minutes with upstream's guard enabled on its
device. This has not been observed here.

The isolated new-save opening video faulted at the same null read (address `0xd2`) with both SRT
walkers: 2/5 translated and 3/5 portable runs on 2026-10-12. One run each way stopped at
`GPU [Debug] <Critical> vk_shader_hle.cpp:242 ExecuteCopyShaderHLE: Assertion Failed!`
instead. The faulting thread was main or GXRenderThread; command-buffer
probes in two runs each way saw those two users of the main buffer, no third movie-thread writer
and no overlapping buffer memory. Wider and narrower command-buffer guards did not remove the
fault, and the wider guard coincided with a stall. The portable walker is now the arm64 default
because the failure reproduces with the translated walker; `BB_SRT_INTERP=0` selects the old
one. The opening-video fault remains unresolved; a timing race is a hypothesis. A separate Central Yharnam baseline stopped at
`SurfaceFormat: Unknown data_format=15 and num_format=4`; watch for recurrence alongside the
earlier one-off `data_format=6 and num_format=1`. Do not record guest code or memory in docs or
commits.

## Upstream merges

bbport releases come as squashed commits; remote `upstream`
(https://github.com/deadinside28/bloodborne_pc). Before the first N4 renderer code slice, its
HEAD was checked on 2026-10-12 and still matched release 0.51. Once N4 code starts, merge
non-renderer changes (runtime, patches, scripts, game settings and launcher) as before. Review
changes in `renderer_vulkan/`, `buffer_cache/`, `texture_cache/` and `shader_recompiler/` for each
release and port useful ones by hand into the shared core or Vulkan backend; record ported,
skipped and deferred decisions in `docs/upstream/RENDERER_PORTS.md`. Check what assumes Linux or
native x86:

- no `MAP_FIXED_NOREPLACE`, `memfd_create`, `fallocate`, `gregs`/`REG_RIP` on macOS: use
  `bb_shared_memory`, `bb_shared_memory_zero`, `BB_UC_RIP`/`BB_UC_RSP`;
- the image is not at 0x800000000: use `BB_IMAGE_BASE`, and image offsets in game profiles;
- host code cannot run x86 stubs or call guest functions directly under bbcpu: such features stay
  int3 traps or are off on macOS (upstream's guest-hook detours, the game-menu pages);
- libc++ needs complete types where GCC does not.

Then build, run the game a few times (`tools/test_run.sh`), update `docs/CREDITS.md` ("merged up
to"), `docs/upstream/README*.md` and the renderer port log, and bump the submodule in
bloodborne-mac. On macOS the
"Auto" memory model picks the 0.3 one (non-AMD GPU).

## Working rules (all three repositories)

- **The project is three repositories**, nested as submodules:
  [bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac) (the app) ⊃ `runtime/` =
  [bloodborne-runtime](https://github.com/bloodborne-native/bloodborne-runtime) ⊃ `recomp/` =
  [bloodborne-recomp](https://github.com/bloodborne-native/bloodborne-recomp). Commit in the repository
  that owns the file, push it, then bump the submodule pointer in the parent (runtime → recomp,
  mac → runtime) and push that too. Submodules are checked out at a commit (detached HEAD):
  before editing inside `runtime/` or `runtime/recomp/`, run `git checkout main` (and
  `git pull`) there. Separate clones of each repository side by side work the same way.
- **Before any push, check `git remote get-url origin`** (a clone once still pointed at a local
  repository and a force-push landed there). Push to `main`; ask before force-pushing anything
  that existed before this session.
- **Commits:** a short subject, a body that says what changed and why (numbers when measured), and
  the trailer the session asks for (`Co-Authored-By: ...`).
- **Credits:** this work is built on [bbport](https://github.com/deadinside28/bloodborne_pc) by
  deadinside28 and on [shadPS4](https://github.com/shadps4-emu/shadPS4); keep them credited where
  they are (README, docs/CREDITS.md, the wikis). Code taken from another project or fork is
  credited in the same commit: CREDITS.md, the wiki's credits page, the commit message and a short
  code comment.
- **No game code, ever:** no decompiled or generated game code, no game files, no records of game
  memory in any repository. Docs and wikis talk about the recompiler only (not about decompiled
  code or private folders).
- **Testing the game:** run it in the background with `runtime/tools/test_run.sh NAME [VAR=...]`
  (bloodborne-runtime's `tools/test_run.sh`): it copies the player's save first and never touches
  the real one. Check `pgrep -x bb-probe` first; one game at a time. Test windows open on the
  user's screen: say so before long batches of runs (bisections), and stop when asked.
- **Never capture the user's screen.** Pictures come from the game itself: `tools/grab.sh NAME`
  (its frame) or `tools/grab.sh --screen NAME` (with the in-game menu). Screenshots in English
  (`BB_LANGUAGE=1 BB_UI_LANGUAGE=en`).
- **Shell:** no `rm -rf` on variable paths (literal paths only); no `xargs ... sh -c '...'` (write
  a script file instead).
- **Docs:** plain, precise English; numbers measured, not guessed. When a milestone or a measured
  number changes, update the docs and the wiki status pages (progress bars: 20 blocks, one per 5%).
