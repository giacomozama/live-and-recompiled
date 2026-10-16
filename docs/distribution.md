# Distribution

Releases never contain the game's code. `Game.exe` is built on each player's machine, from their own copy of the
game, by `Setup.exe`. This page explains why, what a release contains, and how to make one.

## Why

The recompiled C++ is a mechanical translation of the XBE, Microsoft's XDK libraries included. Distributing it
(or an exe compiled from it) means distributing the rights holders' code, which copyright reserves to them. That
holds in the US (17 U.S.C. §106) and in the EU (Software Directive Art. 4). It's also the easiest thing to take down.
What we write, the recompiler, the runtime (a reimplementation of the Xbox and XDK APIs), the hooks and the
addresses and facts in the config, is ours to distribute. Players turning their own copy into a program on their
own machine is a much stronger position: in the US that's an owner's adaptation for use (§117(a)), and in the EU a
lawful user's adaptation (Art. 5(1)). It's the same model emulators and decompilation projects follow. (Not legal
advice.)

Two other rules follow from this. The tools never circumvent copy protection and never ship keys; players use a disc
image or files they already have. And nothing in the project tells anyone where to get the game.

## What a release contains

```
Conker/
  Setup.exe            builds Game.exe here from the game in assets/
  README.md            the player README (game/README.md)
  THIRD_PARTY.txt
  assets/              empty: the player's disc image or game files go here
  kit/
    kit.toml           the release's SHA-1, the compile and link lines
    bin/xbe_recomp.exe the recompiler
    config/, xdk/      game.toml and the XDK tables: addresses, layouts, signature hashes
    include/           cpu/x86_ops.h, cpu/x86_context.h, image_symbols.h (addresses)
    lib/               libGame_host.a (runtime + hooks + ImGui), libSDL3.a, libxaudio2.a
    toolchain/         llvm-mingw (Windows-hosted, x86_64 only): clang, lld, headers, libraries
```

**Never put in a release, an issue or a chat:** `Game.exe`, anything from `game/generated/` or `kit/work/`, object
files built from them, crash dumps (`CrashDumps/*.dmp` hold the running game), `shader_programs.bin` (the game's
vertex programs), F3 captures with textures, or any file from the disc. `shaders.rec` is fine (hashes and settings).

## What Setup does

1. Finds `assets/` (beside it or in a folder above), and in it `default.xbe` or a `.iso` with it on the disc.
2. Checks the XBE's SHA-1 against `kit.toml`: the prebuilt host is only valid for that exact release.
3. Runs `xbe_recomp` into `kit/work/generated`, then checks the `image_symbols.h` it wrote is byte-identical to the
   one the host library was compiled against.
4. Compiles every generated file with the kit's clang, in parallel (biggest first), at below-normal priority.
5. Links `Game.exe` with the host library as a whole archive. The order matches the SDK's own build, so a player's
   exe has the same code layout as the developers' build of the same version, and crash addresses line up.
6. Deletes `kit/work` (`--keep` leaves it) and writes `setup.log`.

Measured on a 16-thread machine: about 50 s in total, with 9.8 CPU-minutes of compiling. A 6–8-thread PC should take
2–3 minutes.

## How the pieces fit in the build

- `add_xbe_image` (SDK CMake) builds `<target>_host`, a static library of everything except the generated code, and
  links the exe from it plus the generated objects. The dev build and the player build link the same way.
- `add_xbe_player_kit` (MinGW builds only) stages the release into `out/build/llvm-mingw/package/Game`: `Setup.exe`,
  `kit.toml` with the exact flags, the libraries and the headers.
- The compiler is **llvm-mingw**, pinned in `tools/fetch-toolchain.sh` with SHA-256 checks. The Linux-hosted build
  compiles the host library; the Windows-hosted build of the same release goes in the kit. `run-proton.sh` uses the
  same toolchain and build folder, so what you test is what players build.

## Making a release

```bash
tools/package.sh            # fetch toolchains, build the kit, test it, zip it
```

1. **Toolchains.** It downloads both llvm-mingw builds into `toolchain/` if they're missing.
2. **Kit.** It builds `Game_kit` in `game/out/build/llvm-mingw`, plus the native `xbe_recomp` and `xbe_setup`.
3. **Staging.** It copies the kit into `game/out/release/Conker/` and adds a trimmed Windows toolchain: the clang and
   lld executables and the DLLs they load, headers without `.idl`, x86_64 libraries and builtins. That's about 310 MB
   unpacked.
4. **Test.** It runs the player's steps on Linux (native `Setup`, Linux-hosted llvm-mingw of the same release) against
   `game/assets`, and fails unless a `Game.exe` comes out and `kit/work` is gone. `--no-test` skips it.
5. **Zip.** It writes `game/out/release/Conker-<git describe>.zip` (about 77 MB).

Before publishing, run the Windows-hosted kit once for real: on Windows, or with `umu-run Setup.exe` in an unpacked
copy. The automatic test can't run the Windows-hosted compiler.

## Updating the toolchain

Change `VERSION` and both SHA-256s in `tools/fetch-toolchain.sh` (the release page lists the digests), and
`LLVM_MINGW_VERSION` in `game/cmake-llvm-mingw.cmake`. Then reconfigure `game/out/build/llvm-mingw` from scratch.
Players need a new release, since the host library and the kit's compiler must match.
