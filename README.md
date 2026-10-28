# Live & Recompiled

An unofficial native PC port of the original Xbox *Conker: Live & Reloaded*, built on a heavily modified version of
[Xbox XBE Recomp](https://github.com/TGP482/XboxXBERecomp). Currently supports Linux and Windows.

This project is not affiliated with or endorsed by Rare or Microsoft. *Conker* and *Conker: Live & Reloaded* are
trademarks of their respective owners.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own copy of
> the game. The game [can currently be purchased from the XBOX Store](https://www.xbox.com/games/store/conker-live-and-reloaded/bvfb8cbs75r6). The XBOX Store version is fully compatible with this project.

> [!WARNING]
> **AI disclaimer**
>
> ALL first-party code here is LLM generated and was NOT reviewed line-by-line. Testing was mostly done manually.

## Features

- **High frame rates** - 60, 120, 180 or 240 FPS instead of the original 30.
- **Any aspect ratio** - Ultrawide and other displays get a wider view.
- **Better graphics** - Anti-aliasing, additional post-processing effects, visual and gameplay enhancements.
- **Controls** - Xbox, PlayStation and Switch controllers with rumble, or keyboard and mouse with mouse look.
- **Skippable cutscenes** - Skip any in-engine cutscene without having seen them once first.

Everything is configurable in the Esc menu and in `settings.toml`.

## Status

Single player is playable start to finish. It's expected to be mostly bug free and optimized almost everywhere. Some minor visual glitches might occur, especially in relation with framerates above 60 and frame interpolation.

Solo Dumbots and split screen multiplayer works, but hasn't been tested thoroughly. Lan/Online play is untested altogether.

## Installation

Releases contain neither the game's code nor its assets. Setup performs the entire extraction and recompilation process on your PC, which might require several minutes.

**Supported versions**
- The USA disc release (`default.xbe` SHA-1 = `2232a1b3874d67b4bb4cf53a940420d2dab59535`)
- The digital XBOX Backwards Compatibility release from the Microsoft Store (equivalent to the USA version).

Download the release for your system, `LiveAndRecompiled-<version>-windows.zip` or `LiveAndRecompiled-<version>-linux.tar.gz`, and unpack it.
Then it is two steps: run Setup, run the game.

1. **Run Setup**: `setup-windows.exe` on Windows, or `./setup-linux` on Linux (double-click it, or run it in a terminal).
   Its window asks for three things; the last two are already filled in:
   - **The game**: your `DefaultPackage` (the digital XBOX Backwards Compatibility release, with its
     `DefaultPackage.data` folder beside it), your disc image (`.iso`), or the folder with the extracted files
     (`default.xbe` with the game's folders beside it). Setup unpacks a package (a few seconds, about 5 GB free) and
     puts the game's files in an `assets` folder in the game folder, where the game looks for them.
     - **Windows:** if it is installed from the Microsoft Store in its default place
       (`C:\XboxGames\Conker- Live and Reloaded\Content\Game`, not encrypted), pick that folder.
       - **NOTE:** You have to run the original app and let it extract the game once first, otherwise the files will not decode correctly.
     - **Linux:** if you bought it on the Microsoft Store, download its package with the
       [Xodus project](https://github.com/xodus-gaming/xodus) or copy it from a Windows install. You can use streaming mode to extract the files automatically.
   - **Working folder**: `work` beside Setup by default (made if it's missing). The compiler is downloaded into it (once) and the
     game is built there. Setup removes the temporary files when done and keeps the compiler for next time, unless
     you untick "Keep the downloaded compiler".
   - **Game folder**: `game` beside Setup by default; where the game is made, with the game's files in an `assets` folder inside it.

   Press Recompile. Setup checks your copy is a supported version, downloads the compiler (first time only), recompiles
   the game and writes the executable. This takes a few minutes. Nothing of the game is uploaded.
   - **Windows:** downloads [Zig](https://ziglang.org) (about 100 MB) and writes `Game.exe`.
   - **Linux:** downloads [Zig](https://ziglang.org) (about 50 MB) and writes the native `Game`. If it says permission
     denied, your archive tool dropped the executable bit: `chmod +x setup-linux`.

   Without a display, or with any option, Setup runs in the terminal instead: `./setup-linux --cli --source
   path/to/DefaultPackage --work /tmp/conker-work --game-dir ~/Games/Conker`, `--source` is required and the other
   options default to the same as the window (`--help` lists them, including `--delete-compiler`).
2. **Run `Game.exe`** (Windows) **or `./Game`** (Linux), in the game folder chosen during Setup.

Setup keeps nothing it generated except the game and the compiler in the working folder (unless you chose not to). Run it again after updating to a new release.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## Building from source

See [BUILDING.md](BUILDING.md).

## Credits

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBERecomp), the recompiler and runtime this is built on.
- [xemu](https://github.com/xemu-project/xemu) and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) as references for the Xbox's hardware and API behaviour.
- [SMAA](http://www.iryoku.com/smaa/) by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez (MIT).
- [toml++](https://github.com/marzer/tomlplusplus) for the configs
- [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay
- [FLTK](https://www.fltk.org) for Setup's window
- [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.
- [Zig](https://ziglang.org) (`zig c++`: Clang, LLD, mingw-w64 and glibc stubs, libc++), the compiler Setup builds the game with, on both systems.
- [The ConkerXbox60FPSWidescreen patch](https://www.moddb.com/downloads/conkerxbox60fpswidescreen) for important information about game code.

## License

This project is licensed under the GNU General Public License, version 3 only (see [LICENSE](LICENSE)). The parts that
come from Xbox XBE Recomp keep its MIT license, and the libraries it uses keep theirs (both in
[THIRD_PARTY.txt](THIRD_PARTY.txt)). The game itself is not part of it.
