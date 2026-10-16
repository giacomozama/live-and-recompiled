# Conker reference

Facts about *Conker: Live & Reloaded*'s XBE and engine that the port depends on. Addresses are guest virtual
addresses in the retail `default.xbe` (title `0x4D530051`). `tools/xbe_disasm.py <va> [len]` disassembles any of
them; `game/sdk/tools/symbols/xbe2pe.py config/game.toml game.exe` makes a PE for Ghidra with the recompiled
functions as exports.

## The image

- Entry `0x42F0E9`; all 13 libraries at **XDK 1.0.5849** (the SDK's `xdk/5849/` tables came from another title).
- 21,956 functions / 1.47 M instructions → ~227 MB of C++ in 128 partitions.
- Rough map: game code in `.text`; the D3D8 library around `0x535000..0x546000` (globals `0x547000..0x54A900`);
  DirectSound `0x4C0000..0x4CA000` with its DSP table at `0x4E1D1C`; XMV around `0x4E2xxx`; XACT `0x54B000..0x556000`.
- Libraries are **LTCG**. D3D8 functions take arguments in registers (`[abi]`) and many internals are called
  directly from `.text`; `tools/xbe_d3d_internals.py iso/default.xbe game/config/game.toml` lists the ones the
  config doesn't name. Most of them need no replacement: they run recompiled, and their push-buffer output reaches
  the renderer through `Walk()`.
- The DirectSound `.XTLID` ordinals are assigned **alphabetically within each interface** (`B003x` IDirectSound,
  `B005x..B007x` IDirectSoundBuffer, `B008x..B009x` IDirectSoundStream), so an unnamed export can be identified by
  its position between known ones. `IDirectSound_CreateSoundStream` (`0x4C67B9`) and
  `IDirectSoundStream_SetOutputBuffer` (`0x4C5A03`) were found this way.

## Config notes (`game/config/game.toml`)

The file's comments carry the proofs; the points to know:

- **Corrected `[data]`**: `D3D_DeferredTextureState = 0x547330` (the scan reported `0x5473A0`, stage 0's
  `TEXCOORDINDEX` field) and `D3D_Streams = 0x547158` (the scan reported entry 0's buffer field). Both bugs made the
  runtime write over the wrong fields.
- `D3D_FvfShader = 0x547040`: about 68 D3D internals dereference `device+0x794` whenever state changes, so it must
  never be null.
- `D3D_DeviceInit` (`0x53E5D0`) is deliberately **not** run: it brings up NV2A hardware the runtime can't answer.
  The runtime backs the record block it would allocate (`DeviceRecords`), and the internals that need the miniport
  are replaced (`0x538D50`).
- `D3D_InitDefaultState` isn't found in this image; the runtime seeds the stencil defaults itself.
- Hand-mapped functions: `D3DDevice_DrawVerticesUP = 0x53A600`, `D3DDevice_DrawIndexedVerticesUP = 0x53A7B0`,
  `D3DDevice_SetRenderTarget = 0x538E00`, the DirectSound ordinals above, and `[functions] 0x1E6590` (reached only
  through a pointer).
- `[abi]` lists the register maps for the D3D functions the runtime replaces or calls.

## Game hooks

Function replacements (`X86_FUNC`) in `game/src/game/`:

| Address | What it is | What the port does | File |
|---|---|---|---|
| `0x538D50` | Begin frame: set back buffer, clear | Does it through the runtime's D3D (the original reads the miniport) | `hooks.cpp` |
| `0x543170` | D3D deferred state flush | Skipped; `d3d::DeferredFlush` decides raw vs D3D state | `hooks.cpp` |
| `0x53AD90` | Apply queued push-buffer fixups | `d3d::FlushPending()` first, then the original | `hooks.cpp` |
| `0x4E2801` | XMV per-frame update | Returns -1 with `XBE_SKIP_FMV` (movie "failed", skipped) | `hooks.cpp` |
| `0x4FF00` | "May LT skip this cutscene?" | Any cutscene when `skippable_cutscenes` is on | `hooks.cpp` |
| `0x1A9540` | Draw one grass patch | Fading patches get the wind program (`grass_wind`) | `hooks.cpp` |
| `0x4C950` | Controller layer init | Re-marks opened pads as inserted (else the sticks read zero) | `hooks.cpp` |
| `0x4C3F2F` | DSound buffer flag helper | No-op (console sub-object) | `hooks.cpp` |
| `0x4C5235`, `0x4C51B8` | Voice 3D descriptor setters | Hand distance/cone/occlusion volume and azimuth to `dsound::SetVoice3d` | `hooks.cpp` |
| `0x4C3DFE` | Their shared body | No-op (writes the console voice) | `hooks.cpp` |
| `0x4C5D4C`, `0x4C37C1..0x4C3897`, `0x4C9C86` | 3D calculator and listener helpers | Install the HRTF table (`0x4C753A`) first, then run | `hooks.cpp` |
| `0x2A7B60` | Frame timer | Paces the frame above 30 FPS, sets the present interval | `display.cpp` |

Mid-function `[[hook]]`s (`game.toml`, implemented in `display.cpp`):

| At | Function | Does |
|---|---|---|
| `0x2A7CBC` | `ConkerFrameTicks` | Above 30 FPS, the frame's ticks come from its measured time (remainder carried) |
| `0x2D37F4` | `ConkerFrameStep` | A frame's step divisor is never zero |
| `0x64468` | `ConkerCameraAspect` | Widens the projection's aspect to the frame's (Hor+) |

## Engine notes

**Frame loop and timing.** The frame loop calls the frame timer `0x2A7B60` (from `0x2A48C9`), renders (`0x2A3FD0`,
from `0x2A4985`) and calls `Swap` at `0x2A499B`. The console presents every second blank (`0x2A78D0` builds the
present parameters; interval byte at `0x6C0268`). Each frame the timer writes `[0x7FA21C]` dt in seconds (TSC-based,
or ticks/60 in fixed-step mode `[0x7819D5]`, used by multiplayer) and `[0x7FA20C]` ticks (60 Hz blanks the frame
took, at most 60). About 400 places step by ticks; single player otherwise runs on dt. That's why the 60/120 FPS mode
works: ticks are derived from real time. Still unproven: per-frame logic that ignores both clocks.

**Aspect.** `0x64468` builds a camera's projection from `[camera+0x260]` (aspect, 4:3) and `[camera+0x264]` (vertical
FOV). The frame stays 640x480 and is shown at the display's shape, so the HUD stretches (as with the community XBE
patch); a sharper wide frame needs a higher Render %. These hooks reproduce the community widescreen and 60 FPS XBE
patches (`0x6446A`, `0x1F6B23`, `0x2A9DCC` in that patch).

