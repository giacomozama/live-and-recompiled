# Conker: Live & Recompiled

An unofficial native PC port of the original Xbox *Conker: Live & Reloaded*, built on a heavily modified version of
[Xbox XBE Recomp](https://github.com/TGP482/XboxXBERecomp). Currently supports Linux and Windows.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own copy of
> the game. The game [can currently be purchased from the Windows Store](https://www.xbox.com/games/store/conker-live-and-reloaded/bvfb8cbs75r6). The Windows Store version is fully compatible with this project.

> [!WARNING]
> **AI usage in this project**
>
> In case you're wondering, ALL first-party code here is LLM generated. It was tested manually, but pretty much no code at all was typed in by humans. I think it works well, but it IS one-hundred percent, unadulterated, vibecoded slop. 

## Status and Known Bugs

- Single player SHOULD be fully playable but there may be uncaught bugs/edge cases. Playtesting is very appreciated: you are encouraged to play through the game and report bugs, crashes and glitches.
- Still in the process of recording shaders encountered during gameplay. These can then be rebuilt at startup time so future players never encounter stutters even during their first gameplay. Sharing these is also appreciated (`shaders.rec`)
- No effort has been made to implement multiplayer yet.

## Features

- **High frame rates** - 60, 120, 180 or 240 FPS instead of the original 30 (experimental).
- **Any aspect ratio** - Ultrawide and other displays get a wider view, with the HUD stretched to fit.
- **Better graphics** - Anti-aliasing, additional post-processing effects, improved fur, flowing and rippling water, 
shiny armour, see-through glass and swaying distant grass.
- **Controls** - Xbox, PlayStation and Switch controllers with rumble, or keyboard and mouse with mouse look; all
  rebindable.
- **Skippable cutscenes** - Skip any in-engine cutscene without seeing them first.

Everything is configurable in the Esc menu and in `settings.toml`.

## Installation

A release contains none of the game. Part of the recompiled code is a mechanical translation of the game's own
`default.xbe`, including Microsoft's XDK libraries, which can't be shared; the rest (the recompiler, runtime and hooks)
is written for this project. So Setup recompiles your own copy on your PC, once, with a compiler it downloads from the
official release and checks by SHA-256. Emulators and decompilation projects work the same way. (This isn't legal
advice.)

**Supported versions**
- The USA disc release (`default.xbe` SHA-1 = `2232a1b3874d67b4bb4cf53a940420d2dab59535`)
- The digital XBOX Backwards Compatibility release from the Microsoft Store (equivalent to the USA version).

It is three steps: put the game in `assets`, run Setup, run the game.

1. **Put your game in the `assets` folder.** What you have decides how:
   - **Disc image (`.iso`)** or **extracted files** (`default.xbe` with the game's folders beside it): put it in
     `assets` as it is.
   - **Digital XBOX Backwards Compatibility release** (`DefaultPackage` with a `DefaultPackage.data` folder): copy both
     into `assets`. Setup unpacks them (a few seconds, about 5 GB free); afterwards the package can go.
     - **Windows:** if it is installed from the Microsoft Store in its default place
       (`C:\XboxGames\Conker- Live and Reloaded\Content\Game`, not encrypted), leave `assets` empty. Setup finds it,
       asks whether to use it, and unpacks it for you.
     - **Linux:** if you bought it on the Microsoft Store, download its package with the
       [Xodus project](https://github.com/xodus-gaming/xodus) or copy it from a Windows install.
2. **Run Setup**: `setup-windows.exe` on Windows, or `./setup-linux` in a terminal opened in this folder on Linux. It
   unpacks the digital release if needed, checks your copy is a supported version, downloads the compiler (first time
   only), recompiles the game and writes the executable. This takes a few minutes. Nothing of the game is uploaded.
   - **Windows:** downloads [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (80 to 180 MB) and writes `Game.exe`.
   - **Linux:** downloads [Zig](https://ziglang.org) (about 50 MB) and writes the native `Game`. If it says permission
     denied, your unzip tool dropped the executable bit: `chmod +x setup-linux kit/bin/xbe_recomp`.
3. **Run `Game.exe`** (Windows) **or `./Game`** (Linux). Settings are in `settings.toml` beside it, and in the Esc menu.

Setup keeps nothing it generated except the game and the compiler in `kit/toolchain`. Run it again after updating
to a new release.

## Building from source

See [BUILDING.md](BUILDING.md).

## Credits

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp), the recompiler and runtime this is built on.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the overridable function layout and instruction hooks.
- [xemu](https://github.com/xemu-project/xemu) and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) as references for the Xbox's hardware and API behaviour. This project does not use emulation or any emulator code.
- [SMAA](http://www.iryoku.com/smaa/) by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez (MIT), for the anti-aliasing effect.
- [Zydis](https://github.com/zyantific/zydis) for decoding x86 instructions, [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (Clang, LLD, mingw-w64, libc++), the compiler `setup-windows.exe` builds the game with.
- [Zig](https://ziglang.org) (`zig c++`: Clang, LLD, glibc stubs, libc++), the compiler `setup-linux` builds the native game with.
