# Live & Recompiled

An unofficial native PC port of the original Xbox *Conker: Live & Reloaded*, built on a heavily modified version of
[Xbox XBE Recomp](https://github.com/TGP482/XboxXBERecomp). Currently supports Linux and Windows.

This project is not affiliated with or endorsed by Rare or Microsoft. *Conker* and *Conker: Live & Reloaded* are
trademarks of their respective owners.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own copy of
> the game. The game [can currently be purchased from the Windows Store](https://www.xbox.com/games/store/conker-live-and-reloaded/bvfb8cbs75r6). The Windows Store version is fully compatible with this project.

> [!WARNING]
> **AI disclaimer**
>
> ALL first-party code here is LLM generated. It was tested manually, but pretty much no code at all was typed in by humans. I think it works well, but it IS one-hundred percent, unadulterated, "vibecoded slop".
> This was not made to gain recognition or to showcase my own skills, it was made because I wanted an enjoyable Conker: L&R experience on PC and, while I understand and even partially share concerns surrounding LLM coding, I also recognize that implementing a project like this manually would take *years* of effort and dedication while producing nothing humanity hasn't already done before and not even guaranteeing a superior final product. I don't see the value in that for such a niche game, but I welcome and encourage you to try and implement your own, manually coded version if you truly care about one existing.

## Status and Known Bugs

This is a beta: single player is supposed to be fully playable, but there may be uncaught bugs and/or edge cases. Playtesting and bug reports are very appreciated.

Modes above 60 FPS WILL have frame interpolation glitches, as we have not achieved 100% draw matching yet.

No effort has been made to implement multiplayer yet.

## Contributing

You may open issues for requesting features or reporting bugs, and submit PRs if you fix a bug or want to add a new feature yourself. There is no guarantee it will be merged, but you won't be laughed at either. We DO accept LLM-written PRs obviously.

**Bug reporting advice**

If you notice a bug, you can take a frame capture by pressing F3. It will be saved in a subdirectory of the `cap` directory. *Please remove the `tex_XXXXXXX.bmp` and `.hlsl` files from the capture directory*, as they come from the game itself and must not be submitted. Then zip the directory and attach it to your bug report.

If you notice frame interpolation glitches at high FPS, run the game with `XBE_INTERP_RECORD=120` (or whichever framerate you are playing at) and press F4 *immediately after* experiencing the glitch. Captures are generated inside the `cap` directory with the `interp_` prefix. Zip and attach the generated directory to your bug report.

## Features

- **High frame rates** - 60, 120, 180 or 240 FPS instead of the original 30 (experimental).
- **Any aspect ratio** - Ultrawide and other displays get a wider view, with the HUD stretched to fit.
- **Better graphics** - Anti-aliasing, additional post-processing effects, improved fur, flowing and rippling water, 
shiny armour, see-through glass and swaying distant grass.
- **Controls** - Xbox, PlayStation and Switch controllers with rumble, or keyboard and mouse with mouse look; all
  rebindable.
- **Skippable cutscenes** - Skip any in-engine cutscene without having seen them once first.

Everything is configurable in the Esc menu and in `settings.toml`.

## Installation

For legal reasons, releases contain none of the game. Setup performs the entire extraction and recompilation process on your PC, which might require several minutes.

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
     - **Linux:** if you bought it on the Microsoft Store, download its package with the
       [Xodus project](https://github.com/xodus-gaming/xodus) or copy it from a Windows install.
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
2. **Run `Game.exe`** (Windows) **or `./Game`** (Linux), in the game folder (`game` beside Setup). Settings are in `settings.toml` beside it, and in the Esc menu.

Setup keeps nothing it generated except the game, and the compiler in the working folder (unless you chose not to). Run it again after updating
to a new release.

## Building from source

See [BUILDING.md](BUILDING.md).

## Credits

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBERecomp), the recompiler and runtime this is built on.
- [xemu](https://github.com/xemu-project/xemu) and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) as references for the Xbox's hardware and API behaviour. This project does not use emulation or any emulator code.
- [SMAA](http://www.iryoku.com/smaa/) by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez (MIT), for the anti-aliasing effect.
- [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay, [FLTK](https://www.fltk.org) for Setup's window and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.
- [Zig](https://ziglang.org) (`zig c++`: Clang, LLD, mingw-w64 and glibc stubs, libc++), the compiler Setup builds the game with, on both systems.

## License

This project is licensed under the GNU General Public License, version 3 only (see [LICENSE](LICENSE)). The parts that
come from Xbox XBE Recomp keep its MIT license, and the libraries it uses keep theirs (both in
[THIRD_PARTY.txt](THIRD_PARTY.txt)). The game itself is not part of it.
