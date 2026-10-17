# Conker: Live & Reloaded — static recompilation

An unofficial native PC port of the original Xbox *Conker: Live & Reloaded*. The game's x86 `default.xbe` is
translated to C++ ahead of time; the Xbox kernel and XDK libraries are implemented at the API level, and everything
is drawn with Direct3D 12 and played through XAudio2. It's built on
[Xbox XBE Recomp](https://github.com/TGP482/XboxXBERecomp), vendored in `game/sdk/`.

> This repository contains no game assets. The disc image and everything extracted from it are your own.

The game boots, plays through the frontend, cutscenes and levels, and adds 60/120 FPS, any aspect ratio, MSAA,
shader warm-up, keyboard and mouse controls, and skippable cutscenes. The player-facing feature list is in
[game/README.md](game/README.md).

## Layout

```
conker-lar/
  game/                 the port (CMake project)
    sdk/                the SDK, a git subtree of TGP482/XboxXBERecomp
    config/game.toml    recompiler config for Conker (hand-maintained)
    src/common/         app::kGame and shared headers
    src/game/           Conker hooks, frame rate / aspect, keyboard, mouse, F1 menu
    generated/ out/ assets/ winrun/ prefix/    gitignored: generated C++, builds, disc links, Proton run folder
  tools/                build.sh (Linux dev build), package.cmake + package.sh/.bat (player release),
                        toolchains.cmake (the pinned llvm-mingw and Zig), fetch-toolchain.cmake, disassembly
                        helpers
  toolchain/            gitignored: llvm-mingw, fetched on demand
```

Not in git, kept beside the checkout: the disc image, `iso/` (its extraction), `xemu/` (hardware reference
source), `toolchain/` (llvm-mingw, fetched on demand).

## Two ways to get Game.exe

There are two ways to end up with `Game.exe`, and both start from your own copy of the game. Either way the result
is a Windows program; on Linux it runs with Proton or Wine.

| | From a release (players) | From source (developers) |
|---|---|---|
| What you download | A small zip: `Setup.exe`, `setup-linux`, a kit | This repository |
| What your machine builds | The game's recompiled code only | Everything: recompiler, runtime, hooks, game code |
| What comes prebuilt | The runtime, hooks and recompiler | Nothing |
| Compiler | Pinned llvm-mingw, downloaded by Setup | The same pinned llvm-mingw, downloaded by the first build |
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

The same rule covers the compiler. Setup downloads llvm-mingw from its official release, checks the file's
SHA-256, and builds with exactly the version the kit was made with. No copy of it is redistributed here.

It also explains two limits:
- **One release of the game.** The config, and the prebuilt runtime in a release, match one exact `default.xbe`
  by address. The supported one is the USA disc, SHA-1 `2232a1b3874d67b4bb4cf53a940420d2dab59535`. Setup refuses
  any other copy rather than build the wrong code, and the game looks for that exact XBE in `assets/`.
- **Nothing generated is shared.** Never post `Game.exe`, anything from `game/generated/` or `kit/work/`, crash
  dumps, or files from the disc in issues or chats.

## Building from source

### What you need

- **Your copy of the game**, the supported release above, as a disc image (`.iso`) or extracted files
  (`default.xbe` with `dvddata/` and `media/` beside it).
- **Linux:** `clang`/`clang++`, `cmake` (3.25 or newer), `ninja`, `curl` and `zip`. Add `extract-xiso` to build from a
  disc image, and Wine or Proton to run the result. `ndisasm` is only for `tools/xbe_disasm.py`.
- **Windows:** CMake (3.25 or newer), Ninja and Git. `build.bat` offers to install whichever is missing with
  `winget`. No Visual Studio: the compiler is the pinned llvm-mingw on Windows too.

### On Linux

```bash
tools/build.sh --iso path/to/disc.iso    # first build, from a disc image: extracts it into iso/
tools/build.sh --game path/to/files      # or from files you already extracted
tools/build.sh                           # after a change: incremental
umu-run game/out/build/llvm-mingw/Game.exe
```

`--iso` and `--game` link `game/assets/` to the game. You only need them once, or to switch to another copy; the
build notices the switch and recompiles. The first run downloads the pinned llvm-mingw into `toolchain/` and checks
its SHA-256, then recompiles the whole XBE and compiles the result, so it takes a while. Later builds redo only what
changed.

`Game.exe` finds `game/assets/` by walking up from its own folder. It keeps `settings.toml`, its logs, shader records
and saves beside itself. `tools/build.sh --harness` also builds `game/out/harness/conker`, a headless Linux build with
no window or GPU, for testing.

### On Windows

1. Put the extracted game in `game\assets\`: `default.xbe` with `dvddata\` and `media\` beside it. (`build.bat`
   reads the files, not a disc image. `extract-xiso` has Windows builds.)
2. Run `game\build.bat`. It checks for CMake, Ninja and Git, downloads llvm-mingw into `toolchain\` on the first
   run, recompiles the XBE and builds the port. Later runs rebuild only what changed.
3. Run `game\out\build\llvm-mingw\Game.exe`.

It's the same build as on Linux and in releases, with the same compiler and flags. `build.bat --pdb` also writes
`Game.pdb`, for debugging in Visual Studio or WinDbg; it makes builds slower, and
`cmake -DXBE_PDB=OFF game\out\build\llvm-mingw` turns it off again. (`tools/build.sh --pdb` does the same on Linux.)
IDEs that read `game\CMakePresets.json` get the same build as the `llvm-mingw` preset, once `build.bat` has fetched
the compiler.

## Building a release

A release is `Setup.exe`, `setup-linux` and a kit: the recompiler, the prebuilt runtime and hooks, the config, the
headers the generated code includes, and the pinned compiler's address and SHA-256. It contains none of the game.

### On Linux

Put the supported release in `game/assets/` (any `tools/build.sh` above does that), then:

```bash
tools/package.sh                         # writes game/out/release/Conker-<version>.zip
```

It downloads the pinned llvm-mingw and Zig if they're missing (the pins are in `tools/toolchains.cmake`). It builds
the kit with llvm-mingw, and `setup-linux` and the Linux `xbe_recomp` with Zig: static musl programs that run on any
x86_64 distro. Then it tests the release as a Linux player would: the
packaged `setup-linux` builds `Game.exe` from the game in `game/assets/`, and the test fails unless one comes out.
`--no-test` skips the test, and `--test-download` makes it use the compiler Setup downloads.

The kit only accepts the exact XBE it was packaged from, so package from the supported release.

### On Windows

You need the same CMake, Ninja and Git as for building (running `game\build.bat` once sets them up). Packaging
builds with the pinned llvm-mingw and Zig, which it downloads into `toolchain\`. Put the supported release in
`game\assets\`, then:

```bat
tools\package.bat
```

It writes `game\out\release\Conker-<version>.zip` and takes the same `--no-test`, `--test-download` and
`-j N` options. Here the test runs the packaged `Setup.exe`.

Zig builds `setup-linux` and the Linux `xbe_recomp` on Windows too, so a release is the same whichever system
packaged it.

Both scripts run `tools/package.cmake`, which can also be run directly: `cmake -P tools/package.cmake`, with
`-DTEST=OFF`, `-DTEST_DOWNLOAD=ON` or `-DJOBS=N` before `-P`.
