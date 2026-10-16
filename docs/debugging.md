# Debugging and tooling

## Two ways to run

| | Windows build under Proton | Linux headless harness |
|---|---|---|
| Build / run | `./run-proton.sh` (`--no-build`, `--build-only`, `--skip-fmv`, `--skippable-cutscenes`) | `./rebuild.sh --harness`, then `sdk/harness/run.sh` (below) |
| Renderer | D3D12 (vkd3d-proton), render thread | none, or the CPU reference renderer on chosen frames |
| Logs | `game/winrun/logs/game.log` | `<out>/<k>/run.out`, `<out>/<k>/logs/` |
| Good for | anything visual, timing, audio: **the ground truth** | fast, scriptable, gdb-able runs; crashes, traps, draw lists |

The harness differs from the real renderer in ways that matter: no render thread, every visibility test passes
(`640*480` pixels), 4:3 and 30 FPS unless `XBE_ASPECT` / `XBE_FRAME_RATE` say otherwise, and a separate CPU
renderer. Confirm visual fixes on Proton.

### Harness

```bash
cd game
bash sdk/harness/build.sh -r . -g generated/game -o out/harness -n conker
bash sdk/harness/run.sh -x out/harness/conker -g assets -o out/run/<fresh-folder> -t 60 [-s script]
```

A run folder keeps the title's saves (`hdd`) between runs, so use a new folder for a reproducible run instead of
deleting an old one. Pad scripts are lines of `frame buttons [frames held]` (frames are presented frames; buttons
`UP DOWN LEFT RIGHT START BACK A B X Y LT RT LS LSD LSL LSR`). To start a new game with the FMVs on: `12500 A` (skip
the intro movie), `13500 LEFT`, `14000 A`, `15000 A`. The movie start drifts a little, so if the A misses it the
intro plays out for 67 s; press A later or more than once. The throne cutscene starts around frame 37000-42000.
With `XBE_SKIP_FMV=1` the menu comes up in about 0.3 s.

## Keys (Windows build)

| Key | Does |
|---|---|
| F1 / Insert | Settings menu |
| F3 | With `XBE_CAPTURE_DIR` set, captures the next frame; logs every guest thread's stack (needs `XBE_VERBOSE` in a release build) |
| F5 | Writes `fnprof_NN.txt` in a profiler build (below) |
| F6 / F7 / F8 | `XBE_CAMSCAN` only: restart / log candidates / probe |

## The F3 frame capture

Run with `XBE_CAPTURE_DIR=cap` and press F3. The capture lists every draw in order with its targets, vertex inputs
(stream, type, stride, first vertices), program hash and constants (on first use), pixel shader key and generated
HLSL, stage samplers and texture modes, fog and final combiner state, stencil state, point-sprite state, clears, and
`Backend::Note()` lines such as who turned the stencil test on. `XBE_CAPTURE_FRAMES=N` captures N frames per press
(for effects that alternate); `XBE_CAPTURE_VS=<handle>` / `XBE_CAPTURE_RT=<surface>` save targets around matching
draws. Comparing a capture of "looks right" with "looks wrong" has found most rendering bugs (grass, glow, flag).

## Environment variables

Gameplay and options (both builds):

| Variable | Effect |
|---|---|
| `XBE_SKIP_FMV` | Movies report failure and are skipped |
| `XBE_SKIPPABLE_CUTSCENES` | LT skips any in-engine cutscene |
| `XBE_GRASS_WIND=0` | Distant grass stands still (console behaviour) |
| `XBE_FRAME_RATE=30\|60\|120` | Overrides `frame_rate` |
| `XBE_ASPECT=W:H` | Overrides the frame aspect |
| `XBE_CAMSCAN=1` | Camera-angle search (`camscan.cpp`) |

Logging and diagnostics (runtime):

