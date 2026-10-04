# Game Title - Recomp

A static recompilation of the original Xbox version of Game Title into a native 64 bit Windows executable. The game's x86 code is translated to C++ ahead of time, the Xbox kernel, Direct3D and DirectSound calls are implemented on Windows, and everything is drawn with Direct3D 12 and played through XAudio2.

## Installation

1. Install Visual Studio 2022 with **Desktop development with C++**, **C++ Clang tools for Windows** and **C++ CMake tools for Windows** (build.bat offers to add the last two), plus Git.
2. Clone this repository.
3. Put the game files in the `assets` folder (see Game Setup).
4. Run `build.bat`. The first build recompiles the game and takes a while.
5. Run `out\build\release\Game.exe`.

> [!NOTE]
> No game data, XBE bytes or generated code are in this repository. You must own the game.

## Game Setup

Copy the game disc's files into `assets`: `default.xbe` with the game's folders beside it. `default.xbe` becomes `Game.exe`. To play, the `assets` folder beside the exe takes the same files or the disc image (`.iso`).

## Features

### Display
- **Native Renderer** - The game renders through a native Direct3D 12 renderer, no emulation.
- **Display Mode** - Allows you to change between Fullscreen, Borderless and Windowed modes.
- **Resolution** - Lists every mode your monitor reports and defaults to the desktop mode.
- **Render Resolution** - Renders the game at 20% to 200% of the output resolution, independent of the window size.
- **Max Frame Rate** - Caps the frame rate at your monitor's refresh rate, at a value you pick, or removes the cap.
- **VSync** - Turns vertical sync on or off.

### Graphics
- **Anisotropic Filtering** - Sharper textures at grazing angles.

### General
- **Disc Images** - Runs the game from an `.iso` in the `assets` folder, a whole disc dump or the game partition alone.
- **Settings** - Saved in the settings file beside the exe.
- **Crash Dumps** - Minidumps are written to `CrashDumps` beside the exe.

## Credits

- XboxXBE-Recomp, the recompiler and runtime this is built on.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the recompiled function layout.
- [Zydis](https://github.com/zyantific/zydis), [toml++](https://github.com/marzer/tomlplusplus) and [Dear ImGui](https://github.com/ocornut/imgui).
