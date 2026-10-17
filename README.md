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
  tools/                build.sh (dev build), package.sh (player release), fetch-toolchain.sh (pinned llvm-mingw),
                        disassembly helpers
  toolchain/            gitignored: llvm-mingw, fetched on demand
```

Not in git, kept beside the checkout: the disc image, `iso/` (its extraction), `xemu/` (hardware reference
source), `toolchain/` (llvm-mingw, fetched on demand).

## Building and running

The port builds on Linux. The result is a Windows executable, `Game.exe`, which you run with Proton or Wine.

### What you need

- **Your copy of the game.** The build supports one release: the USA disc, whose `default.xbe` has SHA-1
  `2232a1b3874d67b4bb4cf53a940420d2dab59535`. Other dumps or modified copies won't match the config.
- **Tools:** `clang`/`clang++`, `cmake` (3.25 or newer), `ninja`, `curl` and `zip`. Add `extract-xiso` to build from
  a disc image, and `umu-launcher` (`umu-run`) or Wine to run the result. `ndisasm` is only for
  `tools/xbe_disasm.py`.

The Windows compiler, a pinned [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), is downloaded into
`toolchain/` on the first build and checked against its SHA-256.

### Build

```bash
tools/build.sh --iso path/to/disc.iso    # first build, from a disc image: extracts it into iso/
tools/build.sh --game path/to/files      # or from files you already extracted (default.xbe, dvddata/, media/)
tools/build.sh                           # after a change: incremental
```

`--iso` and `--game` link `game/assets/` to the game; you only need them once, or to switch to another copy, which
the build notices and recompiles for. The first build recompiles the whole XBE and compiles the result, so it
takes a while. After that only what changed is rebuilt.

### Run

```bash
umu-run game/out/build/llvm-mingw/Game.exe
```

`Game.exe` finds `game/assets/` by walking up from its own folder. It keeps `settings.toml`, its logs, shader records
and saves beside itself.

### Headless harness

```bash
tools/build.sh --harness                 # also builds game/out/harness/conker, a Linux build with no window or GPU
```

### Player release

```bash
tools/package.sh                         # game/out/release/Conker-<version>.zip
```

Releases carry no game code. A release is `Setup.exe` and `setup-linux` plus a kit of everything except the
recompiled game. Each player builds `Game.exe` on their own machine from their own copy. `package.sh` builds the
kit from the game in `game/assets/` and tests it by running the packaged `setup-linux` against that game.
`--no-test` skips the test, and `--test-download` makes it use the compiler Setup downloads. Package from the
supported release: the kit only accepts the exact XBE it was made from.

### On Windows

`game/build.bat` builds natively with Visual Studio's Clang tools, for working on the port on Windows. Players use
the release's `Setup.exe` instead.
