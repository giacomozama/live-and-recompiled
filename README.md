<h1 align="center">Live & Recompiled</h1>

<p align="center">
  <img src=".github/logo.svg" alt="Live &amp; Recompiled" width="400">
  <br/>
  <em>I assume pareidolia qualifies as fair use.</em>
</p>
<br/>

An unofficial *Conker: Live & Reloaded* native recompilation and enhancement project for Linux and Windows, and
experimentally [Android](#android).

This project is **NOT** affiliated with or endorsed by Rare or Microsoft. *Conker* and *Conker: Live & Reloaded* are
trademarks of their respective owners.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own copy of
> the game. The game [can currently be purchased from the XBOX Store](https://www.xbox.com/games/store/conker-live-and-reloaded/bvfb8cbs75r6). The XBOX Store version is fully compatible with this project.

> [!WARNING]
> **AI disclaimer**  
> ALL first-party code here is LLM-generated and was NOT reviewed line-by-line.

## Features

- **High frame rates** - 60, 120, 180 or 240 FPS instead of the original 30.
- **Any aspect ratio** - Play the game at any aspect ratio.
- **Better graphics** - Vulkan rendering at any resolution (with Direct3D 11 and OpenGL renderers as options for weaker machines), SMAA or MSAA anti-aliasing, AMD FSR 1.0 upscaling and sharpening, NVIDIA DLSS on RTX cards, per-pixel lighting, ambient occlusion, smooth baked shadows, dynamic water, motion blur, HDR10 output, and toggles for fog, vignette and more.
- **Redone fur** - Optional strand-based fur with hair lighting, tuned per character.
- **Controls** - Full gamepad and keyboard + mouse support, with key prompts in place of button icons, mouse aim for scopes, aircraft and the Sneeker's drone, rebindable Walk and Creep keys, adjustable field of view and extra look up.
- **Multiplayer** - Full multiplayer support, with local split-screen, LAN autodiscovery and direct IP over the internet. It adds text chat, a Friendly Fire match option, player names, outlines, health bars and rank emblems, netcode fixes, a visible medgun beam, high quality character models, and color-blind-friendly blue / orange team colors.
- **Achievements** - Local achievements, kept with your saves.
- **Mods** - Convenient, plug-and-play modding layer to easily install mods, such as the [Decensor Patch/Mod](https://github.com/nastys/conker-live-and-reloaded-decensor) by nastys.
- **Quality of life** - Bypass loading screens and skip any cutscene without having seen it once first, by holding any button. Optional English speech bubbles in the throne monologues.
- **Fixes to the original game** - Fixed animation and rendering glitches and improved rope physics.
- **Settings menu** - A comprehensive settings menu worthy of a PC game, translated into the game's languages, with a first-launch welcome screen offering graphics presets.

The settings are in the Esc menu and in `settings.toml`.

Settings and saves live in `%APPDATA%\LiveAndRecompiled` on Windows and in `~/.config/LiveAndRecompiled` and
`~/.local/share/LiveAndRecompiled` on Linux; logs and caches in `%LOCALAPPDATA%\LiveAndRecompiled`, or
`~/.local/state` and `~/.cache` on Linux. Put an empty `portable.txt` beside the game's executable to keep them all
in its folder instead.

## Status

The recomp is fully implemented, both single and multiplayer.

That does not mean there won't be bugs or that no more features can be added. Bug reports and feature requests are very much appreciated.

See [KNOWN_ISSUES.md](KNOWN_ISSUES.md) for the known issues.

## Installation

Releases contain neither the game's code nor its assets. Setup performs the entire extraction and recompilation process on your PC, which might require several minutes.

**Supported versions**
- The USA disc release (`default.xbe` SHA-1 = `2232a1b3874d67b4bb4cf53a940420d2dab59535`)
- The digital XBOX Backwards Compatibility release from the XBOX Store (equivalent to the USA version).

Download the release for your system, `LiveAndRecompiled-<version>-windows-x86_64.zip` or `LiveAndRecompiled-<version>-linux-x86_64.tar.gz`, and unpack it.
Then it is two steps: run Setup, run the game.

1. **Run Setup**: `setup-windows.exe` on Windows, or `./setup-linux` on Linux (double-click it, or run it in a terminal).
   Its window asks for three things; the last two are already filled in:
   - **The game**: your `DefaultPackage` (the digital XBOX Backwards Compatibility release, with its
     `DefaultPackage.data` folder beside it), your disc image (`.iso`), or the folder with the extracted files
     (`default.xbe` with the game's folders beside it). Setup unpacks a package (a few seconds; it needs about 5 GB free) and
     puts the game's files in an `assets` folder in the game folder, where the game looks for them.
     - **Windows:** if it is installed from the XBOX Store in its default place
       (`C:\XboxGames\Conker- Live and Reloaded\Content\Game`, not encrypted), pick that folder.
       - **NOTE:** You have to run the original app and let it extract the game once first, otherwise the files will not decode correctly.
     - **Linux:** if you bought it on the XBOX Store, download its package with the
       [Xodus project](https://github.com/xodus-gaming/xodus) or copy it from a Windows install. You can use streaming mode to extract the files automatically.
   - **Working folder**: `work` beside Setup by default (made if it's missing). The compiler that comes inside Setup is unpacked into it (once) and the
     game is built there. Setup removes the temporary files and the unpacked compiler when done.
   - **Game folder**: `game` beside Setup by default; where the game is made, with the game's files in an `assets` folder inside it.

   Press Start. Setup checks your copy is a supported version, unpacks the compiler (first time only), recompiles
   the game and writes the executable. This takes a few minutes. Nothing of the game is uploaded.
   - **Windows:** builds with the [Zig](https://ziglang.org) inside Setup and writes `Game.exe`.
   - **Linux:** builds with the [Zig](https://ziglang.org) inside Setup and writes the native `Game`. If it says permission
     denied, your archive tool dropped the executable bit: `chmod +x setup-linux`. Setup itself needs only glibc; its
     window needs your desktop's own libraries, which a desktop already has: on Wayland, Wayland, xkbcommon, Cairo and
     Pango; on X11, X11 and Xft. If they're missing, Setup says "no wayland window" or "no x11 window" in the terminal
     and builds there instead (`--cli`, below). To get the window on a minimal system: on Debian or Ubuntu, `sudo apt
     install libwayland-client0 libwayland-cursor0 libxkbcommon0 libcairo2 libpango-1.0-0 libpangocairo-1.0-0` (Wayland)
     or `libx11-6 libxft2` (X11); on Arch, `sudo pacman -S wayland libxkbcommon cairo pango` or `libx11 libxft`.

   Without a display, or with any option, Setup runs in the terminal instead: `./setup-linux --cli --source
   path/to/DefaultPackage --work /tmp/conker-work --game-dir ~/Games/Conker`. `--source` is required and the other
   options default to the same as the window (`--help` lists them).
2. **Run `Game.exe`** (Windows) **or `./Game`** (Linux), in the game folder chosen during Setup.

Setup keeps nothing it generated except the game. Run it again after updating to a new release.

## Android

> [!NOTE]
> The Android version is experimental and hasn't been tested on a device yet.

The app is built on your PC, like the PC version, and the game's files are copied onto the device separately.

**What you need:** a 64-bit ARM device with Android 9 or newer and a Vulkan 1.3 GPU that has push descriptors
(recent Snapdragon, Dimensity, Exynos or Tensor phones and tablets), about 6 GB of free space on it, and your copy
of the game (any of the supported versions above). A controller is recommended; without one, controls are drawn on
the screen.

1. **Build the app** on Windows or Linux: download `LiveAndRecompiled-<version>-android-builder-windows.zip` or
   `-linux.tar.gz`, unpack it and run `apk-builder-windows.exe` or `./apk-builder-linux`. Pick your copy of the game
   (only its `default.xbe` is read on the PC) and press Build APK. It downloads [Zig](https://ziglang.org) the first
   time (about 100 MB), recompiles the game for Android and writes `LiveAndRecompiled.apk`, signed with a key it makes
   for your PC (`android-signing.toml` in `%APPDATA%\LiveAndRecompiled` or `~/.config/LiveAndRecompiled`). Keep that
   file: Android only installs a new build over the old one when the same key signed both. Without a display, or with
   `--cli`, it runs in the terminal (`--help` lists the options).
2. **Install it**: copy the APK to the device and open it (allow installing from that app when asked), or
   `adb install LiveAndRecompiled.apk`.
3. **Copy the game to the device**: the disc image (`.iso`), the folder with the extracted files, or the Xbox Store
   release's `DefaultPackage` with its `DefaultPackage.data` folder.
4. **Open the app** and pick the game: "Choose a disc image" for an `.iso`, "Choose a folder" for the rest. It checks
   the version and unpacks the game's files into the app's own storage (a few minutes), then starts the game. You can
   delete the copy you picked afterwards.

The back button opens the menu. Settings and saves stay in the app's storage; logs are in
`Android/data/org.liveandrecompiled.conker/files/logs` on the device's storage. To use another copy of the game,
clear the app's storage in Android's settings (which deletes the saves too).

## Contributing

When reporting bugs, provide as much information as you can: a detailed description, your recomp settings and version, screenshots, your OS and hardware.

New feature suggestions and/or PRs are also welcome.

## Building from source

See [BUILDING.md](BUILDING.md).

## Credits

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBERecomp), the recompiler and runtime this was originally built on.
- [xemu](https://github.com/xemu-project/xemu) and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) as references for the Xbox's hardware and API behavior.
- [SMAA](http://www.iryoku.com/smaa/) by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez (MIT).
- [toml++](https://github.com/marzer/tomlplusplus) for the configs.
- [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay.
- [FLTK](https://www.fltk.org) for the Setup UI.
- [micro-ecc](https://github.com/kmackay/micro-ecc) for signing the Android app.
- [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.
- [Zig](https://ziglang.org) (`zig c++`: Clang, LLD, mingw-w64 and glibc stubs, libc++), the compiler Setup builds the game with, on both systems.
- [The ConkerXbox60FPSWidescreen patch](https://www.moddb.com/downloads/conkerxbox60fpswidescreen) for important information about game code.
- [Rare Limited](https://www.rare.co.uk/) for creating the original game.

## License

This project is licensed under the GNU General Public License, version 3 only (see [LICENSE](LICENSE)). The parts that
come from Xbox XBE Recomp keep its MIT license, and the libraries it uses keep theirs (both in
[THIRD_PARTY.txt](THIRD_PARTY.txt)). The game itself is not part of it.

### Additional permission under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or combining it with the NVIDIA DLSS SDK (or a modified
version of that library), containing parts covered by the terms of the NVIDIA RTX SDKs License, the licensors of this
Program grant you additional permission to convey the resulting work.

The DLSS support uses this: the DLSS module (`src/dlss`, built as `xbe_dlss.dll` and `libxbe_dlss.so`) links NVIDIA's
NGX library, and NVIDIA's DLSS library ships beside the game. Both of NVIDIA's are under NVIDIA's own terms
(`NVIDIA_RTX_SDKS_LICENSE.txt` in the release), not this project's.
