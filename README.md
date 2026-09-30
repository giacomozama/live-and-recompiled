# XboxXBE-Recomp

An SDK for statically recompiling original Xbox (x86 XBE) games into native 64 bit Windows executables. The game's x86 code is translated to C++ ahead of time. The Xbox kernel and the XDK libraries (Direct3D 8, DirectSound, XAPI, XNet) are implemented at the API level on Windows. Everything is drawn with Direct3D 12 and played through XAudio2. There is no emulation.

## Installation

1. Put this repository in your game repository as `sdk/` (a copy or a submodule).
2. In the game's `CMakeLists.txt`:
   ```cmake
   include(sdk/cmake/XbeRecomp.cmake)
   file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
   list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")
   add_xbe_image(MyGame default.xbe game.toml 128 game)
   ```
3. Build with Clang and Ninja (`clang++`, `-fuse-ld=lld`). Zydis, toml++ and Dear ImGui are fetched by CMake.

## Game Setup

1. Copy `templates/game` to a new repository and put this SDK in it as `sdk/`. Set `GAME_TITLE`, `GAME_XBE` and `GAME_EXE` at the top of `build.bat` (`GAME_EXE` in `release.bat` too), and the target and xbe in `add_xbe_image` in `CMakeLists.txt`.
2. Put the game's XBE and its data folders in the `assets` folder. No game files, keys or generated code belong in any repository.
3. Run `xbe_symbols`, which `build.bat` does on the first build:
   ```
   xbe_symbols assets\default.xbe sdk\xdk --out config\game.toml [--xbsdb symbols.txt]
   ```
   It prints the XBE's library versions, picks `xdk/<build>/signatures.toml` and finds the XDK functions and data the host replaces, then writes the config: `[xbe]` with a partition count and the code sections, the `layout.toml` include, `[symbols]` and `[data]`. Misses and ambiguous names are listed and left at 0, which become trapping stubs. `--xbsdb` takes the output of `XbSymbolDatabaseCLI -e <xbe>`: its names fill misses and its functions become extra entry points. `build.bat` runs it when `XbSymbolDatabaseCLI` is on PATH; the SDK only calls that tool and holds none of its data.
4. Fill in `app::kGame` in `src/common/game_info.cpp`: title, window name, log and settings file names, splash, the xbe to exe table, game options, and the hooks for loading, the window, drawing and the pads.
5. Game hooks go in `src/game`. Define `X86_FUNC(sub_XXXXXXXX)` to replace a recompiled function, and call `__imp__sub_XXXXXXXX` to reach the original. `python sdk/tools/symbols/xbe2pe.py config/game.toml game.exe` writes a flat PE with the recompiled functions as exports for Ghidra.
6. Run `build.bat`. One game repo can build several images, one config and `add_xbe_image` each; `--name` sets the config's `generated/<name>`, which is the folder argument of `add_xbe_image`.

A new XDK build needs `xdk/<build>/layout.toml` and signatures made from titles whose configs are complete: `xbe_symbols --sign config/a.toml [config/b.toml] --out sdk/xdk/<build>/signatures.toml`. Signatures are lengths and hashes of the functions with addresses masked, never library code.

## Features

### Recompiler
- **Function Discovery** - Direct call fixpoint, pointer seeds, gap fill and trusted walks across guessed entries.
- **Jump Tables** - Bounded, byte indexed and backwards tables, MSVC memcpy tables and movsb/movsd sleds.
- **C++ Exceptions** - MSVC FuncInfo thunks and catch funclet continuations.
- **x87, SSE and MMX** - Dynamic top of stack x87, SSE and MMX, CPUID probe flags.
- **Overridable Output** - Every function is a weak `sub_X` over `__imp__sub_X`, so the host replaces any of them without editing generated code.
- **Incremental** - Generated files are only rewritten when their content changes.
- **XDK Signatures** - `xbe_symbols` finds the XDK functions and data of a new title by masked hashes and writes its config.

### CPU and Kernel
- **Guest Calls** - Indirect call table over the code, host calls into guest code, RDTSC at 733.33 MHz and a Pentium III CPUID.
- **Kernel** - Objects and waits, host threads, files mapped to host folders, memory with the physical mirrors at 0x80000000 and 0xF0000000, crypto, EEPROM settings, C++ exception dispatch.
- **Image Switching** - XLaunchNewImage and quick reboots relaunch into the matching executable, with the kept display handed over without a gap.

### Graphics
- **Direct3D 8 at the API Level** - Device, targets, states, shaders, CopyRects and Swap as the XDK does them.
- **Native Direct3D 12 Renderer** - NV2A vertex programs, register combiners, texture modes and the fixed function pipeline translated to HLSL, threaded command stream, target and texture caches.
- **Render Scale** - Any render scale with integer pixel centre correction and upscale aware blur passes.
- **Renderer Extensions** - `gpu::Extension` (`runtime/gpu/ext.h`) lets a game change target sizes and formats, add passes where frames are read and add to the generated HLSL, set with `gpu::SetExtension` from `GameInfo::init`. Without one the console path is drawn.

### Audio
- **DirectSound at the API Level** - Software mixer on XAudio2 with Xbox ADPCM, streams and the effects image.

### Input
- **Controllers** - XInput pads with rumble.

### General
- **System Link** - XNet over Winsock for LAN play.
- **Crash Dumps** - Minidumps in CrashDumps beside the exe.
- **Diagnostics** - `XBE_PERF` logs frame timing, `XBE_CAPTURE_DIR` lets F3 capture a frame, `XBE_D3D12_DEBUG` turns on the D3D12 debug layer.
- **Bink** - BinkGetSummary fix for instant host reads.

## Credits

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the recompiled function layout this follows.
- [Zydis](https://github.com/zyantific/zydis), [toml++](https://github.com/marzer/tomlplusplus) and [Dear ImGui](https://github.com/ocornut/imgui).
- [xemu](https://github.com/xemu-project/xemu) and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded), read for hardware and API behaviour.
- [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase) for finding XDK functions in retail XBEs.
