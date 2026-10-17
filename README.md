# Conker: Live & Reloaded — static recompilation

An unofficial native PC port of the original Xbox *Conker: Live & Reloaded*, with extended graphics options and other
enhancements. The game's x86 `default.xbe` is translated to C++ ahead of time; the Xbox kernel and XDK libraries are
implemented at the API level, and everything is drawn with Direct3D 12 and played through XAudio2. It's built on
[Xbox XBE Recomp](https://github.com/TGP482/XboxXBERecomp), vendored in `game/sdk/`.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of
> the game to install or build.

The game boots, plays through the frontend, cutscenes and levels, and adds 60/120 FPS, any aspect ratio, MSAA,
shader warm-up, keyboard and mouse controls, and skippable cutscenes.

## Installation

The release contains no game code: the game is built on your PC from your own copy, once. The recompiled game is a
translation of the original program, so sharing it would mean sharing the rights holders' code. What this project
shares is its own work, the recompiler, the runtime and the hooks, and Setup combines them with your copy on your
machine.

1. Put your game in the `assets` folder: the disc image (`.iso`), or `default.xbe` with the game's folders beside it.
   If you have the digital, backwards compatible release instead (`DefaultPackage` with a `DefaultPackage.data`
   folder), copy those two into `assets` and run `unpack-store-windows.exe` (on Linux, `./unpack-store-linux`). It
   unpacks the game's files into `assets` in a few seconds and needs about 5 GB free; afterwards the package can go.
   On Windows you can skip this step: if the Microsoft Store version is installed in its default place
   (`C:\XboxGames\Conker- Live and Reloaded\Content\Game`, not encrypted) and `assets` has no game, Setup finds it and
   asks whether to use it, then unpacks it for you. On Linux, if you bought the game on the Microsoft Store, you can
   download its package manually with the [Xodus project](https://github.com/xodus-gaming/xodus); put the
   `DefaultPackage` and `DefaultPackage.data` it gives you in `assets` and run `./unpack-store-linux`.
2. Run `setup-windows.exe` (on Linux, `./setup-linux` in a terminal opened in this folder). It checks your
   copy is the release this build supports, downloads the compiler it needs the first time only
   ([llvm-mingw](https://github.com/mstorsjo/llvm-mingw), 80 to 180 MB, on Windows; [Zig](https://ziglang.org), about
   50 MB, on Linux), recompiles the game and writes `Game.exe` (on Linux, the native `Game`). This takes a few
   minutes. Nothing of the game is uploaded or downloaded.
3. Run `Game.exe` (on Linux, `./Game`). Settings are in `settings.toml` beside it, and in the F1 menu.

Setup keeps nothing it generated except the game and the compiler in `kit/toolchain`. Run it again after updating
to a new release.

On Linux, `./Game` is the native port: an SDL3 window (X11 or Wayland) drawn with Vulkan, which needs a Vulkan 1.3
driver and glibc 2.31 or newer (any distro since 2020, SteamOS included). Add it to Steam as a non-Steam game to play
it from Game Mode. It has everything `Game.exe` has but the D3D12 renderer, the boot splash and the window kept up
across a relaunch. To build `Game.exe` instead, for Proton or Wine, run `./setup-linux --target windows`; it downloads
llvm-mingw then. If `./setup-linux` says permission denied, your unzip tool dropped its executable bit:
`chmod +x setup-linux kit/bin/xbe_recomp`. `setup-windows.exe` works under Proton too (`umu-run setup-windows.exe`).

Developers: building from source differs from Setup, see [Building from source](#building-from-source).

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
- **Vulkan Renderer (experimental)** - `renderer = "vulkan"` in `settings.toml` draws with Vulkan 1.3 instead of
  Direct3D 12, from the next launch. It draws the same way, from the same shaders, and shares `shaders.rec`. Without a
  Vulkan 1.3 GPU and driver the game starts on Direct3D 12 and says why in the log. `XBE_VULKAN_DEBUG=1` turns on the
  validation layer, when it is installed, and writes its messages to the log.

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
- **GPU** with support for Direct3D 12 (or Vulkan 1.3, for the Vulkan renderer)

## Repository layout

```
conker-lar/
  game/                 the port (CMake project)
    sdk/                the SDK, a git subtree of TGP482/XboxXBERecomp
    config/game.toml    recompiler config for Conker (hand-maintained)
    src/common/         app::kGame and shared headers
    src/game/           Conker hooks, frame rate / aspect, keyboard, mouse, F1 menu
    generated/ out/ assets/ winrun/ prefix/    gitignored: generated C++, builds, disc links, Proton run folder
  tools/                unpack-store (the digital release's package to game files), build.sh (Linux dev
                        build), package.cmake + package.sh/.bat (player release), toolchains.cmake (the pinned
                        llvm-mingw and Zig), fetch-toolchain.cmake, disassembly helpers
  toolchain/            gitignored: llvm-mingw and Zig, fetched on demand
```

## Two ways to get the game

There are two ways to end up with `Game.exe` or the native Linux `Game`, and both start from your own copy of the
game.

| | From a release (players) | From source (developers) |
|---|---|---|
| What you download | A small zip: `setup-windows.exe`, `setup-linux`, a kit | This repository |
| What your machine builds | The game's recompiled code only | Everything: recompiler, runtime, hooks, game code |
| What comes prebuilt | The runtime, hooks and recompiler | Nothing |
| Compiler | Pinned llvm-mingw (`Game.exe`) or Zig (Linux `Game`), downloaded by Setup | The same pinned llvm-mingw, downloaded by the first build; the native port uses the system clang |
| Time | A few minutes, once | Longer the first time, then incremental |
| Can you change the port | No | Yes |

A release's `Game.exe` and a Linux source build of the same version are built the same way: same compiler, same
flags, same link order. Crash addresses mean the same in both.

### Why releases work this way

The recompiled C++ is a mechanical translation of the game's `default.xbe`, including Microsoft's XDK libraries
linked into it. Shipping that code, or an exe compiled from it, would mean distributing the rights holders' code,
which copyright reserves to them. What this project writes is ours to share: the recompiler, the runtime (a
reimplementation of the Xbox and XDK APIs), the hooks, and the addresses and facts in the config. So a release ships
only that, and Setup turns your own copy into a program on your own machine. Emulators and decompilation projects
use the same model. (This isn't legal advice.)

The same rule covers the compiler. Setup downloads llvm-mingw or Zig from its official release, checks the file's
SHA-256, and builds with exactly the version the kit was made with. No copy of either is redistributed here.

It also explains two limits:
- **One release of the game.** The config, and the prebuilt runtime in a release, match one exact `default.xbe`
  by address. The supported one is the USA disc, SHA-1 `2232a1b3874d67b4bb4cf53a940420d2dab59535`. Setup refuses
  any other copy rather than build the wrong code, and the game looks for that exact XBE in `assets/`. The digital,
  backwards compatible release carries the same XBE and files, so it works too once unpacked (below).
- **Nothing generated is shared.** Never post `Game.exe`, anything from `game/generated/` or `kit/work/`, crash
  dumps, or files from the disc in issues or chats.

## Building from source

### What you need

- **Your copy of the game**, the supported release above, as a disc image (`.iso`), extracted files
  (`default.xbe` with `dvddata/` and `media/` beside it), or the digital release's package (`DefaultPackage` and its
  `DefaultPackage.data` folder), which `tools/unpack-store` unpacks (a Microsoft Store install keeps it in
  `C:\XboxGames\Conker- Live and Reloaded\Content\Game`; see Installation).
- **Linux:** `clang`/`clang++`, `lld`, `cmake` (3.25 or newer), `ninja`, `git`, `curl` and `zip`. Add `extract-xiso`
  to build from a disc image, and Wine or Proton to run `Game.exe`. The native port (`--linux`) also needs the X11 and
  Wayland development headers SDL builds against (SDL loads the libraries themselves at run time), and a Vulkan 1.3
  driver to run. `ndisasm` is only for `tools/xbe_disasm.py`.
- **Windows:** CMake (3.25 or newer), Ninja and Git. `build.bat` offers to install whichever is missing with
  `winget`. No Visual Studio: the compiler is the pinned llvm-mingw on Windows too.

### On Linux

```bash
tools/build.sh --iso path/to/disc.iso    # first build, from a disc image: extracts it into iso/
tools/build.sh --store path/to/package   # or from the digital release's DefaultPackage (or its folder): unpacks into iso/
tools/build.sh --game path/to/files      # or from files you already extracted
tools/build.sh                           # after a change: incremental
umu-run game/out/build/llvm-mingw/Game.exe
tools/build.sh --linux                   # also the native Linux port
game/out/build/release/Game
```

`--iso`, `--store` and `--game` link `game/assets/` to the game. You only need them once, or to switch to another copy; the
build notices the switch and recompiles. The first run downloads the pinned llvm-mingw into `toolchain/` and checks
its SHA-256, then recompiles the whole XBE and compiles the result, so it takes a while. Later builds redo only what
changed.

`Game.exe` finds `game/assets/` by walking up from its own folder. It keeps `settings.toml`, its logs, shader records
and saves beside itself, and so does the native `Game`. The native port is the same game in an SDL3 window, drawn
with Vulkan and played through SDL3's audio; it has everything `Game.exe` has but the D3D12 renderer, the boot splash
and the window kept up across a relaunch. This build uses the system clang and libstdc++; a release's native `Game` is
built with the pinned Zig instead (see [Building a release](#building-a-release)). `tools/build.sh --harness` also
builds `game/out/harness/conker`, a headless Linux build with no window or GPU, for testing.

### On Windows

1. Put the extracted game in `game\assets\`: `default.xbe` with `dvddata\` and `media\` beside it. (`build.bat`
   reads the files, not a disc image. `extract-xiso` has Windows builds.) From the digital release's package,
   `unpack-store` does the unpacking: build it with `cmake --build game\out\build\llvm-mingw --target unpack_store`
   after the first `build.bat`, then `game\out\build\llvm-mingw\unpack-store.exe path\to\DefaultPackage game\assets`.
2. Run `game\build.bat`. It checks for CMake, Ninja and Git, downloads llvm-mingw into `toolchain\` on the first
   run, recompiles the XBE and builds the port. Later runs rebuild only what changed.
3. Run `game\out\build\llvm-mingw\Game.exe`.

It's the same build as on Linux and in releases, with the same compiler and flags. `build.bat --pdb` also writes
`Game.pdb`, for debugging in Visual Studio or WinDbg; it makes builds slower, and
`cmake -DXBE_PDB=OFF game\out\build\llvm-mingw` turns it off again. (`tools/build.sh --pdb` does the same on Linux.)
IDEs that read `game\CMakePresets.json` get the same build as the `llvm-mingw` preset, once `build.bat` has fetched
the compiler.

## The digital release's package

The digital, backwards compatible release is a small `DefaultPackage` file and a `DefaultPackage.data` folder of
`Data0000`, `Data0001`, ... It's an Xbox 360 Games-on-Demand package: the disc's file system cut into 4 KiB blocks
with a table of hashes after every 204 of them. Nothing in it is encrypted, so `tools/unpack-store` only needs to
leave the hash tables out and find where the files start. `unpack_store.cpp` has the layout. It writes the same 881
files, byte for byte, as extracting the disc does, `default.xbe` included, so the supported-release rule above is
all there is to check: Setup still compares the XBE's SHA-1.

```bash
unpack-store DefaultPackage [outdir]     # outdir defaults to assets/ beside the program
tools/build.sh --store DefaultPackage    # builds it first, unpacks into iso/, builds the port
```

It is read only on the package, never touches keys or signatures, and takes about 5 GB of space and a few seconds.
Releases ship it as `unpack-store-windows.exe` and `unpack-store-linux`.

## Building a release

A release is `setup-windows.exe`, `setup-linux`, `unpack-store-windows.exe`, `unpack-store-linux` and a kit: the
recompiler, the prebuilt runtime and hooks for Windows and for Linux, the config, the headers the generated code
includes, and the pinned compilers' addresses and SHA-256s. It contains none of the game.

### On Linux

Put the supported release in `game/assets/` (any `tools/build.sh` above does that), then:

```bash
tools/package.sh                         # writes game/out/release/Conker-<version>.zip
```

It downloads the pinned llvm-mingw and Zig if they're missing (the pins are in `tools/toolchains.cmake`). It builds
the kit's Windows half with llvm-mingw, and with Zig `setup-linux` and the Linux `xbe_recomp` (static musl programs
that run on any x86_64 distro) and the kit's Linux half (`game/out/build/zig-glibc`): the native port's runtime,
against glibc 2.31 and Zig's own static libc++, which a player's Zig links the game code with into a `Game` that needs
only glibc. Its SDL is built against this system's X11, Wayland, ALSA, PulseAudio and PipeWire headers (it loads those
libraries at run time), which the native build needs anyway. Then it tests the release as a Linux player would: the
packaged `setup-linux` builds the native `Game`, then `Game.exe` (`--target windows`), from the game in
`game/assets/`, and the test fails unless both come out. `--no-test` skips the test, and `--test-download` makes it
use the compilers Setup downloads.

The kit only accepts the exact XBE it was packaged from, so package from the supported release.

### On Windows

You need the same CMake, Ninja and Git as for building (running `game\build.bat` once sets them up). Packaging
builds with the pinned llvm-mingw and Zig, which it downloads into `toolchain\`. Put the supported release in
`game\assets\`, then:

```bat
tools\package.bat
```

It writes `game\out\release\Conker-<version>.zip` and takes the same `--no-test`, `--test-download` and
`-j N` options. Here the test runs the packaged `setup-windows.exe`.

Zig builds `setup-linux` and the Linux `xbe_recomp` on Windows too, but the kit's Linux half needs Linux's headers: a
release packaged on Windows has none, and its `setup-linux` builds `Game.exe`.

Both scripts run `tools/package.cmake`, which can also be run directly: `cmake -P tools/package.cmake`, with
`-DTEST=OFF`, `-DTEST_DOWNLOAD=ON` or `-DJOBS=N` before `-P`.

## Credits

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp), the recompiler and runtime this is built on.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the overridable function layout and instruction hooks.
- [Zydis](https://github.com/zyantific/zydis) for decoding x86 instructions, [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (Clang, LLD, mingw-w64, libc++), the compiler `setup-windows.exe` builds the game with.
- [Zig](https://ziglang.org) (`zig c++`: Clang, LLD, glibc stubs, libc++), the compiler `setup-linux` builds the native game with.
