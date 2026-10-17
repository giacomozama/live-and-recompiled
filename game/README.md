# Conker: Live & Reloaded - XBE Recomp

An unofficial native PC port of the original Xbox version of Conker: Live & Reloaded, with extended graphics options and other enhancements.

This port was created using the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

## Installation

The release contains no game code: the game is built on your PC from your own copy, once.

1. Put your game in the `assets` folder: the disc image (`.iso`), or `default.xbe` with the game's folders beside it.
2. Run `Setup.exe` (on Linux or Steam Deck, `./setup-linux` in a terminal opened in this folder). It checks your
   copy is the release this build supports, downloads the compiler it needs
   ([llvm-mingw](https://github.com/mstorsjo/llvm-mingw), 80 to 180 MB, the first time only), recompiles the game and
   writes `Game.exe`. This takes a few minutes. Nothing of the game is uploaded or downloaded.
3. Run `Game.exe`. Settings are in `settings.toml` beside it, and in the F1 menu.

Setup keeps nothing it generated except `Game.exe` and the compiler in `kit/toolchain`. Run it again after updating
to a new release.

On Linux or Steam Deck, `Game.exe` runs through Proton or Wine: add it as a non-Steam game, or use
`umu-run Game.exe`. If `./setup-linux` says permission denied, your unzip tool dropped its executable bit:
`chmod +x setup-linux kit/bin/xbe_recomp`. `Setup.exe` works under Proton too (`umu-run Setup.exe`).

## Features

### Display
- **Frame Rate** - 30 (the original), 60 or 120 (experimental) FPS, from the F1 menu or `frame_rate` in
  `settings.toml`. The game's timers follow real time at any rate. The FPS cap and V-Sync still apply.
- **Any Aspect Ratio** - The 3D view widens to the display's shape (`aspect = "auto"`), or to any `W:H` such as
  `"21:9"`; `"4:3"` keeps the original view. The HUD stretches with the frame. Raise Render % to sharpen wide frames.


### Graphics
- **MSAA** - 2x, 4x or 8x multisample anti-aliasing of the 3D view, from the F1 menu or `msaa` in `settings.toml`. It
  applies at once. Alpha-tested edges such as foliage and fur are not smoothed.
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

These are the defaults: every action can have up to two keys or mouse buttons, rebound under **Controls → Keyboard**
in the F1 menu (click a key, press the new one; right click unbinds, **Reset** brings back that action's default) and
saved with the menu's **Save**. They live in the `[keys]` table of `settings.toml`, by key name, so they can be edited
there too:

```toml
[keys]
move_up = 'W'
a = ['Space', 'Mouse 1']
lt = 'none'
```

The names are the ones the menu shows (`Space`, `Left Shift`, `Escape`, `Numpad 0`, `Mouse 4`, `F5`, `;` ...). F1
and Insert are kept for opening the menu.

### General
- **Skippable Cutscenes** - LT skips any in-engine cutscene, not only the ones already seen this playthrough. Turn it on
  in the F1 menu or with `skippable_cutscenes = true` in `settings.toml`.

### Miscellaneous
- **Boot Splash** - Place a `splash.png` (or `.bmp`) beside the exe to display a splash screen at launch.

## System Requirements

### Minimum
- **OS**
  - Windows 10 64-bit
- **GPU** with support for Direct3D 12

## Building from Source

Only needed to work on the port; players use `Setup.exe` from a release.

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
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (Clang, LLD, mingw-w64, libc++), the compiler `Setup.exe` builds the game with.
