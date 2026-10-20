# Building Conker: Live & Recompiled from source

How to build the port from a checkout and how to package a release. For playing, see the
[README](README.md); for why releases are built this way, see its "Two ways to get the game".

## Building the port

### What you need

- **Your copy of the game**, the supported release (USA disc, SHA-1 `2232a1b3874d67b4bb4cf53a940420d2dab59535`, see the [README](README.md#installation)), as a disc image (`.iso`), extracted files
  (`default.xbe` with `dvddata/` and `media/` beside it), or the digital release's package (`DefaultPackage` and its
  `DefaultPackage.data` folder), which Setup's unpacker (`xbe_setup --unpack`) unpacks (a Microsoft Store install keeps it in
  `C:\XboxGames\Conker- Live and Reloaded\Content\Game`; see the README's Installation).
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
with Vulkan and played through SDL3's audio; it has everything `Game.exe` has but the boot splash
and the window kept up across a relaunch. This build uses the system clang and libstdc++; a release's native `Game` is
built with the pinned Zig instead (see [Building a release](#building-a-release)). `tools/build.sh --harness` also
builds `game/out/harness/conker`, a headless Linux build with no window or GPU, for testing.

### On Windows

1. Put the extracted game in `game\assets\`: `default.xbe` with `dvddata\` and `media\` beside it. (`build.bat`
   reads the files, not a disc image. `extract-xiso` has Windows builds.) From the digital release's package,
   `xbe_setup` does the unpacking: build it with `cmake --build game\out\build\llvm-mingw --target xbe_setup`
   after the first `build.bat`, then `game\out\build\llvm-mingw\xbe_setup.exe --unpack path\to\DefaultPackage game\assets`.
2. Run `game\build.bat`. It checks for CMake, Ninja and Git, downloads llvm-mingw into `toolchain\` on the first
   run, recompiles the XBE and builds the port. Later runs rebuild only what changed.
3. Run `game\out\build\llvm-mingw\Game.exe`.

It's the same build as on Linux and in releases, with the same compiler and flags. `build.bat --pdb` also writes
`Game.pdb`, for debugging in Visual Studio or WinDbg; it makes builds slower, and
`cmake -DXBE_PDB=OFF game\out\build\llvm-mingw` turns it off again. (`tools/build.sh --pdb` does the same on Linux.)
IDEs that read `game\CMakePresets.json` get the same build as the `llvm-mingw` preset, once `build.bat` has fetched
the compiler.

## Building a release

A release is `setup-windows.exe`, `setup-linux` and a kit: the
recompiler, the prebuilt runtime and hooks for Windows and for Linux, the config, the headers the generated code
includes, and the pinned compilers' addresses and SHA-256s. It contains none of the game.

### On Linux

Put the supported release in `game/assets/` (any `tools/build.sh` above does that), then:

```bash
tools/package.sh                         # writes game/out/release/Conker-<version>.zip
```

It downloads the pinned llvm-mingw and Zig if they're missing (the pins are in `tools/toolchains.cmake`). It builds
the kit's Windows half with llvm-mingw, with Zig the Linux `xbe_recomp` (a static musl program that runs on any
x86_64 distro), and `setup-linux` and the kit's Linux half (`game/out/build/zig-glibc`): Setup's FLTK window (Wayland or X11, picked at run time), which
needs their headers, and the native port's runtime,
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

Zig builds the Linux `xbe_recomp` on Windows too, but `setup-linux` and the kit's Linux half need Linux's headers: a
release packaged on Windows has `setup-windows.exe` alone, which builds `Game.exe`.

Both scripts run `tools/package.cmake`, which can also be run directly: `cmake -P tools/package.cmake`, with
`-DTEST=OFF`, `-DTEST_DOWNLOAD=ON` or `-DJOBS=N` before `-P`.

## The digital release's package

The digital, backwards compatible release is a small `DefaultPackage` file and a `DefaultPackage.data` folder of
`Data0000`, `Data0001`, ... It's an Xbox 360 Games-on-Demand package: the disc's file system cut into 4 KiB blocks
with a table of hashes after every 204 of them. Nothing in it is encrypted, so Setup only needs to
leave the hash tables out and find where the files start. `game/sdk/tools/setup/store.cpp` has the layout. It writes the same 881
files, byte for byte, as extracting the disc does, `default.xbe` included, so the supported-release rule is
all there is to check: Setup still compares the XBE's SHA-1.

```bash
setup-linux                              # the window: the game (package, image or files), working and game folder
setup-linux --cli --source DefaultPackage --work DIR --game-dir DIR   # the same on the console
xbe_setup --unpack DefaultPackage outdir # only the unpacking
tools/build.sh --store DefaultPackage    # builds xbe_setup first, unpacks into iso/, builds the port
```

It is read only on the package, never touches keys or signatures, and takes about 5 GB of space and a few seconds.
