# Architecture

How the port is put together: what each layer does, where data flows, and the rules the layers rely on. For
step-by-step recipes see [making-changes.md](making-changes.md); for Conker's own addresses and facts see
[conker.md](conker.md).

## The pieces

```
iso/default.xbe ──xbe_symbols──▶ game/config/game.toml  (written once, then hand-maintained)
        │                                 │
        └────────────xbe_recomp───────────┘
                          ▼
         game/generated/game/*.cpp   one C++ function per guest x86 function (gitignored, ~227 MB)
                          +
         game/sdk/runtime/           Xbox kernel, XDK libraries, D3D12 renderer, audio, host app
                          +
         game/src/                   Conker: function replacements, patches, input, F1 menu
                          ▼
         Game.exe (Windows, D3D12; run here under Proton)   or   the headless harness (Linux, no GPU)
```

| Path | What it is |
|---|---|
| `game/sdk/` | The SDK, [Xbox XBE Recomp](https://github.com/TGP482/XboxXBERecomp), vendored as a git subtree. Title-agnostic. |
| `game/sdk/tools/recomp/` | `xbe_recomp`: x86 → C++ recompiler (analysis + codegen). |
| `game/sdk/tools/symbols/` | `xbe_symbols`: XDK signature scan that writes a config; `xbe2pe.py` for Ghidra. |
| `game/sdk/runtime/` | Everything the recompiled code links against (see [Execution model](#execution-model)). |
| `game/sdk/xdk/5849/` | XDK 1.0.5849 signatures and struct layouts (`layout.toml`). |
| `game/sdk/harness/` | Linux headless build: null/reference renderer, pad scripts. |
| `game/config/game.toml` | The recompiler's input for Conker: symbols, data globals, argument maps, hooks. |
| `game/src/common/` | `game_info.cpp` (`app::kGame`, the game's description to the runtime) and headers. |
| `game/src/game/` | Conker-specific code: `hooks.cpp`, `display.cpp`, `keyboard.cpp`, `mouse.cpp`, `ui.cpp`, `camscan.cpp`. |
| `rebuild.sh`, `run-proton.sh` | Rebuild from the ISO; build the Windows exe and launch it under Proton. |
| `tools/` | Conker-specific Python helpers (XBE disassembly, vertex program disassembly). |

## Build pipeline

1. **`xbe_symbols`** reads the XBE's library versions, matches `xdk/<build>/signatures.toml` and writes
   `config/game.toml`: `[xbe]`, `[symbols]` (XDK functions the runtime replaces or calls), `[data]` (XDK globals),
   `[abi]` placeholders. It **overwrites** the file. Conker's config has since been corrected and extended by hand,
   so it is never regenerated (`rebuild.sh` only writes it when absent; `--fresh-config` drops the hand edits).
2. **`xbe_recomp`** finds every function (direct calls, code pointers, gaps between known functions, the extra
   entries in `[functions]`), resolves switch tables, and writes:
   - `x86_recomp.<n>.cpp` × `partitions` (128): the functions, split for parallel compilation;
   - `x86_func_mapping.cpp`: guest address → host function, for indirect calls;
   - `image_symbols.h`: `SYM_<name>` addresses, `HAS_<name>` flags and the `IMAGE_ABI` argument maps from the config;
   - `x86_recomp_shared.h`: declarations, including the `[[hook]]` functions the game must define.

   Only files whose content changed are rewritten, so a config edit recompiles little.
3. **`add_xbe_image`** (`sdk/cmake/XbeRecomp.cmake`) reruns codegen when the config, the XDK tables or the XBE
   change. It builds `<target>_host`, a static library of everything but the title's code (the runtime,
   `src/common`, `src/game`, ImGui, the XBE's SHA-1), and links the exe from the generated objects plus that library
   as a **whole archive**. Without that, the linker would never pull in a replacement whose recompiled weak twin
   already resolved the symbol. At launch the exe picks the copy in `assets` (folder or ISO) with that exact XBE,
   and only offers another release when none matches.
4. **Windows cross build.** Codegen runs a *native* `xbe_recomp` (`XBE_CODEGEN_TOOL`) while the game is compiled
   with the pinned llvm-mingw (`game/cmake-llvm-mingw.cmake`, fetched by `tools/fetch-toolchain.sh`).
   `run-proton.sh` builds in `game/out/build/llvm-mingw` and runs the result under GE-Proton.
5. **Player kit.** Releases don't contain `Game.exe`. `add_xbe_player_kit` stages `Setup.exe` with the host
   library and the recompiler; Setup downloads the pinned compiler, and players build the exe themselves. See
   [distribution.md](distribution.md).

## Execution model

**Guest memory.** The whole 32-bit Xbox address space is one host reservation; a guest address plus `base`
(`kernel::g_base`, `mem::Base()`) is a host pointer. `kernel::R32/W32/HostPtr` read and write it. The layout follows
the console (`runtime/kernel/memory.h`): contiguous memory at `0x80000000 | physical`, the system pool at
`0xB0000000`, thread stacks at `0xD0000000`. The NV2A (`0xFD000000`) and APU (`0xFE800000`) register windows are
backed by plain memory, because Conker's inlined library code reads and writes them directly. **Physical addresses**
(push-buffer jump targets, texture offsets the engine writes raw) are read through the `0x80000000` view.

**Guest code.** Each guest function becomes
`void sub_XXXXXXXX(X86Context& ctx, uint8_t* base)`. Registers live in `ctx`; flags are locals; the x87 stack is
tracked at run time. Every recompiled function is emitted as `__imp__sub_XXXXXXXX` (the body) plus a **weak**
`sub_XXXXXXXX` that calls it. Any strong `X86_FUNC(sub_XXXXXXXX)` elsewhere (runtime or game) replaces it at link
time, and can still run the original through `__imp__sub_XXXXXXXX`. lld keeps this override behaviour.

**Calls across the boundary.**
- Guest → guest indirect: `x86_icall` looks the target up in the mapping table (`runtime/cpu/dispatch.cpp`).
- Guest → host: a replaced function (above), a kernel export (`x86_kernel` by ordinal), or a `[[hook]]` call
  placed before/after a specific instruction.
- Host → guest: `kernel::CallGuest(ctx, va, {args})`, or `GUEST_CALL(Name, ctx, {args})` for a function named in
  the config (it honours `[abi]` register maps).

**Kernel** (`runtime/kernel/`). `xboxkrnl.cpp` implements exports by ordinal as `stdcall`; the declared argument
count is what the callee pops, so it must match every call site. Guest threads are host threads, each with its own
`X86Context`. Waitable objects, the dispatcher, timers and DPCs are host-side (`objects.cpp`, `threads.cpp`). Files
come from the `assets` folder or a disc image (`disc.h`, mounted as `D:`); the hard disk partitions live in the run
folder. Reads on handles opened without `FILE_SYNCHRONOUS_IO_*` complete asynchronously (~1 ms later, or when the
thread waits), because XACT depends on a read still being in flight.

**XDK libraries** (`runtime/xdk/`). A library function named in `[symbols]` can be replaced on the host with
`HLE_STDCALL(name, nargs)` / `HLE_FASTCALL` / `HLE_CDECL`, or observed with `HLE_WRAP` (runs before the original).
`a[i]` reads API argument *i*, wherever `[abi]` says the LTCG build put it. Struct offsets come from
`xdk/5849/layout.toml` (`XDK_CDevice_*`), globals from `[data]`; a value the config lacks traps where it is first
needed rather than being guessed. What stays recompiled: resource creation and locking, XG helpers, XACT, and the
D3D internals the engine calls directly (see the graphics section).

## Graphics

This is where most of the port's complexity sits. Read this section before touching `d3d8.cpp` or `d3d12.cpp`.

### Two writers, one GPU

Conker's D3D8 library is **LTCG**: much of it is inlined into the engine. So the engine talks to the GPU two ways:

1. **D3D API calls** the runtime replaces (`SetTexture`, `SetVertexShader`, `DrawVertices`, …). These are applied
   to `gpu::State` immediately.
2. **Raw NV2A commands** pushed into the push buffer: inlined library code, hand-written engine emitters, and
   static push buffers loaded with the models (`.rbm`), chained together with jumps.

The runtime hands the title a scratch push buffer (`ScratchPush`) and reads it **lazily** with `d3d::Walk()`. On the
console the GPU consumes both in order; here they are reconciled by two rules.

**Rule 1 — order.** Every HLE that changes state or draws first walks whatever the title pushed before it
(`Walk(Dev(kDevPush))`). Otherwise a raw command pushed between two API calls would run with the state of the later
call. The engine also patches push buffers it already queued (`RunPushBuffer` fixups); the game hook on `0x53AD90`
calls `d3d::FlushPending()` so the earlier run is read before the patch lands.

**Rule 2 — last writer wins**, per register group, exactly like the chip:

| State | Raw write takes it | D3D takes it back |
|---|---|---|
| Vertex program | `SET_TRANSFORM_PROGRAM` upload/start (`g_chip_program`) | `SetVertexShader` |
| Pixel combiner | any raw combiner register (`RawCombiner()` object, handle `0xFFFFFFFF`) | `SetPixelShader`; fixed-function flush with no PS (dirty `0x800`) |
| Fog unit | `FOG_ENABLE/MODE/PARAMS` (`State::chip_fog`) | the library's fog flush (dirty `0x2000`, modelled by `PushFog`) |
| Texture factor | `COMBINER_FACTOR0` (`State::chip_tfactor`) | `SetRenderState(TEXTUREFACTOR)`, a pixel shader definition |
| Specular enable | `SET_SPECULAR_ENABLE` | the lighting flush (dirty `0x1000`) |
| Vertex attribute layout | `VERTEX_DATA_ARRAY_FORMAT/OFFSET` (`Stream::fmt`) | D3D draw with dirty vertex format / streams (`DeclarationBinds`) |
| Sampler state | `SET_TEXTURE_*` (written into the title's own deferred stage table) | inlined `SetTextureStageState` writing the same table |
| Cull, stencil enable/fail | raw registers (`ChipCull`, `kRs*` mirrors) | the D3D setters, which mirror into the raw registers |

The library's deferred-state flush (`0x543170`) is replaced by a game hook that calls `d3d::DeferredFlush(dirty)`;
the HLE'd draws run the same decision through `FlushForDraw()`. Those two functions are the only place where the
dirty flags decide which writer wins.

**HLE'd bodies don't run, so their side effects must be modelled.** On the console, a D3D draw's library body
pushes the declaration's vertex formats, `CreateDevice` points device fields at records, and `SetVertexShader`
points the device at an FVF shader object. Each of these is reproduced explicitly (`DeclarationBinds`,
`DeviceRecords`, `D3D_FvfShader`, `ApplyDefaultStates`). A new HLE needs the same review.

### `Walk()` — the push-buffer decoder

`Walk()` (in `runtime/xdk/d3d8.cpp`) follows xemu's `pfifo` semantics: jumps, old jumps, calls (16-deep) and returns,
with physical targets read through `0x80000000`. A jump back to the put pointer ends the walk; a chain that leaves
its block without returning resumes at the word after the last jump site in the scratch buffer (`home`). A 4M-packet
guard bounds it. Methods decoded:

- **Primitives:** `BEGIN_END` (`0x17FC`) around inline vertex data (`0x1500..`, `0x1518..`, `0x1880..0x1B00`
  including the 2S/4S_M short forms), the inline array (`0x1818`), `DRAW_ARRAYS` (`0x1810`, runs joined like xemu),
  and `ARRAY_ELEMENT16/32` (`0x1800/0x1808`) for indexed passes.
- **Vertex programs and constants:** `0xB00..0xB7C` + `0x1E9C` load + `0x1EA0` start into 136-slot program memory;
  `0xB80..0xBFC` + `0x1EA4` into `vsc[]`. Both load pointers auto-advance across packets.
- **Vertex streams:** `0x1720..` offsets and `0x1760..` formats into synthetic vertex-buffer headers and
  `Stream::fmt`. A format with **count 0** means "no array" (a constant), not an empty stream.
- **Textures:** `0x1B00..0x1BFC`, 0x40 per stage; offset+format become a `D3DPixelContainer` (`InlineTexture`), the
  sampler registers go through `RawTextureState`.
- **Visibility tests:** `0x17C8..0x17D0` to `Backend::ClearReport/GetReport`.
- **Everything else** lands in `State::simple[method >> 2]`, the NV2A register shadow the renderers read by method
  number, with side effects for the registers in the table above.

A method that falls through to `simple[]` is stored silently. A dropped *vertex* method loses whole draws without a
log line, so new method ranges should be checked against xemu's `DEF_METHOD` lists (`xemu/hw/xbox/nv2a/`).

### `gpu::State`

`runtime/gpu/state.h` is the contract between the D3D layer and the renderers: `simple[]` (raw registers by
method/4), `rs[]` (D3DRS deferred/complex states), `tss[4][32]`, `textures[]` (guest texture headers), `streams[]`,
the bound `vs`/`ps` objects, `vsc[192]` in hardware order, transforms, lights, viewport, render and depth targets,
and the `chip_*` ownership flags. Renderers only read it.

### Vertex layout

Two sources, chosen per draw: an HLE'd draw reads its vertices through the bound shader's **declaration** (the
inline formats are hidden for it, as the library body would have replaced them); a raw draw reads them through the
**inline format binds** (slot N is input register N). Mixing them up produces collapsed or scattered geometry.

### The D3D12 backend

`gpu::Backend` (`backend.h`) is the renderer interface: `Clear`, `Draw`, `Copy`, `Present`, `Sync`, reports,
fences. On Windows the `D3D12Backend` (`d3d12.cpp`) runs behind `Threaded` (`render_thread.cpp`): the title's thread
records each call with what the console takes at call time (state, headers, indices) and continues; vertex, texture
and surface memory are read when the draw runs, as the console GPU would. The XDK's fences, busy checks and locks
wait on the render thread.

Per draw (`D3D12Backend::Draw`): assemble the vertex input (`vertex.cpp`) → bind the render/depth targets → build
the vertex shader (NV2A program → HLSL, or fixed function, `shaders.cpp`) → bind stage textures (`texture.cpp`
decodes, unswizzles and caches) → build the pixel shader from a `PixelKey` (register combiners or fixed-function
stages) → pipeline from a `PsoKey` → constants → draw, counted when a visibility test is open.

**Guest surfaces are tracked by data address.** A render target (`Target`) is a host resource at the render scale
(and optionally multisampled, with a single-sampled `res` beside the `ms` copy). When the CPU locks a surface the GPU
drew, `Sync` reads it back into guest memory; when the CPU writes one, a memory signature (`LatchCpuWrite`) notices
and the target is re-uploaded. A draw that samples the target it renders to gets a copy (`Feedback`); a depth buffer
sampled as colour is converted (`DepthBytes`). `Present` scales the front buffer into the window, letterboxed to
`app::FrameAspect()`, then draws the ImGui overlay.

**Shaders are generated, compiled with FXC and cached**, and every shader/pipeline is recorded to `shaders.rec`
(inputs only, shareable) plus `shader_programs.bin` (the title's vertex programs, local) beside the exe. The next
launch compiles them in the background (`shader_record.{h,cpp}`, `StartWarmUp`). If a record's meaning changes
without its size changing, bump `kRecordRevision`.

The renderers tell shaders apart **by object pointer**, so any synthesized `VertexShader`/`PixelShader` (chip
programs, immediate-mode layouts, the raw combiner) is cached per key and kept alive.

`gpu::Extension` (`ext.h`) lets a game add passes or shader code; Conker does not use it.

On the harness, `null_backend.cpp` lists draws and answers every visibility test as "visible", and
`reference_renderer.cpp` draws chosen frames on the CPU.

## Audio

`runtime/xdk/dsound.cpp` implements DirectSound as **host objects** (a `Voice` per buffer/stream) mixed in
software and played through XAudio2; the guest objects it hands out are small zeroed blocks. XACT itself stays
recompiled and calls into the DirectSound library. Library bodies that need a real object are HLE'd; bodies that only
write console voice or DSP registers are skipped by game hooks (`hooks.cpp`). XACT's clock is the APU sample counter
at `0xFE80200C`, driven from the host clock. 3D sound runs the library's own calculator (the game hooks install the
HRTF function table it needs) and passes the result to `dsound::SetVoice3d`.

Two shapes to recognise when a new DirectSound path traps: `this` adjusted to the console voice (`this-0x1c` for a
buffer, `this+4` for a stream), and calls through the DSP function table at `0x4E1D1C..0x4E1D44`.

## Host application

- **Threads:** the window thread (`app/main.cpp`, message loop, ImGui input), the guest threads (the title's main
  thread runs the game loop), the render thread, the vblank thread (`d3d8.cpp`, 60 Hz count/event/callback), the
  audio mixer, and shader warm-up workers.
- **`app::kGame`** (`game/src/common/game_info.cpp`) is everything the runtime asks of the game: names, the
  settings file, options, and hooks (`load`/`save` for extra settings tables, `message`, `ui`, `pad`, `aspect`, …).
- **Settings** (`settings.toml` beside the exe): the runtime's own keys (`display_mode`, `width`, `height`,
  `render_percent`, `fps_cap`, `vsync`, `anisotropic_filtering`, `msaa`, `shader_warmup`, `aspect`, `join_ip`), the
  game's `options` (`fps_overlay`, `fps_in_title`, `frame_rate`, `skippable_cutscenes`, `grass_wind`, `mouse_look`,
  `mouse_sensitivity`, `mouse_sensitivity_y`, `mouse_invert_y`) and the `[keys]` table (`keyboard.cpp`).
- **Input:** SDL3 pads become console controller reports (`core/input.cpp`, `xdk/xapi.cpp`). `GameInfo::pad`
  (`keyboard::Pad`) then merges the keyboard and, through `mouse::Pad`, mouse look into port 0 as stick/button input.
- **F1 menu** (`ui.cpp`): ImGui, drawn on the render thread under `app::UiLock()`; tabs Display, Graphics, Gameplay,
  Controls. Options take effect live because the hooks read their globals on every call. `Draw` must never block on
  another thread; it posts messages to the window thread instead.
- **Crashes:** `app/crash.cpp` logs the faulting guest function, the host stack and every guest thread, and writes a
  minidump to `CrashDumps/`.
