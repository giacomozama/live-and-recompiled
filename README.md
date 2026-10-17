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

## Building and running (Linux host)

Prerequisites: `clang`/`clang++`, `cmake`, `ninja`, `curl`, `zip`, `extract-xiso` to unpack a disc image, and
`ndisasm` for the disassembly tool. The Windows compiler, a pinned llvm-mingw, is downloaded into `toolchain/` the
first time it's needed.

```bash
tools/build.sh --iso path/to/disc.iso                 # extract into iso/, link game/assets, build Game.exe (long)
tools/build.sh --game /path/to/extracted              # or link game/assets to files you already extracted
tools/build.sh                                        # rebuild after a change (incremental)
tools/build.sh --harness                              # also the Linux headless harness, game/out/harness/conker
tools/package.sh                                      # player release: Setup.exe, setup-linux + kit, no game code
```

The exe is `game/out/build/llvm-mingw/Game.exe`, for Windows: run it with Proton or Wine (for example
`umu-run game/out/build/llvm-mingw/Game.exe`). It finds `game/assets/` by walking up from its own folder, and keeps
its settings, logs, shader records and saves beside itself.

On Windows the game repository builds natively with `game/build.bat` (Visual Studio with Clang), for developers
working on Windows. Players use the release's `Setup.exe` instead.

## SDK updates

SDK changes are ordinary commits here. Pull upstream with
`git subtree pull --prefix=game/sdk https://github.com/TGP482/XboxXBERecomp.git main`; send changes back with
`git subtree split --prefix=game/sdk`. Before vendoring, the SDK was the fork `giacomozama/XboxXBERecomp`, branch
`conker` (frozen at `a10407e`).
