# Game Title - XBE Recomp

An unofficial native PC port of the original Xbox version of Game Title, with extended graphics options and other enhancements.

This port was created using the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

## Installation

- Put your game files in the `assets` folder. This can be an ISO, or the extracted data files.
- Run `Game.exe`.
- Recomp settings can be adjusted in `settings.toml` beside the exe.

## Features

### Display
- **Frame Rate** - 30 (the original), 60 or 120 (experimental) FPS, from the F1 menu or `frame_rate` in
  `settings.toml`. The game's timers follow real time at any rate. The FPS cap and V-Sync still apply.
- **Any Aspect Ratio** - The 3D view widens to the display's shape (`aspect = "auto"`), or to any `W:H` such as
  `"21:9"`; `"4:3"` keeps the original view. The HUD stretches with the frame. Raise Render % to sharpen wide frames.


### Graphics
- **Shader Warm-up** - The shaders and pipelines a run builds are listed in `shaders.rec` beside the exe, and the next
  launch builds them in the background while the game boots, before the frames that need them. `shaders.rec` holds no
  game code and can be shared: vertex programs are named in it by hash, and their code goes in `shader_programs.bin`,
  which stays on your PC. A shared `shaders.rec` alone warms everything but the vertex shaders of programs your copy
  has not run yet. `shader_warmup = false` in `settings.toml` turns the warm-up off; recording stays on.

### Controller
Supports Xbox, PlayStation and Switch controllers, with hot plugging and rumble.

A gamepad is not required: the keyboard drives player 1 as well, and the two can be used at
the same time.

| Action | Key |
|---|---|
| Move (left stick) | `W` `A` `S` `D` |
| D-pad | arrow keys |
| Camera (right stick) | `I` `J` `K` `L` |
| A / B / X / Y | `Space` / `Left Shift` / `E` / `Q` |
| LB (white) / RB (black) | `1` / `2` |
| LT / RT | `3` / `4` |
| Start / Back | `Enter` / `Backspace` |
| Left / right stick click | `C` / `V` |

### General

### Miscellaneous
- **Boot Splash** - Place a `splash.png` (or `.bmp`) beside the exe to display a splash screen at launch.

## System Requirements

### Minimum
- **OS**
  - Windows 10 64-bit
- **GPU** with support for Direct3D 12

## Building

### 1. Clone the Repository
Clone this repository using [Git](https://git-scm.com/).

### 2. Add the Required Game Files
Place your extracted game data files in the `assets` folder. `default.xbe` must be there, with the game's folders beside it.

### 3. Install Dependencies
Install Visual Studio 2022 with **Desktop development with C++**, **C++ Clang tools for Windows** and **C++ CMake tools for Windows** (`build.bat` offers to add the last two).

### 4. Build the Project
Run `build.bat`. The first build recompiles the game and takes a while. The exe is written to `out\build\release`.

## Credits

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp), the recompiler and runtime this is built on.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the overridable function layout and instruction hooks.
- [Zydis](https://github.com/zyantific/zydis) for decoding x86 instructions, [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.
