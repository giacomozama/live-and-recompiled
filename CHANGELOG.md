# Changelog

## 0.1.0 - 2026-09-30

First version, extracted from the Splinter Cell Double Agent (original Xbox) static recompilation, which builds both of its images (default.xbe and sc4_adv.xbe) on it.

- **Recompiler** - `xbe_recomp`: XBE parsing with retail keys, function discovery (direct call fixpoint, pointer seeds, gap fill, trusted walks), jump tables (bounded, byte indexed, backwards, MSVC memcpy tables, movsb/movsd sleds), MSVC C++ exceptions, x87 with a dynamic top, SSE and MMX, partitioned output of weak `sub_X` over `__imp__sub_X`, `image_symbols.h` from `[symbols]`, `[data]` and `[layout]`, files written only when changed.
- **Runtime** - CPU glue, xboxkrnl, Direct3D 8, DirectSound, XAPI, XNet system link, Bink and CRT fixes at the API level; native D3D12 renderer with a threaded command stream, raw vertex path, render scale; XAudio2 output; XInput pads; window, settings, splash, crash dumps, relaunch handover.
- **Clean room shader helpers** - Combiner input mapping, texture mode code and vertex program helpers in the HLSL generator, and depth format decoding, rewritten from the specifications and bit layouts; value mappings proven equal exhaustively, generator output equal on 20,000 keys.
- **XDK 5849** - `xdk/5849/layout.toml` (28 keys, each proven by disassembly in both SCDA images), `signatures.toml` (masked hashes, no library bytes), `quirks.md` with the addresses behind each reproduced behaviour. Unknown layout keys trap instead of being guessed.
- **xbe_symbols** - Finds XDK functions and data in a new XBE from the build's signatures and writes its config; `--xbsdb` takes XbSymbolDatabaseCLI output; `--sign` writes signatures from complete configs. `xbe2pe.py` writes a flat PE for Ghidra.
- **Game template** - `templates/game`: CMakeLists, presets, `build.bat` that runs `xbe_symbols` on the first build, `release.bat`, `GameInfo` and hook stubs, README.
- **GameInfo** - One struct per game for names, files, the image table, settings and hooks (init, loaded, window, message, ui, pad, rumble).
- **Renderer extension** - `gpu::Extension`, `gpu::Context` and `gpu::ShaderHooks` (`runtime/gpu/ext.h`), set with `gpu::SetExtension`.
- **Diagnostics** - `XBE_PERF`, `XBE_CAPTURE_DIR`, `XBE_D3D12_DEBUG`, off by default.
- **Game features kept out** - The settings panel, console, cursor, keyboard and mouse input and the enhanced SM3 renderer stay in the game repo, wired through `GameInfo` and the renderer extension.
- **Tooling** - `harness/` headless Linux runner, `tools/sdk_sync` for keeping game copies and the SDK in step.
