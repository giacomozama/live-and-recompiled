# Xbox XBE Recomp

An SDK for statically recompiling original Xbox (x86 XBE) games into native 64 bit Windows executables. The game's x86 code is translated to C++ ahead of time. The Xbox kernel and the XDK libraries (Direct3D 8, DirectSound, XAPI, XNet) are implemented at the API level on Windows. Everything is drawn with a native Direct3D 12 renderer and played through XAudio2.

This tool was originally created for porting [Tom Clancy's Splinter Cell: Double Agent](https://github.com/TGP482/SCDoubleAgent-XBERecomp).

> [!NOTE]
> This project does not include any game assets, keys or generated code. You must provide the files from your own legally acquired copy of the game to build.

## Installation

- Put this repository in your game repository as `sdk/`. This can be a copy or a submodule.
- In the game's `CMakeLists.txt`:
   ```cmake
   include(sdk/cmake/XbeRecomp.cmake)
   file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
   list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")
   add_xbe_image(MyGame default.xbe game.toml 128 game)
   ```
- Build with Clang and Ninja (`clang++`, `-fuse-ld=lld`). Zydis, toml++, Dear ImGui and SDL3 are fetched by CMake.

## Game Setup

1. Copy `templates/game` to a new repository and put this SDK in it as `sdk/`. Set `GAME_TITLE`, `GAME_XBE` and `GAME_EXE` at the top of `build.bat` (`GAME_EXE` in `release.bat` too), and the target and xbe in `add_xbe_image` in `CMakeLists.txt`.
2. Put the game's XBE and its data folders in the `assets` folder. No game files, keys or generated code belong in any repository.
3. Run `xbe_symbols`, which `build.bat` does on the first build:
   ```
   xbe_symbols assets\default.xbe sdk\xdk --out config\game.toml
   ```
   This prints the XBE's library versions, picks `xdk/<build>/signatures.toml` and finds the XDK functions and data the host replaces, then writes the config: `[xbe]` with a partition count and the code sections, the `layout.toml` include, `[symbols]` and `[data]`. Misses and ambiguous names are listed and left at 0, which become trapping stubs. It stops when no `signatures.toml` exists for the XBE's build.
4. Fill in `app::kGame` in `src/common/game_info.cpp`: title, window name, log and settings file names, splash, the xbe to exe table, game options, and the hooks for loading, the window, drawing and the pads.
5. Game hooks go in `src/game`. Define `X86_FUNC(sub_XXXXXXXX)` to replace a recompiled function, and call `__imp__sub_XXXXXXXX` to reach the original. `python sdk/tools/symbols/xbe2pe.py config/game.toml game.exe` writes a flat PE with the recompiled functions as exports for Ghidra.
6. Run `build.bat`. One game repo can build several images, one config and `add_xbe_image` each; `--name` sets the config's `generated/<name>`, which is the folder argument of `add_xbe_image`.

A new XDK build needs `xdk/<build>/layout.toml` and signatures made from titles whose configs are complete: `xbe_symbols --sign config/a.toml [config/b.toml] --out sdk/xdk/<build>/signatures.toml`. Signatures are lengths and hashes of the functions with addresses masked, never library code.

## Features

### Recompiler
- **Function Discovery** - Finds functions with a direct call fixpoint, pointer seeds, gap fill and trusted walks across guessed entries.
- **Jump Tables** - Resolves bounded, byte indexed and backwards tables, MSVC memcpy tables and movsb/movsd sleds.
- **C++ Exceptions** - Supports MSVC FuncInfo thunks and catch funclet continuations.
- **x87, SSE and MMX** - Translates x87 with a dynamic top of stack, SSE and MMX, along with the CPUID probe flags.
- **Overridable Output** - Makes every function a weak `sub_X` over `__imp__sub_X`, allowing the host to replace any of them without editing generated code.
- **Instruction Hooks** - Adds `[[hook]]`, which calls a game defined function before or after any guest instruction, with guest flags kept in `ctx`; a bool hook can return from the function or jump within it.
- **Analysis Overrides** - Adds `[[function]]` to add an entry with an optional decode end, `[[switch]]` to set the targets of an indirect jmp and `[xbe] data_ranges` to mark bytes that are never code.
- **Incremental** - Only rewrites generated files when their content changes.
- **XDK Signatures** - Finds the XDK functions and data of a new title by masked hashes and writes its config.

### CPU and Kernel
- **Guest Calls** - Adds an indirect call table over the code and host calls into guest code, with RDTSC at 733.33 MHz and a Pentium III CPUID.
- **Kernel** - Implements objects and waits, host threads, files mapped to host folders or read from a disc image, memory with the physical mirrors at 0x80000000 and 0xF0000000, crypto, EEPROM settings and C++ exception dispatch.
- **Image Switching** - Relaunches into the matching executable on XLaunchNewImage and quick reboots, handing the kept display over without a gap.

### Graphics
- **Direct3D 8 at the API Level** - Implements the device, targets, states, shaders, CopyRects and Swap as the XDK does them.
- **Native Direct3D 12 Renderer** - Translates NV2A vertex programs, register combiners, texture modes and the fixed function pipeline to HLSL, with a threaded command stream and target and texture caches.
- **Render Scale** - Renders the game at any scale with integer pixel centre correction and upscale aware blur passes.
- **Renderer Extensions** - Allows a game to change target sizes and formats, add passes where frames are read and add to the generated HLSL through `gpu::Extension` (`runtime/gpu/ext.h`), set with `gpu::SetExtension` from `GameInfo::init`. Without one the console path is drawn.

### Audio
- **DirectSound at the API Level** - Mixes in software on XAudio2 with Xbox ADPCM, streams and the effects image.

### Input
- **Controllers** - Supports hot plugged Xbox, PlayStation and Switch controllers through SDL3, with rumble.

### General
- **Disc Images** - Runs the game from an `.iso` in `assets` (whole disc dump or game partition) as well as from extracted files in release builds.
- **System Link** - Implements XNet over Winsock for LAN play.
- **Crash Dumps** - Writes minidumps to a CrashDumps folder beside the exe.
- **Diagnostics** - Logs frame timing with `XBE_PERF`, captures a frame with F3 when `XBE_CAPTURE_DIR` is set and turns on the D3D12 debug layer with `XBE_D3D12_DEBUG`.
- **Bink** - Fixes BinkGetSummary for instant host reads.

## Credits

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the overridable function layout and instruction hooks this follows.
- [Zydis](https://github.com/zyantific/zydis) for decoding x86 instructions, [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.
- [xemu](https://github.com/xemu-project/xemu) and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded), read for hardware and API behaviour.
