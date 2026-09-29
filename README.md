# XboxXBE-Recomp

The XboxXBE-Recomp serves as a software development kit that converts original Xbox (x86 XBE) software into native 64-bit Windows applications. The process translates the x86 machine code of the software into C++ source code before the application runs. There is an implementation of the Xbox kernel and the XDK libraries, like Direct3D 8, DirectSound, XAPI & XNet, at the API level for the Windows platform. The system uses Direct3D 12 to render all graphics and utilizes XAudio2 to output sound. It does not use an emulator to run the code.

Installation

To begin place this repository inside the software repository as a directory named `sdk/` via a copy or a submodule.
On the `CMakeLists.txt` file of the software, add the following instructions
```cmake
include(sdk/cmake/XbeRecomp.cmake)
file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src//.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")
add_xbe_image(MyGame default.xbe game.toml 128 game)
```
By using Clang or Ninja (`clang++`, `-fuse-ld=lld`), compile the software. The CMake tool automatically downloads Zydis, toml++ & Dear ImGui during this process.

## Game Setup

## Features

### Recompiler
- **Function Discovery** - Direct call fixpoint, pointer seeds, gap fill and trusted walks across guessed entries.
- **Jump Tables** - Bounded, byte indexed and backwards tables, MSVC memcpy tables and movsb/movsd sleds.
- **C++ Exceptions** - MSVC FuncInfo thunks and catch funclet continuations.
- **x87, SSE and MMX** - Dynamic top of stack x87, SSE and MMX, CPUID probe flags.
- **Overridable Output** - Every function is a weak `sub_X` over `__imp__sub_X`, so the host replaces any of them without editing generated code.
- **Incremental** - Generated files are only rewritten when their content changes.

### CPU and Kernel
- **Guest Calls** - Indirect call table over the code, host calls into guest code, RDTSC at 733.33 MHz and a Pentium III CPUID.
- **Kernel** - Objects and waits, host threads, files mapped to host folders, memory with the physical mirrors at 0x80000000 and 0xF0000000, crypto.

### Graphics
- **Shader Translation** - NV2A vertex programs, register combiners, texture modes and the fixed function pipeline translated to HLSL.
- **Textures and Vertices** - Xbox texture formats and swizzle, raw vertex and index data straight to the GPU.
- **Render Thread** - Deferred threaded command stream with fences and late read backs.

### Audio
- **DirectSound at the API Level** - Software mixer on XAudio2 with Xbox ADPCM, streams and the effects image.

### General
- **XAPI** - Controllers, rumble and launch data.
- **Bink** - BinkGetSummary fix for instant host reads.
- **Logging** - Log file with a per game prefix.

### Audio
- **DirectSound at the API Level** - Software mixer on XAudio2 with Xbox ADPCM, streams and the effects image.

### General
- **XAPI** - Controllers, rumble and launch data.
- **Bink** - BinkGetSummary fix for instant host reads.
- **Logging** - Log file with a per game prefix.

## Credits

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the recompiled function layout this follows.
- [Zydis](https://github.com/zyantific/zydis), [toml++](https://github.com/marzer/tomlplusplus) and [Dear ImGui](https://github.com/ocornut/imgui).
- [xemu](https://github.com/xemu-project/xemu) for hardware and API behaviour.
- [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase) for finding XDK functions in retail XBEs.
