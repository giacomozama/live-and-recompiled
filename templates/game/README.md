# Game Title - Recomp

An unofficial native PC port of the original Xbox version of Game Title.

This port was created using the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool.

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

### Graphics
- 

### General

## Credits

- [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp), the recompiler and runtime this is built on.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the overridable function layout and instruction hooks.
- [Zydis](https://github.com/zyantific/zydis) for decoding x86 instructions, [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.
