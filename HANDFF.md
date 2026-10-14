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
- `Direct3D_CreateDevice` (×2), 640×480, 4 vertex shaders created
- volume query (`IoQueryVolumeInformation`), into **engine initialisation**
- reaches ~0.9 s of guest execution, then **crashes** writing guest address `0x18` from engine function
  `0x62A60` (see [Current blocker](#current-blocker)).

Pipeline: 21,956 functions / 1.47 M instructions recompiled → 227 MB of C++ → `Game.exe`.

## Layout

```
conker-lar/
  Conker Live and Reloaded.iso     the user's disc image (gitignored via iso/)
  iso/                             extracted disc (default.xbe, dvddata/, media/)
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
  ~/Downloads/umu-launcher-1.4.1-zipapp/umu/umu-run "$PWD/Game.exe"
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
- **First host replacements** (`src/game/hooks.cpp`):
  - `0x538D50` (per-frame buffer clear): `SetRenderTarget(backbuffer, autoDepth)` + `Clear(target|depth,0,z=1)`
    through `GUEST_CALL(D3DDevice_*)`.
  - `0x543170` (library deferred state flush): no-op that clears the handled dirty bits — the runtime keeps
    its own state and decodes the engine's pushbuffer writes.
- **Kernel export 76** `IoQueryVolumeInformation` implemented in `runtime/kernel/xboxkrnl.cpp` (size/device
  class for the mounted volumes).

## Changes to the provided tools (`patches/xboxxberecomp.patch`)

- `cmake/XbeRecomp.cmake` — adds `XBE_CODEGEN_TOOL` so codegen can run with a **native** `xbe_recomp` while
  cross-compiling the game for Windows.
- `runtime/kernel/memory.cpp` / `memory.h` — Linux build needs `<unistd.h>`; backs the NV2A register window
  `0xFD000000` with scratch on both platforms (Conker's library code pokes it directly).
- `runtime/gpu/shaders.cpp` — replaces the `1.#INF` / `-1.#INF` HLSL literals with
  `asfloat(0x7F800000u)` / `asfloat(0xFF800000u)`; vkd3d-proton cannot parse the old literals.
- `runtime/xdk/d3d8.cpp` — optional, config-gated `D3D_DeviceInit` call after `CreateDevice` (disabled for
  Conker; kept as a generic hook).
- `runtime/kernel/xboxkrnl.cpp` — `IoQueryVolumeInformation` (export 76).
- `harness/build.sh`, `harness/run.sh` — executable bit only.

## Current blocker

Engine init crashes on a bogus object write:

```
exception C0000005 ... write ... guest 00000018
... from 0x62A60 (sub_00062A60) <- 0x62330 <- 0x5E5A0 <- 0x63680 <- 0x1F5440 <- 0x2A6970 (main loop)
```

`0x62A60` is a fastcall-style function (`esi` = object, `ecx`/`edx` args). `esi` is invalid (~`0x10`), so a
subsystem structure — likely behind the globals `0x84a190` / `0x84a19c` loaded in `0x62330` — is not
populated. Both the harness and the Proton build stop at the same point.

### Suggested next steps
1. Debug `0x62A60`/`0x62330` with `tools/xbe_disasm.py` and gdb (set a timer interrupt; the headless harness
   is the fast loop). Find what should populate `[0x84a190]` (probably an engine init that a missing XDK
   return or an unimplemented export skipped).
2. Compare against a second reference port built with this exact SDK (e.g. the SCDA recomp,
   <https://github.com/TGP482/SCDA-XBERecomp>) for `[abi]`/`[data]` and hook patterns.
3. Continue host-replacing the remaining D3D internals from
   `notes/d3d_internals_called_from_text.txt` as crashes surface (the runtime's HLE already implements the
   public API; each replacement just routes the internal's intent to it).
4. Implement more kernel exports on demand (the running log names each missing one).

## Gotchas

- The generated C++ is ~227 MB and 132 files; builds are cache-sensitive (`build.sh -G` shares objects).
- `xbe_symbols` **overwrites** `config/game.toml`, including hand-written `[abi]`/`[data]`. Keep a copy, or
  re-merge after regenerating.
- Proton needs a display and Vulkan; this host has `DISPLAY=:0`/Wayland and an NVIDIA GPU.
- `umu-run` is not on `PATH` here; the command above uses the 1.4.1 zipapp under `~/Downloads`.
- `.gitignore` files: `iso/`, `game/{out,assets,prefix,winrun,generated,third_party/mingw/lib}` are ignored;
  the SDK changes live only in `patches/`, so **apply the patch after any fresh SDK checkout**.
