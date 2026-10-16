# Conker: Live & Reloaded — static recompilation

An unofficial native PC port of the original Xbox *Conker: Live & Reloaded*. The game's x86 `default.xbe` is
translated to C++ ahead of time; the Xbox kernel and XDK libraries are implemented at the API level, and everything
is drawn with Direct3D 12 and played through XAudio2. It's built on
[Xbox XBE Recomp](https://github.com/TGP482/XboxXBERecomp), vendored in `game/sdk/`.

> This repository contains no game assets. The disc image and everything extracted from it are your own.

The game boots, plays through the frontend, cutscenes and levels, and adds 60/120 FPS, any aspect ratio, MSAA,
shader warm-up, keyboard and mouse controls, and skippable cutscenes. The player-facing feature list is in
[game/README.md](game/README.md); open problems are in [docs/conker.md](docs/conker.md#known-issues).

## Documentation

| | |
|---|---|
| [docs/architecture.md](docs/architecture.md) | How it fits together: build pipeline, execution model, the graphics model, audio, host app |
| [docs/making-changes.md](docs/making-changes.md) | Where a change goes, recipes (hooks, settings, NV2A methods), rules that have caused bugs |
| [docs/conker.md](docs/conker.md) | The game: config notes, every hook, engine addresses and facts, known issues |
| [docs/debugging.md](docs/debugging.md) | Proton vs harness, F3 captures, environment variables, profiler, tools |

## Layout

```
conker-lar/
  game/                 the port (CMake project)
    sdk/                the SDK, a git subtree of TGP482/XboxXBERecomp
    config/game.toml    recompiler config for Conker (hand-maintained)
    src/common/         app::kGame and shared headers
    src/game/           Conker hooks, frame rate / aspect, keyboard, mouse, F1 menu
    generated/ out/ assets/ winrun/ prefix/    gitignored: generated C++, builds, disc links, Proton run folder
  rebuild.sh            ISO -> extracted disc -> tools -> config -> recompiled C++
  run-proton.sh         build the Windows exe and launch it under GE-Proton
  tools/                XBE / vertex-program disassembly helpers
  docs/
```

Not in git, kept beside the checkout: the disc image, `iso/` (its extraction), `xemu/` (hardware reference
source), `clr_unpack/`, `60fps/`.

## Building and running (Linux host)

Prerequisites: `extract-xiso`, `clang`/`clang++`, `cmake`, `ninja`, `lld`/`llvm-ar`, `mingw-w64` (x86_64, posix
threads), `ndisasm` for the disassembly tool, and for running the Windows build **GE-Proton** plus
**umu-launcher** (`umu-run`). Neither is part of the repository; install them separately.

```bash
./rebuild.sh --iso "Conker Live and Reloaded.iso"   # extract, build xbe_recomp, recompile (first run is long)
./run-proton.sh                                     # build game/out/build/win/Game.exe, copy to game/winrun, launch
./run-proton.sh --skip-fmv                          # same, skipping the intro movies
./rebuild.sh --harness                              # Linux headless harness (see docs/debugging.md)
```

`run-proton.sh` runs the `umu-run` on `PATH` (or `UMU=/path/to/umu-run`) and reads `PROTONPATH` (default
`~/.local/share/Steam/compatibilitytools.d/GE-Proton11-7`), `GAMEID` and `JOBS`. The Windows exe finds `assets/` by walking up from its own folder, so it runs straight from
`game/winrun/`; its settings (`settings.toml`), logs, shader records and saves live there too.

On Windows the game repository builds natively with `game/build.bat` (Visual Studio with Clang), as
[game/README.md](game/README.md) describes.

## SDK updates

SDK changes are ordinary commits here. Pull upstream with
`git subtree pull --prefix=game/sdk https://github.com/TGP482/XboxXBERecomp.git main`; send changes back with
`git subtree split --prefix=game/sdk`. Before vendoring, the SDK was the fork `giacomozama/XboxXBERecomp`, branch
`conker` (frozen at `a10407e`).