**Input.** Controller records are at `[0x806FD8]` (0x140 bytes per pad, inserted flag at `+0x13C`); the stick filter
`0x4CF80` zeroes pads not marked inserted. The right stick lands in `0x806AD8` and is copied into each player's pad
by `0x4E0D0`. The player camera is `[[0x8470DC] + 4]` (yaw at `+0x60`), found with `XBE_CAMSCAN`. Mouse look
currently drives the **right stick**, not the camera angles.

**Cutscenes.** `0x4FF00` is the per-frame skip test: seen-this-playthrough bitmask at `0x765608` (set at `0x50877`),
mode at `[0x7FA1F8]` (`0x18` the throne intro, `0x1D` its later scenes, `0x21` skips on its own button).

**Models and push buffers.** Character models are static push buffers from the `.rbm` (a 576-byte NOP first,
patched to a jump when the object is culled), entered from the scratch buffer with a jump and leaving with one.
`RunPushBuffer` (`0x53AE30`, from the node renderers, e.g. `0x3F3510`) runs one with a fixup that selects a material
block per pass, which is why `0x53AD90` must flush first (otherwise fur draws as the base pass).

**Visibility tests.** Characters and doors are culled with GPU occlusion tests (reader `0x313C20`), drawn as
`DrawIndexedVertices` boxes at the start of the frame against the previous frame's depth. A pending test keeps the
previous result. A wrong vertex layout for those boxes makes objects flash out.

**Rendering facts the renderer relies on.**
- The engine's final combiner is `lerp(R0, fog, fog.a)`: "fog disabled" must mean factor 0, not 1, or everything
  turns fog-coloured.
- Glow (end of frame): a 256x256 mask from the scene and the depth buffer read as `LIN_R8G8B8A8` (A = stencil),
  blurred and added. **The stencil value is the glow level**: 0 none, 1..15 weighted by scene alpha, 16+ gives
  `(k-15)/255`.
- Depth of field reads the depth buffer as colour (`LIN_R8G8B8A8` for the focus mask, `LIN_A8B8G8R8` for the
  composite).
- Character lighting (program `00A46AF68A6652C9`): `oD0` is SH ambient (`c70..c81`), `oD1` the key light
  (`c83`/`c84`) times blob shadows (`c50..c61`), `oT2` a normal lookup into a per-character 32x32 lighting map the
  engine renders itself (program `21751DC84070A8C5`).
- Fur: program `1B4CFC40E338AEDB` offsets vertices along the normal by `c85.x * v5`; stage 1 is the fur mask.
- Grass: near patches use a wind program, fading ones a plain transform whose alpha is `TFACTOR.a`, set by a raw
  `COMBINER_FACTOR0` write (`0x83F30`). Handles at `[0x6B89A4]` (wind) and `[0x6B89DC]` (fade).
- The material blocks write cull face, stencil enable/fail and specular enable raw, so those reach the renderer only
  through `Walk()` (the soldier's two-sided flag, the glow marks, character specular).

**Audio.** XACT's clock is the APU sample counter `0xFE80200C` (read at `0x55485B`). XACT stops a stream with an
async `FlushEx` and then waits for `ENVELOPECOMPLETE` from `GetStatus` (`0x4C8943`). It needs reads to be truly
asynchronous (`0x552DF9` → `0x54BBC0`). The title tune is in `m_hungover.xwb`.

## Known issues

- **Missing loading screen**: the game freezes where a loading screen should show (after the boot splash, starting a
  new game, between cutscenes) and continues once the level has loaded.
- Puddles reflect Conker but not the environment (seen on the harness; re-check on D3D12).
- Harness only: walking in TrainingLevel hit a guest divide error at `0x53A833` (D3D library). Not investigated.
- The HUD and menus stretch at aspects other than 4:3.
- MSAA doesn't smooth alpha-tested edges (foliage, fur); that would need alpha to coverage.
- 120 FPS: logic that ignores both clocks may run fast. FMV timing above 30 is unverified.
