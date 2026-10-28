# Building Live & Recompiled from source

How to build the port from a checkout and how to package a release. For playing, see the
[README](README.md).

## Where things are

| Path | What it is |
|---|---|
| `src/runtime/` | The Xbox kernel, the XDK libraries (`xdk/`, with the XDK 1.0.5849 tables in `xdk/5849/`), the Vulkan renderer, audio and the host app |
| `src/conker/` | The game: `game.toml` (the recompiler's config), `common/` and `game/` (hooks, patches, input, the Esc menu) |
| `src/recomp/`, `src/symbols/` | `xbe_recomp`, the x86 to C++ recompiler, and `xbe_symbols`, which writes a new title's config |
| `src/setup/` | Setup, the player's build: unpacks the game, recompiles it and links it |
| `src/harness/` | A headless Linux build for testing, with no window or GPU |
| `cmake/` | `XbeRecomp.cmake` (the build of all of the above) and the Zig toolchain files |
| `tools/` | `build.sh` and `build.bat`, `package.sh` and `package.bat`, the toolchain pin and a few Python helpers |
| `assets/`, `generated/`, `out/`, `toolchain/` | Not in git: the game, the recompiled code, the build folders and the downloaded Zig |

## Building the port

### What you need

- **Your copy of the game**, the supported release (USA disc, SHA-1 `2232a1b3874d67b4bb4cf53a940420d2dab59535`, see the [README](README.md#installation)), as a disc image (`.iso`), extracted files
  (`default.xbe` with `dvddata/` and `media/` beside it), or the digital release's package (`DefaultPackage` and its
  `DefaultPackage.data` folder), which Setup's unpacker (`setup-linux --unpack`) unpacks (a Microsoft Store install keeps it in
  `C:\XboxGames\Conker- Live and Reloaded\Content\Game`; see the README's Installation).
- **Linux:** `cmake` (3.25 or newer), `ninja` and `git` (and `curl` for packaging's
  `--test-download`). Add `extract-xiso`
  to build from a disc image, and Wine or Proton to run `Game.exe`. The native port (`--linux`) also needs the X11 and
  Wayland development headers SDL builds against (SDL loads the libraries themselves at run time), and a Vulkan 1.3
  driver to run. `ndisasm` is only for `tools/xbe_disasm.py`.
- **Windows:** CMake (3.25 or newer), Ninja and Git. `tools\build.bat` offers to install whichever is missing with
  `winget`. No Visual Studio: the compiler is the pinned Zig, as on Linux and in releases.

### On Linux

```bash
tools/build.sh --iso path/to/disc.iso    # first build, from a disc image: extracts it into assets/
tools/build.sh --store path/to/package   # or from the digital release's DefaultPackage (or its folder): unpacks into assets/
tools/build.sh --game path/to/files      # or from files you already extracted: copies them into assets/
tools/build.sh                           # after a change: incremental
umu-run out/build/zig-windows/Game.exe
tools/build.sh --linux                   # also the native Linux port
out/build/zig-glibc/Game
```

`--iso`, `--store` and `--game` put the game in `assets/`. You only need them once; to switch to another copy, empty
`assets/` and run one again, and the build notices the switch and recompiles. The first run downloads the pinned Zig into `toolchain/` and checks
its SHA-256, then recompiles the whole XBE and compiles the result, so it takes a while. Later builds redo only what
changed.

`Game.exe` finds `assets/` by walking up from its own folder. It keeps `settings.toml`, its logs, shader records
and saves beside itself, and so does the native `Game`. The native port is the same game in an SDL3 window, drawn
with Vulkan and played through SDL3's audio; it has everything `Game.exe` has but the boot splash
and the window kept up across a relaunch. Every build, the native one and the harness
included, uses the pinned Zig against glibc 2.31 with Zig's libc++, as a release's native `Game` does (see
[Building a release](#building-a-release)), so what is measured in one holds for what players run. `tools/build.sh --harness` also
builds `out/harness/conker`, a headless Linux build with no window or GPU, for testing.

### On Windows

1. Put the extracted game in `assets\`: `default.xbe` with `dvddata\` and `media\` beside it. (`tools\build.bat`
   reads the files, not a disc image. `extract-xiso` has Windows builds.) From the digital release's package,
   `setup-windows.exe` does the unpacking. Without the game the build configures only the tools, so build it with
   `cmake -DWHAT=zig-windows -P tools\configure.cmake` and `cmake --build out\build\zig-windows --target setup`,
   then run `out\build\zig-windows\setup-windows.exe --unpack path\to\DefaultPackage assets`.
2. Run `tools\build.bat`. It checks for CMake, Ninja and Git, downloads Zig into `toolchain\` on the first
   run, recompiles the XBE and builds the port. Later runs rebuild only what changed.
3. Run `out\build\zig-windows\Game.exe`.

It's the same build as on Linux and in releases, with the same compiler and flags. `tools\build.bat --pdb` also writes
`Game.pdb`, for debugging in Visual Studio or WinDbg; it makes builds slower, and
`cmake -DXBE_PDB=OFF out\build\zig-windows` turns it off again. (`tools/build.sh --pdb` does the same on Linux.)
IDEs that read `CMakePresets.json` get the same build as the `zig-windows` preset, once `tools\build.bat` has fetched
the compiler.

## Building a release

A release is `setup-windows.exe` and `setup-linux`, each with its own kit inside (appended to the executable, unpacked into
its working folder, `work/` beside it, on the first run): the recompiler, the prebuilt runtime and hooks for that OS, the config, the headers
the generated code includes, and the pinned compiler's address and SHA-256. It contains none of the game.

### On Linux

Put the supported release in `assets/` (any `tools/build.sh` above does that), then:

```bash
tools/package.sh                         # writes out/release/LiveAndRecompiled-<version>-windows.zip and -linux.tar.gz
```

It downloads the pinned Zig if it's missing (the pin is in `tools/toolchains.cmake`). Zig builds both halves of the kit: the Windows one (`out/build/zig-windows`: Setup,
`xbe_recomp` and the runtime for `x86_64-windows-gnu`, with Zig's own mingw-w64 and static libc++), and the Linux one
(`out/build/zig-glibc`): `setup-linux` and the native port's runtime against glibc 2.31 and Zig's static libc++.
Setup's FLTK window (Wayland or X11, picked at run time) and the native SDL need Linux's X11, Wayland, ALSA, PulseAudio
and PipeWire headers (SDL loads those libraries at run time). A player's Zig links the game code with the same
libc++ into a `Game` that needs only glibc, or a `Game.exe` that needs only system DLLs. Then it tests the release as
a Linux player would: the packaged `setup-linux` builds the native `Game` from the game in
`assets/`, and the test fails unless it comes out. `--no-test` skips the test, and `--test-download` makes it
use the compiler Setup downloads.

The kit only accepts the exact XBE it was packaged from, so package from the supported release.

### On Windows

You need the same CMake, Ninja and Git as for building (running `tools\build.bat` once sets them up). Packaging
builds with the pinned Zig, which it downloads into `toolchain\`. Put the supported release in
`assets\`, then:

```bat
tools\package.bat
```

It writes `out\release\LiveAndRecompiled-<version>-windows.zip` and takes the same `--no-test`, `--test-download` and
`-j N` options. Here the test runs the packaged `setup-windows.exe`.

`setup-linux` and the kit's Linux half need Linux's headers: a release packaged on Windows has `setup-windows.exe`
alone, which builds `Game.exe`.

Both scripts run `tools/package.cmake`, which can also be run directly: `cmake -P tools/package.cmake`, with
`-DTEST=OFF`, `-DTEST_DOWNLOAD=ON` or `-DJOBS=N` before `-P`.

## The digital release's package

The digital, backwards compatible release is a small `DefaultPackage` file and a `DefaultPackage.data` folder of
`Data0000`, `Data0001`, ... It's an Xbox 360 Games-on-Demand package: the disc's file system cut into 4 KiB blocks
with a table of hashes after every 204 of them. Nothing in it is encrypted, so Setup only needs to
leave the hash tables out and find where the files start. `src/setup/store.cpp` has the layout. It writes the same 881
files as extracting the disc does, and Setup still compares the XBE's SHA-1: the disc's, or the Microsoft Store
`default.xbe`'s, which has the same code and data and only a different header (`XBE_ALSO_SHA1` in `CMakeLists.txt`).

```bash
setup-linux                              # the window: the game (package, image or files), working and game folder
setup-linux --cli --source DefaultPackage --work DIR --game-dir DIR   # the same on the console
setup-linux --unpack DefaultPackage outdir # only the unpacking
tools/build.sh --store DefaultPackage    # builds setup-linux first, unpacks into assets/, builds the port
```

It is read only on the package, never touches keys or signatures, and takes about 5 GB of space and a few seconds.