| Variable | Effect |
|---|---|
| `XBE_VERBOSE` | Info/debug log lines in a release build (otherwise only `LOGR`/`LOGE`) |
| `XBE_PERF` | Perf lines once a second; names read-backs |
| `XBE_FRAME_LOG` | Per frame: draws, opaque draws, counted draws, pending reports |
| `XBE_CRASH_TRAIL` | Each thread keeps its last indirect/kernel/host calls for the crash report |
| `XBE_D3D12_DEBUG` | D3D12 debug layer |
| `XBE_CAPTURE_DIR`, `XBE_CAPTURE_FRAMES`, `XBE_CAPTURE_VS`, `XBE_CAPTURE_RT` | F3 capture (above) |
| `XBE_DUMP_METHODS` | Raw NV2A methods before each draw, every method's first value, draw callers |
| `XBE_DUMP_PROGRAM` | Each distinct chip vertex program (`tools/vsh_disasm.py run.out [hash]` disassembles them) |
| `XBE_TRACE_WALK` | `Walk()` bails, stops, resyncs, inline texture headers |
| `XBE_NO_WALK_JUMPS` | Old behaviour: stop the walk at the first jump (for comparison) |
| `XBE_TRACE_REPORTS` | Visibility tests: zero results, flash signatures, skipped counted draws |
| `XBE_REPORTS_VISIBLE` | Every visibility test passes |
| `XBE_TRACE_TICKS` | Each frame's microseconds and ticks |
| `XBE_TRACE_DSOUND` (`=all`) | Voice calls, packets with their `.xwb`, per-voice RMS, 3D descriptors (`all`: volume/mix too) |
| `XBE_TRACE_VOICE=<obj>` | One stream's status polls |

Harness only:

| Variable | Effect |
|---|---|
| `SCRIPT=<file>` | Pad script; a `cmd` file in the run folder presses buttons once when read |
| `REF=N` | Every Nth frame drawn by the reference renderer to `cap/ref_<frame>.tga`; a `snap` file draws the next two |
| `CAP=N` | Every Nth frame's draw list to `cap/draws_<frame>.txt` |
| `DUMP_TEX=1\|2` | Stage textures of drawn frames as TGA (`2`: every stage) |
| `DUMP_RT` | Render targets of drawn frames |
| `DUMP_PS=<handle>` | A pixel shader's translated combiner |
| `DUMP_VS=<handle>` | A vertex shader draw's outputs |
| `PROBES="x,y …"`, `PROBE_WIDTH`, `TRACE="x,y"` | Every colour written at those pixels; combiner registers there |
| `TRACE_PROG=<hash>`, `TRACE_VS=<handle>`, `TRACE_BLENDED` | Combiner traces of draws running a program / shader, wherever they land |
| `LIST_CONSTS` | Every non-zero vertex constant in the draw list |
| `REPORT_LAG=N`, `REPORT_ZERO`, `LOG_REPORTS` | Answer visibility tests N frames late / all failing / log them |
| `STALL_TRAP=<ms>` | Watchdog: trap when a frame stalls (use under gdb) |
| `TEST_OPAQUE` | Draw `DST_COLOR/ZERO` blends opaque |
| `INSTANCE`, `-n` | Several instances for system link on `127.0.0.k` |

## Other tools

- **Function profiler.** Build the generated code with `-DXBE_FN_PROFILE` in a separate build directory (e.g. a copy
  of `out/build/win`'s cache with it added to `CMAKE_CXX_FLAGS`), swap the exe in as `winrun/Game.exe` and launch
  with `--no-build`. Each recompiled function counts its calls, split by whether pad 0's left trigger is held; F5
  writes `fnprof_NN.txt`. Hold LT during the thing you want to find. This is how the cutscene-skip test was found.
- **Crash reports.** The log names the faulting guest function (`sub_XXXXXXXX`, in `generated/game/x86_recomp.*.cpp`),
  walks the host stack and dumps every guest thread; host addresses are `Game.exe+RVA [VA]` for
  `llvm-addr2line -fiC -e Game.exe VA`. A minidump goes to `CrashDumps/`.
- **`[xbe] trace = [...]`** in `game.toml` logs every call of those functions with caller, arguments and result.
- **`tools/xbe_disasm.py <va> [len] [count]`**: disassemble the XBE by virtual address (ndisasm).
- **`tools/vsh_disasm.py run.out [hash]`**: disassemble `XBE_DUMP_PROGRAM` output.
- **`tools/xbe_d3d_internals.py iso/default.xbe game/config/game.toml`**: D3D internals `.text` calls that the config
  doesn't name.
- **`game/sdk/tools/symbols/xbe2pe.py config/game.toml game.exe`**: a PE for Ghidra.
- **xemu** (`xemu/` in the workspace, not in git) is the hardware reference: `hw/xbox/nv2a/` for the GPU
  (`pgraph`, `pfifo`, `nv2a_regs.h`), and its shader generators for fog and combiner semantics.
