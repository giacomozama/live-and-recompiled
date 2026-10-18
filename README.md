# Conker: Live & Reloaded — static recompilation

An unofficial native PC port of the original Xbox *Conker: Live & Reloaded*, with extended graphics options and other
enhancements. It's built on a heavily modified version of
[Xbox XBE Recomp](https://github.com/TGP482/XboxXBERecomp), vendored in `game/sdk/`.

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of
> the game to install or build. The game [can currently be purchased from the Windows Store](https://www.xbox.com/games/store/conker-live-and-reloaded/bvfb8cbs75r6). The Windows Store version is fully compatible with this project.

> [!WARNING]
> **AI usage in this project**
>
> First-party code in this repo was not written with LLM *assistance*. It was, in fact, *entirely* written by LLMs (if you're wondering which ones, it was about $4 worth of Deepseek 4.1 tokens to write the initial boilerplate, and Opus 5.5 on a single $20 Claude sub to do the rest). This is, as it is called nowadays, a **slopcomp**. The human in the loop has only provided initial prompting, minimal hints about known game internals (to save on reasoning, mostly), and extensive high level debugging in the form of taking screenshots and dumps for the LLM to figure out. The only human-made things in this repo are the very notice you're reading and some parts of this README.
>
> Yes, it IS AI slop according to most definitions, it is entirely vibecoded. Not even the "good kind" of vibecoded. Code itself was NOT reviewed at all and bugs are found by manually testing the game. I have no reason to believe it will do any harm to your PC or do anything more destructive than leaking memory, crashing or having visual or gameplay glitches.
>
> If you don't like generative AI and won't try this because of it, I understand and sincerely respect your decision. I myself believe gen-AI is detrimental to most creative fields, but I also believe that for a static recomp like this, little creative work is lost by delegating the task to LLMs. There is a reason recomps were so rare before LLMs could do them in an afternoon: they are largely menial, repetitive, mind-numbing, derivative, uninspiring work that doesn't create new knowledge for anybody. It's mostly equivalent to creating a very optimized emulator that only works for a single game: most people just don't care enough and would rather put their effort towards the more general solution or towards human-readable source decompilation for specific, high-profile games. It's hard to envision circumstances in which a project like this would be significantly improved by extensive human coding contributions.
>
> I despise the idea of a handful of companies monopolizing all professional applications of human intelligence. I miss the time when not everything was about doing things as fast as possible in spite of quality. I miss writing code by hand every day. But we told ourselves we'd use the best tool for the job. And for this job (and unfortunately many others), this was the right tool.

## Implementation Status / Known Bugs

- Single player is theoretically implemented, but no one has played it start to finish yet. Playtesting is very appreciated: you are encouraged to play through the game and report bugs, crashes and glitches.
- Still in the process of recording shaders encountered during gameplay. These can then be rebuilt at startup time so future players never encounter stutters even during their first gameplay. Sharing these is also appreciated (`shaders.rec`)
- Mouse looking currently works very poorly.
- Minor depth of field quirks.
- Bugs with above-30 FPS settings (especially 120fps).
- No effort has been made to implement multiplayer yet, it's not a priority since deep down everyone knows no one will actually play it more than once.

## Installation

Setup builds the game on your PC from your own copy, once. It is three steps on both systems: put the game in
`assets`, run Setup, run the game. Pick your system:

### Windows

1. **Put your game in the `assets` folder.** What you have decides how:
   - **A disc image (`.iso`)**, or **extracted files** (`default.xbe` with the game's folders beside it): put it in
     `assets` as it is.
   - **The digital, XBOX Backwards Compatibility release** (`DefaultPackage` with a `DefaultPackage.data` folder):
     - **Installed from the Microsoft Store in its default place** (`C:\XboxGames\Conker- Live and Reloaded\Content\Game`,
       not encrypted): leave `assets` empty. Setup finds it, asks whether to use it, and unpacks it for you.
     - **Anything else:** copy `DefaultPackage` and `DefaultPackage.data` into `assets` and run
       `unpack-store-windows.exe`. It unpacks the game's files into `assets` in a few seconds and needs about 5 GB
       free; afterwards the package can go.
2. **Run `setup-windows.exe`.** It checks your copy is the release this build supports, downloads
   [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (80 to 180 MB, the first time only), recompiles the game
   and writes `Game.exe`. This takes a few minutes.
3. **Run `Game.exe`.** Settings are in `settings.toml` beside it, and in the F1 menu.

### Linux

1. **Put your game in the `assets` folder.** What you have decides how:
   - **A disc image (`.iso`)**, or **extracted files** (`default.xbe` with the game's folders beside it): put it in
     `assets` as it is.
   - **The digital, backwards compatible release** (`DefaultPackage` with a `DefaultPackage.data` folder): if you
     bought the game on the Microsoft Store, download its package with the
     [Xodus project](https://github.com/xodus-gaming/xodus), copy `DefaultPackage` and `DefaultPackage.data` into
     `assets` and run `./unpack-store-linux`. It unpacks the game's files into `assets` in a few seconds and needs
     about 5 GB free; afterwards the package can go.
2. **Run `./setup-linux`** in a terminal opened in this folder. It checks your copy is the release this build
   supports, downloads [Zig](https://ziglang.org) (about 50 MB, the first time only), recompiles the game and
   writes the native `Game`. This takes a few minutes. Nothing of the game is uploaded or downloaded. If it says
   permission denied, your unzip tool dropped the executable bit:
   `chmod +x setup-linux unpack-store-linux kit/bin/xbe_recomp`.
3. **Run `./Game`.** Settings are in `settings.toml` beside it, and in the F1 menu.

`./Game` is the native port: an SDL3 window (X11 or Wayland) drawn with Vulkan, which needs a Vulkan 1.3 driver and
glibc 2.31 or newer (any distro since 2020, SteamOS included). Add it to Steam as a non-Steam game to play it from
Game Mode. It has everything `Game.exe` has but the boot splash and the window kept up across a relaunch. To build
`Game.exe` instead, for Proton or Wine, run `./setup-linux --target windows`; it downloads llvm-mingw then.
`setup-windows.exe` works under Proton too (`umu-run setup-windows.exe`).

Setup keeps nothing it generated except the game and the compiler in `kit/toolchain`. Run it again after updating
to a new release.

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
- **Vulkan Renderer** - The game draws with Vulkan 1.3 on Windows and Linux alike, and needs a Vulkan 1.3 GPU and
  driver. `renderer` in `settings.toml` names the renderer (`"vulkan"`, the only one; anything else gets it too).
  `XBE_VULKAN_DEBUG=1` turns on the validation layer, when it is installed, and writes its messages to the log.

### Controller
Supports Xbox, PlayStation and Switch controllers, with hot plugging and rumble. A gamepad is not required: the
keyboard and mouse drive player 1 as well, and both can be used at the same time. Every action can be rebound under
**Controls** in the F1 menu.

### General
- **Skippable Cutscenes** - LT skips any in-engine cutscene, not only the ones already seen this playthrough. Turn it on
  in the F1 menu or with `skippable_cutscenes = true` in `settings.toml`.

## System Requirements

### Minimum
- **OS**
  - Windows 10 64-bit, or
  - Linux x86-64 with glibc 2.31 or newer
- **GPU** with support for Vulkan 1.3
- **Disk** about 5 GB free to unpack the digital release, if you have that one

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
with Vulkan and played through SDL3's audio; it has everything `Game.exe` has but the boot splash
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
- [xemu](https://github.com/xemu-project/xemu) and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) as references for the Xbox's hardware and API behaviour. This project does not use emulation or any emulator code.
- [Zydis](https://github.com/zyantific/zydis) for decoding x86 instructions, [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (Clang, LLD, mingw-w64, libc++), the compiler `setup-windows.exe` builds the game with.
- [Zig](https://ziglang.org) (`zig c++`: Clang, LLD, glibc stubs, libc++), the compiler `setup-linux` builds the native game with.
