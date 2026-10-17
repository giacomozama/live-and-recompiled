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
  Setup.exe            builds Game.exe here from the game in assets/ (on Windows, or under Wine)
  setup-linux          the same, natively on Linux and SteamOS (static, any x86_64 distro)
  README.md            the player README (game/README.md)
  THIRD_PARTY.txt
  assets/              empty: the player's disc image or game files go here
  kit/
    kit.toml           the release's SHA-1, the compile and link lines, each host's compiler URL and SHA-256
    bin/               the recompiler: xbe_recomp.exe for Setup.exe, xbe_recomp for setup-linux
    config/, xdk/      game.toml and the XDK tables: addresses, layouts, signature hashes
    include/           cpu/x86_ops.h, cpu/x86_context.h, image_symbols.h (addresses)
    lib/               libGame_host.a (runtime + hooks + ImGui), libSDL3.a, libxaudio2.a
```

The compiler isn't in the release either. On its first run Setup downloads the llvm-mingw that `kit.toml` pins
for the host it runs on (`[toolchain.windows]` or `[toolchain.linux]`), and unpacks it into `kit/toolchain/<host>/`:
clang, lld, headers, x86_64 libraries and builtins. The Windows one is about 180 MB to download and 380 MB unpacked,
the Linux one 80 MB and 355 MB. Both hosts build the same `Game.exe`; on Linux it then runs under Proton.

**Never put in a release, an issue or a chat:** `Game.exe`, anything from `game/generated/` or `kit/work/`, object
files built from them, crash dumps (`CrashDumps/*.dmp` hold the running game), `shader_programs.bin` (the game's
vertex programs), F3 captures with textures, or any file from the disc. `shaders.rec` is fine (hashes and settings).

## What Setup does

1. Finds `assets/` (beside it or in a folder above), and in it `default.xbe` or a `.iso` with it on the disc.
2. Checks the XBE's SHA-1 against `kit.toml`: the prebuilt host is only valid for that exact release.
3. Unless `kit/toolchain/<host>/` already holds it, downloads the llvm-mingw archive `kit.toml` names for this host
   and checks its SHA-256. Setup.exe uses WinHTTP, so it works under Wine too, and unpacks the `.zip` with miniz.
   setup-linux uses `curl` and unpacks the `.tar.xz` with `tar`. Either way it keeps only what an x86_64 build needs.
   `sha256.txt` in that folder records which archive is unpacked; a mismatch downloads again.
   `--fetch-toolchain` does only this step.
4. Runs `xbe_recomp` into `kit/work/generated`, then checks the `image_symbols.h` it wrote is byte-identical to the
   one the host library was compiled against.
5. Compiles every generated file with the kit's clang, in parallel (biggest first), at below-normal priority.
6. Links `Game.exe` with the host library as a whole archive. The order matches the SDK's own build, so a player's
   exe has the same code layout as the developers' build of the same version, and crash addresses line up.
7. Deletes `kit/work` (`--keep` leaves it) and writes `setup.log`. The toolchain stays for the next run.

Measured on a 16-thread machine, not counting the download (Windows-hosted compiler): about 50 s in total, with 9.8 CPU-minutes of
compiling. A 6–8-thread PC should take 2–3 minutes.

## How the pieces fit in the build

- `add_xbe_image` (SDK CMake) builds `<target>_host`, a static library of everything except the generated code, and
  links the exe from it plus the generated objects. The dev build and the player build link the same way.
- `add_xbe_player_kit` (MinGW builds only) stages the release into `out/build/llvm-mingw/package/Game`: `Setup.exe`,
  `kit.toml` with the exact flags, the libraries and the headers.
- The compiler is **llvm-mingw**, pinned in `tools/fetch-toolchain.sh` with SHA-256 checks. The Linux-hosted build
  compiles the host library; `package.sh` writes both hosts' archives of the same release into `kit.toml`
  (`fetch-toolchain.sh --print`), and Setup downloads the one for its host. `run-proton.sh` uses the same toolchain
  and build folder, so what you test is what players build.
- `setup-linux` and the Linux `xbe_recomp` come from `game/out/build/linux-kit`, a native build linked `-static` so
  they run on any distro. The Linux-hosted llvm-mingw itself needs glibc 2.34 or newer (Ubuntu 22.04 and later).

## Making a release

```bash
tools/package.sh                  # fetch the toolchain, build the kit, test it, zip it
tools/package.sh --test-download  # the same, with the compiler setup-linux downloads
```

1. **Toolchain.** It downloads the Linux-hosted llvm-mingw into `toolchain/` if it's missing.
2. **Kit.** It builds `Game_kit` in `game/out/build/llvm-mingw`, the native `xbe_recomp` for codegen, and the static
   `xbe_recomp` and `xbe_setup` in `game/out/build/linux-kit`.
3. **Staging.** It copies the kit into `game/out/release/Conker/`, adds `setup-linux` and `kit/bin/xbe_recomp`, and
   writes both hosts' llvm-mingw URLs and SHA-256s into `kit.toml`. No compiler is copied.
4. **Test.** It runs a Linux player's steps: the staged `setup-linux` against `game/assets`, with the Linux-hosted
   llvm-mingw from `toolchain/`, or with the one it downloads under `--test-download`. It fails unless a `Game.exe`
   comes out and `kit/work` is gone. `--no-test` skips it.
5. **Zip.** It writes `game/out/release/Conker-<git describe>.zip`.

Before publishing, run `Setup.exe` once for real: on Windows, or with `umu-run Setup.exe` in an unpacked copy. The
automatic test can't run Setup.exe, its WinHTTP download or the Windows-hosted compiler.

## Updating the toolchain

Change `VERSION` and both SHA-256s in `tools/fetch-toolchain.sh` (the release page lists the digests; they are
the archives players download too), and
`LLVM_MINGW_VERSION` in `game/cmake-llvm-mingw.cmake`. Then reconfigure `game/out/build/llvm-mingw` from scratch.
Players need a new release, since the host library and the kit's compiler must match.
