# Conker: Live & Reloaded — static recompilation handoff

Unofficial native port of the original-Xbox *Conker: Live & Reloaded* built with
[Xbox XBE Recomp](../../) (this workspace's `XboxXBERecomp/`, the SDK). The game's x86 `.xbe` is translated
to C++ ahead of time; the Xbox kernel and XDK libraries are implemented at the API level and drawn with a
Direct3D 12 renderer. This document is the state of the port at handoff.

> This repository contains no game assets. The ISO / extracted files are the user's own.

## Status (TL;DR)

The game **statically recompiles and boots** on both the Linux headless harness and a **Windows build run
under Proton**:

- kernel init, disc mount (`D:`), XOnline/controller init
- `Direct3D_CreateDevice` (×2), 640×480, ~50 vertex shaders created, per-frame D3D through the runtime
- volume queries (`IoQueryVolumeInformation`), XNet, DirectSound + XACT/audio, the frontend level
  (`Level\Frontend`), fonts, text and package (`*.rba`/`*.rbm`/`*.xwb`) loads all run
- reaches ~0.26 s of guest execution, then **crashes** inside a DirectSound library method that reads a
  field the runtime's host-side DSound object does not have (see [Current blocker](#current-blocker)).

Pipeline: 21,956 functions / 1.47 M instructions recompiled → 227 MB of C++ → `Game.exe`.

## Progress since the previous handoff

The previous state stopped in engine init writing guest `0x18` from `0x62A60`. That was a **kernel
stack-argument-count bug** (`IoQueryVolumeInformation` popped 4 args while the engine's CRT pushes 5),
which corrupted the caller's frame and only looked like an engine/subsystem bug. Fixing it, and then six
later issues, carried the port from that bogus write deep into the frontend and audio setup:

| # | Fix | Where |
|---|-----|-------|
| 1 | `IoQueryVolumeInformation` pops 5 args, not 4 (the CRT's `_findfirst` pushes `Device, Class, Length, Buffer, IoStatus`) | `runtime/kernel/xboxkrnl.cpp` |
| 2 | `D3DDevice_DrawVerticesUP = 0x53A600` (was unmapped; the engine calls the library internal directly, which read uninitialised device state) | `config/game.toml` |
| 3 | `D3D_FvfShader = 0x547040` so `CreateDevice` points `device->vs` at the library's FVF shader object; ~68 D3D internals guard on it | `config/game.toml` |
| 4 | back the MCPX APU register window `0xFE800000` with scratch; XACT polls `0xFE80200C` | `runtime/kernel/memory.{h,cpp}` |
| 5 | `0x4C67B9` (a second `CreateSoundBuffer` thunk the signature scan missed) routes to the runtime's `IDirectSound_CreateSoundBuffer` HLE | `src/game/hooks.cpp` |
| 6 | `0x4C3F2F`, a DSound buffer flag helper that touches a sub-object the runtime's objects lack, becomes a no-op | `src/game/hooks.cpp` |
| 7 | `NewBuffer` ignores a descriptor whose `dwSize != 0x18` (the engine's XACT path passes one) so it cannot request a multi-GB block | `runtime/xdk/dsound.cpp` |

Each fix was verified on the headless harness; the crash point moved forward every time. The remaining
work is the DSound object layout (below).

## Layout

```
conker-lar/
  Conker Live and Reloaded.iso     the user's disc image (gitignored via iso/)
  iso/                             extracted disc (default.xbe, dvddata/, media/)
  umu-run                          umu-launcher 1.4.1, runs the Windows build under Proton
  XboxXBERecomp/                   the SDK (pristine upstream + patches/, see below)
  clr_unpack/  xemu/               provided references (unused so far)
  REVERSE_ENGINEERING.md           forum notes for Conker internals
  tools/xbe_disasm.py              disassemble an XBE range by VA (ndisasm)
  tools/xbe_d3d_internals.py       list D3D internals .text calls directly
  notes/d3d_internals_called_from_text.txt
  patches/xboxxberecomp.patch      changes made to the SDK (apply to a fresh checkout)
  game/                            the game repo (the port)
    sdk -> ../XboxXBERecomp        symlink the template expects
    assets/ -> iso symlinks        default.xbe, dvddata, media
    config/game.toml               recompiler config ([abi]/[data]/hooks)
    src/game/hooks.cpp             host replacements for D3D internals
    src/common/game_info.cpp       title / window / exe name
    cmake-mingw.cmake              Windows cross toolchain
    third_party/mingw/lib/         libxaudio2.a -> libxaudio2_8.a link shim
    generated/game/                227 MB of recompiled C++ (gitignored)
    out/                           builds (gitignored)
    prefix/                        Proton prefix (gitignored)
    winrun/                        run folder for the exe (gitignored)
```

## Prerequisites

- `extract-xiso`, `clang`/`clang++` 23, `cmake` 4.4, `ninja`, `ndisasm`
- `mingw-w64` (x86_64, posix threads), `llvm` (`ld.lld`, `llvm-ar`)
- For testing the Windows exe: **GE-Proton11-7** and **umu-run 1.4.1**
- A GitHub clone of the SDK is `XboxXBERecomp/`; if you re-clone it, apply `patches/xboxxberecomp.patch`
  (`git -C XboxXBERecomp apply ../patches/xboxxberecomp.patch`).

## Reproduce from scratch

### 1. Extract the disc
```bash
cd /home/giacomo/Projects/conker-lar
extract-xiso -x "Conker Live and Reloaded.iso" -d iso
```

### 2. Create the game repo
```bash
cp -r XboxXBERecomp/templates/game game
ln -s ../XboxXBERecomp game/sdk
mkdir -p game/assets
ln -s ../../iso/default.xbe game/assets/default.xbe
ln -s ../../iso/dvddata     game/assets/dvddata
ln -s ../../iso/media       game/assets/media
```

### 3. Build the tools (native Linux)
```bash
cd game
cmake -S . -B out/build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS="-march=x86-64-v2" -DCMAKE_CXX_FLAGS="-march=x86-64-v2"
cmake --build out/build/release --target xbe_symbols xbe_recomp -j 16
```

### 4. Write the config
```bash
out/build/release/xbe_symbols assets/default.xbe sdk/xdk --out config/game.toml
```
This finds **122 of 238** XDK functions and **6 of 12** data symbols. Conker's D3D8 is LTCG and many
internals are only matched by constants; `xbe_symbols` writes `[abi]` placeholders (`"?"`) that must be
filled by hand — see [Config](#config).

### 5. Recompile the XBE
```bash
out/build/release/xbe_recomp config/game.toml --xdk sdk/xdk     # -> generated/game
```

### 6a. Test on Linux (headless harness)
```bash
bash sdk/harness/build.sh -r . -g generated/game -o out/harness -n conker
bash sdk/harness/run.sh   -x out/harness/conker -g assets -o out/run -t 20
# logs: out/run/1/run.out and out/run/1/logs/
```

### 6b. Build the Windows exe and run under Proton
```bash
# link-name shim: the runtime links "xaudio2", mingw ships libxaudio2_8.a
mkdir -p third_party/mingw/lib
ln -sf /usr/x86_64-w64-mingw32/lib/libxaudio2_8.a third_party/mingw/lib/libxaudio2.a

cmake -S . -B out/build/win -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake-mingw.cmake \
  -DCMAKE_C_FLAGS="-march=x86-64-v2" -DCMAKE_CXX_FLAGS="-march=x86-64-v2" \
  -DCMAKE_EXE_LINKER_FLAGS="-L$PWD/third_party/mingw/lib -municode -static -lwinpthread" \
  -DXBE_CODEGEN_TOOL=$PWD/out/build/release/xbe_recomp
cmake --build out/build/win --target Game -j 16

mkdir -p winrun && cp out/build/win/Game.exe winrun/ && ln -sf ../assets winrun/assets
cd winrun
WINEPREFIX=$PWD/../prefix \
PROTONPATH=~/.local/share/Steam/compatibilitytools.d/GE-Proton11-7 \
GAMEID=conker-lar \
  ../../umu-run "$PWD/Game.exe"          # umu-run 1.4.1, provided at the repo root
# logs: winrun/logs/game.log
```
`-municode` is needed for the `wWinMain` entry; `-static` links the mingw runtimes so the exe imports only
system DLLs plus `XAudio2_8.dll`. The exe finds `assets/` by walking up from its own directory, so it can run
straight from `winrun/`.

## Config (`game/config/game.toml`)

### `[abi]` — register/stack argument locations (proven by hand)
Conker's D3D8LTCG passes arguments in registers, unlike the reference titles, so each differs from the
signature and must be mapped. Proven with `tools/xbe_disasm.py`:

```toml
CDevice_KickOff = ["edx"]
D3DDevice_DrawVertices = ["stack", "ecx", "eax"]
D3DDevice_GetVertexShaderFunction = ["edx", "eax", "ecx"]
D3DDevice_SelectVertexShader = ["eax", "ebx"]
D3DDevice_SetIndices = ["ebx", "stack"]
D3DDevice_SetPixelShader = ["eax"]
D3DDevice_SetPixelShaderConstant = ["ecx", "stack", "eax"]
D3DDevice_SetShaderConstantMode = ["eax"]
D3DDevice_SetStreamSource = ["eax", "ebx", "stack"]
D3DDevice_SetTexture = ["eax", "stack"]
D3DDevice_SetTextureState_TexCoordIndex = ["esi", "stack"]
D3DDevice_SetTransform = ["eax", "edx"]
D3DDevice_Swap = ["eax"]
D3DResource_IsBusy = ["ecx"]
D3D_InternalRelease = ["esi"]
D3D_MakeRequestedSpace = ["eax", "stack"]
Direct3D_CreateDevice = ["ebx", "edx", "esi", "eax", "stack", "ecx"]
```

### `[data]` — D3D globals found in Conker's XBE
```toml
D3D_Device                  = 0x547540
D3D_pDevice                 = 0x5499E8
D3D_DirtyFlags              = 0x5499F0
D3D_DeferredTextureState    = 0x5473A0   # stage*0x80 + TexCoordIndex field
D3D_IndexData               = 0x54A898
D3D_RenderState             = 0x5499F8   # rep movsd 0x39 dwords in SetPixelShader
D3D_Streams                 = 0x547160   # [n*12]
D3D_SimpleRenderStateEncode = 0x5499F8   # this SDK build never reads it; only needs to be non-zero
D3D_FvfShader               = 0x547040   # CDevice_VertexShader (device+0x794); see progress table #3
```

Additional `[symbols]` hand-mappings (the signature scan misses some Conker internals):
```toml
D3DDevice_DrawVerticesUP    = 0x53A600   # writes NV2A method 0x17FC (primitive) + 0x1818 (inline array)
```

`D3D_DeviceInit` (0x53E5D0) is present but **commented out**: running the library's own device init performs
NV2A hardware bring-up (register handshakes, `wbinvd`, a FIFO drain at `0x53ED30`) the runtime cannot answer.
The internals that need it are host-replaced instead.

## Reverse-engineering findings

- **XBE**: title `0x4D530051`, entry `0x0042F0E9`, 27 sections, all 13 libraries at **XDK 1.0.5849** —
  the build the SDK ships `xdk/5849/signatures.toml` for (made from *Splinter Cell: Double Agent*).
- **D3D8 is D3D8LTCG**: the engine calls **library internals directly** (and emits raw NV2A pushbuffers via
  `0x537990`, an 8-byte "append (method,value)" primitive). The runtime already decodes those pushbuffer
  writes in `d3d8.cpp`'s `Walk()`/`g_state.simple[method>>2]`, so they need no replacement.
  `notes/d3d_internals_called_from_text.txt` lists **78** D3D functions `.text` calls that are not in
  `[symbols]` and are hook candidates (`tools/xbe_d3d_internals.py` regenerates it).
- **Miniport**: `[device+0x1C28]` (read as a pointer by `0x54085F`/`0x540B13`) is only ever set by the
  library's hardware init, never by the runtime — hence host-replacing the frame paths that need it.
- **Host replacements** (`src/game/hooks.cpp`):
  - `0x538D50` (per-frame buffer clear): `SetRenderTarget(backbuffer, autoDepth)` + `Clear(target|depth,0,z=1)`
    through `GUEST_CALL(D3DDevice_*)`.
  - `0x543170` (library deferred state flush): no-op that clears the handled dirty bits — the runtime keeps
    its own state and decodes the engine's pushbuffer writes.
  - `0x4C67B9` (second `CreateSoundBuffer` thunk the SDK scan missed): forwards to
    `GUEST_CALL(IDirectSound_CreateSoundBuffer, …)` so the library's console object builder does not run.
  - `0x4C3F2F` (DSound buffer flag helper touching a sub-object the runtime's objects lack): no-op.
- **MMIO**: the NV2A (`0xFD000000`) and MCPX APU (`0xFE800000`) register windows are backed by scratch in
  `runtime/kernel/memory.cpp`; Conker's D3D and XACT code read/write them directly.
- **Kernel export 76** `IoQueryVolumeInformation` implemented in `runtime/kernel/xboxkrnl.cpp` (size/device
  class for the mounted volumes).

## Changes to the provided tools (`patches/xboxxberecomp.patch`)

- `cmake/XbeRecomp.cmake` — adds `XBE_CODEGEN_TOOL` so codegen can run with a **native** `xbe_recomp` while
  cross-compiling the game for Windows.
- `runtime/kernel/memory.cpp` / `memory.h` — Linux build needs `<unistd.h>`; backs the NV2A register window
  `0xFD000000` **and** the MCPX APU window `0xFE800000` with scratch on both platforms (Conker's library
  and XACT audio code poke them directly).
- `runtime/gpu/shaders.cpp` — replaces the `1.#INF` / `-1.#INF` HLSL literals with
  `asfloat(0x7F800000u)` / `asfloat(0xFF800000u)`; vkd3d-proton cannot parse the old literals.
- `runtime/xdk/d3d8.cpp` — optional, config-gated `D3D_DeviceInit` call after `CreateDevice` (disabled for
  Conker; kept as a generic hook).
- `runtime/xdk/dsound.cpp` — `NewBuffer` requires a `DSBUFFERDESC` (`dwSize == 0x18`) and caps the byte
  count, so the engine's XACT descriptor cannot ask for a multi-GB contiguous block.
- `runtime/kernel/xboxkrnl.cpp` — `IoQueryVolumeInformation` (export 76), now popping its 5 stack arguments.
- `harness/build.sh`, `harness/run.sh` — executable bit only. Regenerate the whole patch with
  `git -C XboxXBERecomp diff > ../patches/xboxxberecomp.patch`.

## Current blocker

Engine/frontend init now runs deep into the audio path and **crashes inside a DirectSound library method
the runtime does not replace**:

```
exception SIGSEGV, write/read guest 0xb0
  __imp__sub_004C5708+105  (mov esi,[esi+...], null base + 0xb0)
  <- __imp__sub_004C5A03    (an un-HLE'd DSound export)
  <- __imp__sub_0054E22F    (XACT: setting up an XACT sound's DSound buffer)
```

The runtime implements DirectSound as **host-side objects** (`runtime/xdk/dsound.cpp`'s `Voice` map): the
guest objects it hands out are small, zeroed blocks with just the two stream vtables. The engine also calls
several DSound library exports through their console addresses, and `dsound.cpp`'s own `AuditExports()`
warns about exactly these ("exports running their original code"). Those bodies read fields of a real
console `CDirectSoundBuffer` / `CDirectSoundStream` (e.g. `[buffer+0x10]`, and the object behind `+0xb0`
here) that the runtime's objects never build. Two of them are already patched in `hooks.cpp` (see the
progress table); `0x4C5A03`/`0x4C5708` are the next.

### Suggested next steps
1. **Finish the DSound surface.** For every export `AuditExports()` lists as running original code
   (`notes/…` / the run log), either HLE it in `runtime/xdk/dsound.cpp` or no-op it from
   `src/game/hooks.cpp` the way `0x4C3F2F` is. The engine reaches these through XACT in `0x54E05E`/
   `0x54E22F`; `0x4C5A03`, `0x4C519C`, `0x4C51D4`, `0x4C5230` are the ones called here. A more robust fix
   would grow/build the runtime's buffer object so the library bodies see the fields they expect.
2. Keep host-replacing the remaining D3D internals from `notes/d3d_internals_called_from_text.txt` as
   crashes surface (mapping an internal to an existing HLE in `config/game.toml`, like
   `D3DDevice_DrawVerticesUP`).
3. Implement more kernel exports on demand (the running log names each missing one).
4. Rebuild and smoke-test the Windows/Proton exe after config or SDK changes (same commands as
   [Reproduce from scratch](#6b-build-the-windows-exe-and-run-under-proton)).

### Asides found while fixing
- Kernel HLE exports are `stdcall`: the `KERNEL_EXPORT(ord, name, nargs)` count must equal the pushes at
  *every* call site. A mismatch corrupts guest `esp` one call and crashes somewhere unrelated later —
  which is what the old `0x62A60` blocker really was.
- Conker's XACT region (`0x54xxxx`) and DSOUND (`0x4Cxxxx`) both run console code that pokes memory-mapped
  I/O; anything the runtime does not back faults instead of returning a plausible value.
- `CDevice_VertexShader` (`+0x794`) is dereferenced by a widely-called D3D internal (`0x536810`, ~68
  callers) as soon as any settable state changes, so it must never be null.

## Gotchas

- The generated C++ is ~227 MB and 132 files; builds are cache-sensitive (`build.sh -G` shares objects).
- `xbe_symbols` **overwrites** `config/game.toml`, including hand-written `[abi]`/`[data]`. Keep a copy, or
  re-merge after regenerating.
- Proton needs a display and Vulkan; this host has `DISPLAY=:0`/Wayland and an NVIDIA GPU.
- `umu-run` (umu-launcher 1.4.1) is provided at the repository root as `./umu-run`; GE-Proton11-7 lives in
  the Steam `compatibilitytools.d` directory.
- `.gitignore` files: `iso/`, `game/{out,assets,prefix,winrun,generated,third_party/mingw/lib}` are ignored;
  the SDK changes live only in `patches/`, so **apply the patch after any fresh SDK checkout**.
