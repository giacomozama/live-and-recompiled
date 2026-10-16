# Conker: Live & Reloaded — static recompilation handoff

Unofficial native port of the original-Xbox *Conker: Live & Reloaded* built with
[Xbox XBE Recomp](../../) (this workspace's `XboxXBERecomp/`, the SDK). The game's x86 `.xbe` is translated
to C++ ahead of time; the Xbox kernel and XDK libraries are implemented at the API level and drawn with a
Direct3D 12 renderer. This document is the state of the port at handoff.

> This repository contains no game assets. The ISO / extracted files are the user's own.

## Status (TL;DR)

The game **statically recompiles and boots** on both the Linux headless harness and a **Windows build run
under Proton**:

- kernel init, disc mount (`D:`), XOnline/controller init
- `Direct3D_CreateDevice` (×2), 640×480, ~50 vertex shaders created, per-frame D3D through the runtime
- volume queries (`IoQueryVolumeInformation`), XNet, DirectSound + XACT/audio, the frontend level
  (`Level\Frontend`), fonts, text and package (`*.rba`/`*.rbm`/`*.xwb`) loads all run
- the **DirectSound/XACT object model is complete enough to boot the frontend**: the harness now runs
  indefinitely (30 s+), presenting ~8 draws per frame instead of crashing in audio setup
- the engine's inlined D3D8 state now reaches the renderer: the deferred texture-stage table base was
  mislocated and is fixed, so the combiner reads real `COLOROP`/args (see
  [D3D8 raw push buffers](#progress-since-the-previous-blocker-d3d8-raw-push-buffers))
- the crash at the end of the launch FMVs is fixed and the game now **reaches the frontend menu**
  (`T:\preferences.bin`, `UIFrontend`/`UIPause` text) on the D3D12/Proton path; `XBE_SKIP_FMV=1` skips the
  intro cutscenes so iteration is not gated on 67 s of FMV — see
  [post-FMV crash](#progress-since-the-previous-blocker-the-post-fmv-crash)
- **the frontend 3D scene renders** (Cock & Plucker exterior and tavern, Conker, the Tediz, rain, candles), textured,
  lit and fogged — see [the frontend 3D scene renders](#the-frontend-3d-scene-renders-2026-10-15). Open: some skin patches
  look flat, puddle reflections are partial (harness-only, probably).

Pipeline: 21,956 functions / 1.47 M instructions recompiled → 227 MB of C++ → `Game.exe`.

## Progress since the previous handoff

The previous state stopped in engine init writing guest `0x18` from `0x62A60`. That was a **kernel
stack-argument-count bug** (`IoQueryVolumeInformation` popped 4 args while the engine's CRT pushes 5),
which corrupted the caller's frame and only looked like an engine/subsystem bug. Fixing it, and then six
later issues, carried the port from that bogus write deep into the frontend and audio setup:

| # | Fix | Where |
|---|-----|-------|
| 1 | `IoQueryVolumeInformation` pops 5 args, not 4 (the CRT's `_findfirst` pushes `Device, Class, Length, Buffer, IoStatus`) | `runtime/kernel/xboxkrnl.cpp` |
| 2 | `D3DDevice_DrawVerticesUP = 0x53A600` (was unmapped; the engine calls the library internal directly, which read uninitialised device state) | `config/game.toml` |
| 3 | `D3D_FvfShader = 0x547040` so `CreateDevice` points `device->vs` at the library's FVF shader object; ~68 D3D internals guard on it | `config/game.toml` |
| 4 | back the MCPX APU register window `0xFE800000` with scratch; XACT polls `0xFE80200C` | `runtime/kernel/memory.{h,cpp}` |
| 5 | `0x4C67B9` (a second `CreateSoundBuffer` thunk the signature scan missed) routes to the runtime's `IDirectSound_CreateSoundBuffer` HLE | `src/game/hooks.cpp` |
| 6 | `0x4C3F2F`, a DSound buffer flag helper that touches a sub-object the runtime's objects lack, becomes a no-op | `src/game/hooks.cpp` |
| 7 | `NewBuffer` ignores a descriptor whose `dwSize != 0x18` (the engine's XACT path passes one) so it cannot request a multi-GB block | `runtime/xdk/dsound.cpp` |

Each fix was verified on the headless harness; the crash point moved forward every time, and the next section
finishes the DirectSound object-layout work those fixes exposed.

## Progress since the previous blocker (the DirectSound surface)

The previous handoff stopped on a crash inside `0x4C5708`, a DirectSound library body reached from XACT that
read a console voice sub-object (`[buffer+0x14] -> +0xb0`) the runtime's host objects never build. The
`.XTLID` ordinals identified each crashing export (the DSOUND table of ordinals `0xB00xx` is alphabetical per
interface); the runtime HLEs the ones that need a real object, and the game hooks skip the console-DSP paths:

| # | Export (ordinal) | Fix | Where |
|---|------------------|-----|-------|
| 1 | `IDirectSoundStream_SetOutputBuffer` (`B0098`, body `0x4C5907` via `0x4C5A03`) | HLE no-op (host mixer sums all voices to the front pair) | `runtime/xdk/dsound.cpp` |
| 2 | `IDirectSoundStream_SetPitch` (`B0099`), `SetLFO` (`B0092`); `IDirectSoundBuffer_SetPitch` (`B0076`), `SetLFO` (`B006D`), `SetEG` (`B0067`) | HLE no-ops (console voice-register writes) | `runtime/xdk/dsound.cpp` |
| 3 | `IDirectSoundBuffer/Stream_GetVoiceProperties` (`B0055` / `B0081`) | HLE that zeroes the 92-byte `DSVOICEPROPS` | `runtime/xdk/dsound.cpp` |
| 4 | `IDirectSound_CreateSoundStream` (`B003B`, `0x4C67B9`) — the old fix #5 above routed it to `CreateSoundBuffer`, so the XACT stream had **no vtable** | real HLE (shared `NewStream`) that builds the stream with the XDK vtables; removed the old `hooks.cpp` override | `runtime/xdk/dsound.cpp`, `config/game.toml` |
| 5 | `0x4C3DFE` — the shared body of the library's two voice-descriptor setters (`0x4C51B8` buffer / `0x4C5235` stream, neither an ordinal import) | no-op | `src/game/hooks.cpp` |
| 6 | `0x4C5D4C` — the library's 3D-parameter apply, which calls DSP function pointers its device init installs at `0x4E1D1C`/`0x4E1D28` (skipped, so null) | no-op | `src/game/hooks.cpp` |
| 7 | `0x4C37C1/0x4C37EA/0x4C3821/0x4C3841/0x4C3875/0x4C3897/0x4C9C86` — the listener 3D helpers that tail through the null `0x4E1D20..0x4E1D40` DSP table | no-ops | `src/game/hooks.cpp` |

With #1–#4 the crash left DirectSound and entered the frontend render loop; #5–#7 then removed the last
console-DSP traps (an intermittent one fired ~1.3 s in on the audio thread). The harness now runs the full
30 s and presents frames. `AuditExports()` still lists four exports running their original code
(`XAudioCalculatePitch` `B00A3`, `XAudioCreatePcmFormat` `B00A4`, `XAudioCreateAdpcmFormat` `B00A5`, and the
`FlushEx` wrapper `B0080` whose callee *is* HLE'd); none of them touches a DSound object or the DSP, so they
are safe to leave.

## Progress since the previous blocker (D3D8 raw push buffers)

With audio fixed the port boots the frontend and presents frames, but every frame is **black** — on the
headless harness *and* the real D3D12 path under Proton (verified by the user). The cause is not audio or a
harness limitation:

**The engine inlines the D3D8 library into raw NV2A push-buffer writes** (and a few memory tables), so the
runtime's API HLEs largely never fire. `runtime/xdk/d3d8.cpp`'s `Walk()` decoded only *inline-vertex*
methods (0x1500/0x1518/0x1880/0x1940/0x1A00/0x17FC) and discarded every other method, so almost all state
was dropped. Measured with temporary probes on the harness:

- Every draw had **all vertex-shader constants `c0..c15` zero** (`g_state.vsc`), so each vertex shader
  collapsed its input. The engine emits them as methods `0x1EA4` (register) + `0xB80` (data).
- Every draw had **`blend=0 cw=0 at=0`** and texture-stage `COLOROP` reading `0` (unset).

Two fixes landed in `runtime/xdk/d3d8.cpp` (in the SDK):

| # | Fix | Effect |
|---|-----|--------|
| 1 | `Walk()` decodes transform-constant load (`0x1EA4`) + data (`0xB80`) into `g_state.vsc` | the full-screen frontend quads now get real constants (e.g. `vsc[96..99]` non-zero) |
| 2 | `Walk()` stores every other walked method into `g_state.simple[method>>2]` | render state now arrives: the draw list shows `blend=1 302 1 cw=10101` instead of all-zero |

The harness is stable after both. The TSS lead was then resolved: **the deferred texture-stage table base was
mislocated.** `D3D_DeferredTextureState` was `0x5473A0`, but that is the `TEXCOORDINDEX` **field** of stage 0,
not the table base. The engine writes `[0x547330 + stage*0x80 + field*4]` (proven by disassembling the sibling
bodies at `0x537B2D`/`0x537B7B`/`0x537BC7`; `0x5473A0 = 0x547330 + 28*4`). `SyncDeferred()` therefore read
stage `s+1`'s fields as stage `s`'s, so `COLOROP` (field 12) landed on `AlphaOp` (field 16) and read 0. With
`D3D_DeferredTextureState = 0x547330` the draw list shows real combiner state (`cop 2 2 0`, `cop 2 2 3`,
`addr 3 3`). The runtime enum in `runtime/gpu/state.h` was right all along; only the config value was wrong.
`SyncDeferred()` now also reads the whole `[4][32]` table for a title that inlines the library, while leaving
the fields whose dedicated `SetTextureState_*` HLE writes `g_state` directly untouched.

## Progress since the previous blocker (texture-stage state)

With the TSS base fixed the combiner is correct, **but the frame is still black**. Probes narrowed it down:

- **Rendering works.** With `REF=1` the very first frame renders a 1024×512 DXT1 boot texture to the back
  buffer and is non-black (`mean ~[28,33,33]`, 66 % non-black). Frames 2+ are black.
- **Texture loading works.** A `D3DDevice_SetTexture` probe showed real, non-zero pixel data for DXT1
  (`fmt C`), `LIN_L8` (`fmt 13`) and `YUY2` (`fmt 24`) textures — the YUY2 ones even change every frame
  (video/animation). So the `.rba` decode and pointer rebasing are fine for those.
- **But the steady state never draws that content.** From frame 3 on, every frame is the same ~8 draws, all
  full-screen `prim 8/9, n=4` quads that form a bloom/post-process chain: they read/write the A8R8G8B8
  (`fmt 6`) render targets `7FF34D20`/`7FF34CE0`/`7FF34D00` and the 640×480 framebuffer, which are all
  zero, and the only draw back to the back buffer samples the (zero) bloom target. The loaded DXT1/L8
  textures are bound only in frames 1–2, never in the steady state.
- There are **no `D3DDevice_CopyRects` calls**, so nothing fills those render targets by blit either.
- Pad input (`SCRIPT` pressing START/A for thousands of frames) does not change the draw pattern.

So the missing piece is not the sampler or the format — it is the engine's **scene/frontend draw calls**.
The `D3DDevice_DrawVertices`/`DrawIndexedVertices`/`DrawVerticesUP` HLEs and inline `0x17FC`/`0x1818`
pushbuffer draws are the only paths that reach `g_backend->Draw`, and in the steady state only the
post-process quads arrive. The next step is to find where Conker submits the visible geometry (an inlined
library draw, a raw pushbuffer form `Walk()` drops, or a screen the game is stuck on) — see the
[reverse-engineering reference](REVERSE_ENGINEERING.md) §4 on pushbuffer draw encoding.

**Resolved — that was the bug.** The title interleaves two ways of talking to the GPU: state changes through D3D8 API calls (which the runtime services as host `HLE`s, applied immediately) and raw NV2A commands pushed straight into the pushbuffer (which the runtime only walks lazily, at the next draw/swap). So a command pushed *between* two state calls ran with the state of the *later* call.

The movie's present is exactly that shape (`0x252D80`): `SetTexture(0, video)` → push `BEGIN_END(8)` + `INLINE_ARRAY(0x1818)` + `BEGIN_END(0)` → `SetTexture(0, NULL)`. By the time `Walk()` reached the pushed vertices the bound texture was already `NULL` (then the post‑process's), so the movie quad sampled the wrong texture. And `Walk()` treated the inline array method `0x1818` as an ordinary state write, so the primitive was submitted with **zero vertices** and dropped entirely. The decoded YUY2 surface (`fmt 0x24`) was therefore never bound in any draw — confirming it numerically: across the whole run only `fmt C`/`6`/`12` were ever sampled.

Two fixes:

1. `Walk()` now collects `0x1818` values as raw vertices and draws the primitive like `DrawVerticesUP` (`EndPush` already did this). The title pushes those arrays in a fixed 7 dword layout (position `xyzw`, `D3DCOLOR`, texcoord `xy`) while the bound program's declaration reads the same stride with an `XYZ` position, so the draw is given a layout matching the array (`0x42`/`0x40`/`0x22` at 0/16/20, stride 28) — the same stride the 2D image helper already passes.
2. The state `HLE`s (`SetTexture`, `SetVertexShader`/`SetPixelShader`, `SetStreamSource`, `SetIndices`, `SetRenderTarget`, `SetViewport`, `SetScissors`, `SwitchTexture`) now `Walk()` the pending raw commands *before* applying their own state, which restores the ordering the hardware would have.

The FMV decode itself was never the problem: `0x252D80` reads the XMV frame out of the software decoder's buffers (`[0x779BA0]`/`[0x849C50]`, 640×480 `YUY2`, `fmt 0x24`) and pushes the overlay quad. Those surfaces carry studio‑range YUV (no `0x00` or `0xFF` byte in 600 KB), i.e. real frames.

Follow-up traced the frame path: the frame render is `0x2A3FD0` (called from the frame loop at `0x2A4985`,
`Swap` at `0x2A499B`); it calls the post-process entry `0x1D95D0` at `0x2A4125`/`0x2A4556`. The engine state
globals stay fixed (`[0x849C60]=2`, `[0x779B94]=1`, `[0x84A144]=0` object count), the `ExQueryNonVolatileSetting`
traffic is a startup burst only (index `0x11` = `XC_MISC`), and a gdb snapshot shows the guest main thread
running the normal per-frame loop (`0x42DF42 -> 0x2A3670 -> 0x2A3F10 -> 0x2A4610 -> ... -> 0x419E90`), not
blocked. So the scene render is simply never reached/submitted, i.e. a frontend state-machine question. The state-2
path does call a 2D UI primitive (`0x1EB210`, which reads the device render target and calls the D3D internals
`0x53B830`/`0x53FC70`/`0x53FFE0`) — that is the most likely place the missing UI draws are built. XMV is used
for the intro (the transition manager `0x2529xx` calls `0x4E2531`), but the XMV code touches no NV2A/APU MMIO, so
it is a software decoder and is not the blocker.

## Progress since the previous blocker (the post-FMV crash)

With the frontend's draws reaching the backend, the harness now crashed at the **end of the launch FMVs**
(~67 s of guest time, `BootIntro.xmv`): the guest raised a null read inside the D3D8 library body `0x53AE30`,
reached from the frontend UI path (`0x1E96F0 -> 0x8003C -> 0x3F0270 -> 0x3F01F0 -> 0x3F3510`). Two mislocated
globals were the cause, both the same shape as the earlier `D3D_DeferredTextureState` bug:

| # | Fix | Where |
|---|-----|-------|
| 1 | `0x53AE30` reads `[D3D_pDevice+0x30]` and dereferences it; that is one of three fields the library's own `D3D_DeviceInit` (`0x53E5D0`) points at a 0x60-byte NV2A record block. The port skips that init, so the field is null. The runtime now backs the block (`+0x30`, `+0x2488`, `+0x248C`, `+0x2490`) with zeroed guest memory | `runtime/xdk/d3d8.cpp` |
| 2 | `D3D_Streams` was `0x547160`, but Conker's `SetStreamSource` (`0x535F00`) stores stride at `[n*12 + 0x547158]` and the vertex buffer at `[n*12 + 0x547160]` — so `0x547160` is entry 0's buffer field and the real table base is `0x547158`. With the wrong base the runtime's `SetStreamSource` HLE wrote the stride over slot 0's buffer pointer (`0x12`), and the next frame crashed dereferencing it | `config/game.toml` |

After both, the harness runs past 67 s into the frontend menu (`T:\preferences.bin`, `UIFrontend`, `UIPause`)
with ~2× the draws and no crash; the Windows/Proton build reached the same menu.

### Skipping the intro FMVs (`XBE_SKIP_FMV`)

The FMVs are paced in real time by the XMV decoder, so they gate every iteration for ~67 s. Scaling the kernel
clocks (`KeQueryPerformanceCounter`, `KeInterruptTime`, `KeTickCount`) or the vblank thread did **not** change
their duration, and simply reporting the file missing (`NtCreateFile` not-found) makes the title hang. The
decoder's own error path is the one the transition manager handles: with `XBE_SKIP_FMV` set, `0x4E2801` (the
XMV per-frame update) returns `-1`. The manager's open loop (`0x252470`) treats that as "movie failed" and leaves,
and its playing state (`0x2529xx`) treats it as done and closes/releases the decoder on its normal path. The
frontend is then reached in ~0.3 s instead of 67 s. The hook is `X86_FUNC(sub_004E2801)` in
`src/game/hooks.cpp`; `run-proton.sh --skip-fmv` sets the variable for the Windows run. (Note `0x4E2801` is
`stdcall` — `ret 0x10` — so the hook pops 4 arguments.)

## The frontend 3D scene renders (2026-10-15)

The scene was black because the engine drives the NV2A **directly** for nearly everything the scene needs, and the
runtime only listened to the D3D API. Several fixes the previous handoff described as done were **not in the tree**
(count-0 slots, the `DrawUp` stale layout, the chip program); they are re-implemented here. Each fix below is "last
writer wins" between the D3D path and the engine's raw writes, which is how the chip behaves.

| # | What the engine writes raw | Runtime fix (`XboxXBERecomp`) |
|---|---|---|
| 1 | vertex programs: `SET_TRANSFORM_PROGRAM` (`0xB00`), `_LOAD` (`0x1E9C`), `_START` (`0x1EA0`) | `d3d8.cpp` keeps the chip's 136-slot program memory; `BackendDraw` swaps the bound shader's program for the chip's (start → FINAL bit) when an upload/start came after the last `SetVertexShader`. `Load/SelectVertexShader` HLEs implemented. **This is what made geometry appear.** |
| 2 | transform constants continue across packets | `0xB80..0xBFC` advance the load pointer like xemu (and `0xC00..0x14FF`, the light methods, no longer land in `vsc`) |
| 3 | `VERTEX_DATA_ARRAY_FORMAT` count 0 = "no array" | `vertex.cpp` tests the count, not the type (also stopped dropping D3DCOLOR, type 0) |
| 4 | stale inline formats under `DrawVerticesUP` | `DrawUp` hides inline formats for its own array |
| 5 | `DRAW_ARRAYS` (`0x1810`) | decoded with xemu's contiguous-run join; raw draws use `base_vertex = 0` |
| 6 | the whole register combiner (`0x260..`, `0xAA0..`, `0x1E40..0x1E60`, `0x288/0x28C`), `SHADER_STAGE_PROGRAM` (`0x1E70`) | `MakePixelKey` reads the combiner from `simple[]` (SetPixelShader already loads its def there); a raw structural write binds a `RawCombiner` object (handle `0xFFFFFFFF`). **Textures/lighting appeared with this.** |
| 7 | the fog unit (`0x2A4` enable, `0x29C` mode, `0x9C0/0x9C4` params) | `State::chip_fog` + `ChipFogConstants` evaluate xemu's fog factor (shader `fogfactor` modes 4-6). The engine's own final combiner is `lerp(R0, fog, fog.a)`, so "D3D fog disabled ⇒ 1" painted everything fog-coloured. |
| 8 | sampler state per stage (`0x1B08` address, `0x1B0C` control0, `0x1B14` filter, `0x1B24` border, bump env) | `RawTextureState` mirrors them into the title's own deferred TSS table (and `g_state.tss`), so `SyncDeferred` and inlined `SetTextureStageState` interleave correctly. Border colour white fixed the black projected-shadow pass. |
| 9 | the library deferred flush (`0x543170`) is hooked to a no-op, so the D3D fog / fixed-function combiner it would push never reclaimed the chip | runtime `d3d::DeferredFlush(dirty)` (called from the game hook) and `FlushForDraw` in the draw HLEs: dirty `0x2000` (fog, body `0x5421D0`) ⇒ D3D fog wins; dirty `0x800` (FF combiner, `0x545670`) with no PS bound ⇒ drop `RawCombiner` |

Harness additions: the null backend answers visibility reports as "visible" (it used to leave them pending, so the
title culled what it tested); the draw list prints `start/idx`, program size/hash and all inline binds;
`XBE_DUMP_PROGRAM=1` logs each distinct chip program (`tools/vsh_disasm.py run.out [hash]` disassembles them);
`DUMP_VS` prints texcoords/fog/inputs/constants; `TRACE_PROG=<hash>` (+ `TRACE_BLENDED=1`) traces combiner
registers of draws running a program, wherever they land (frames are not reproducible, so pixel traces miss moving
objects); the combiner trace includes the final combiner.

### Hitches, frame rate, aspect (2026-10-15)
- **~300 ms hitches** were `Walk()` spinning to its 4M-packet guard: a static block jumping "back" to the scratch put
  pointer (`t == end`) or to a stale address past it was followed as physical RAM, through stale scratch and the zeroed
  low RAM and its ring wrap (`807FE000 -> 80000000`). Now `t == end` ends the walk and a target past `end` resumes at
  the last jump site's `home`. Found with the harness `STALL_TRAP=<ms>` watchdog under gdb; `WALKGUARD` logs any guard
  hit; `d3d: frame N took X ms` logs frames over 250 ms and the D3D12 backend logs compiles/pipelines over 20 ms.
- **30 fps**: Conker's second `CreateDevice` asks `FullScreen_PresentationInterval = 2`; `Swap` now paces to
  `60 / vblanks` (render state `PRESENTATIONINTERVAL` overrides when set), capped further by `fps_cap`.
- **aspect**: new `aspect` setting in `settings.toml` (`"auto"` = 16:9 on a 3:2+ output, `"4:3"`, `"16:9"`); it
  picks the TV the console reports at boot and the letterbox. `winrun/settings.toml` sets `"4:3"`.

### Animated meshes flashing out (fixed, 2026-10-15)
Characters and doors are culled with GPU visibility tests (the reader is `0x313C20`; the library's `0x539BA0` marks a
slot pending with status `-1` before `GET_REPORT`, and a pending test keeps the previous result, so latency is not a
problem). The tests are D3D `DrawIndexedVertices` boxes (vs `B0001431`, a 24-index quad list) drawn at the start of
the frame against the previous frame's depth. The HLE'd D3D draws kept the engine's raw `VERTEX_DATA_ARRAY_FORMAT`
binds from the last skinned draw, so a box drawn after one read its vertices through the skinned layout, collapsed to
a point and counted 0 pixels; worse with camera motion (more interleaving). `DeclarationBinds()` now clears the inline
formats in `DrawVertices`/`DrawIndexedVertices`, as their library body's own binds would. Measured on Proton: large
objects (up to 230k px) reading 0 went from 17 flash events per 45 s to none (the remaining ones are 1-20 px objects at
occluder edges). Tools: `XBE_TRACE_REPORTS=1` (zero reports, flash signatures, skipped counted draws),
`XBE_REPORTS_VISIBLE=1` (every test passes), harness `REPORT_LAG=N`, `REPORT_ZERO=1`, real counts on `REF` frames.
Note: release builds only write `LOGR`/`LOGE` lines; opt-in traces must use `LOGR`.

### Menu visual glitches (fixed, 2026-10-15)
- **Flat meshes** (bottles, counter flap, faces, eyes) were drawn entirely in fog colour: Conker's final combiner is
  `fog.a*fog.rgb + (1-fog.a)*R0`, and `SyncDeferred` handed the fog unit back to the (disabled) D3D fog, factor 1,
  whenever a D3D fog state changed. A D3D state only marks fog dirty; the flush (`DeferredFlush`/`FlushForDraw`) decides.
- **Character lighting**: characters run `00A46AF68A6652C9`: `oD0` = SH ambient (`c70..c81`), `oD1` = key light
  (`c83`/`c84`) times 1/d² blob shadows (`c50..c61`), `oT2` = normal lookup into a per-character 32x32 lighting map.
  (a) Conker writes `NV097_SET_SPECULAR_ENABLE` (`0x3B8`) raw; the renderer followed `D3DRS_SPECULARENABLE` and zeroed
  `oD1`. Raw now holds until the lighting flush (dirty `0x1000`, library `0x542BF0`). (b) The 32x32 maps (program
  `21751DC84070A8C5`, four `lit` lobes on a -1..1 grid) were black: the D3D draws cleared the inline vertex formats
  every time. The library (`SetStateVB` `0x543200`) pushes the declaration's formats only with dirty `0x10`
  (`SetVertexShader` sets `0x70`, `SetStreamSource` `0x40`, `0x70` on a stride change); the HLEs now do the same.
- **Depth of field**: the chain reads the depth buffer as colour, `LIN_R8G8B8A8` (`0x41`, A = stencil) for the focus
  mask and `LIN_A8B8G8R8` (`0x3F`, A = depth bits 23:16) for the sharp/blurred composite. D3D12 keeps depth on the
  GPU, so `DepthBytes` converts the depth target (depth + stencil plane) into the format's byte order when a 32 bit
  colour texture aliases it. Verified on Proton against xemu.
- harness: draw list shows stages 1-3, `c-38/c-37`, `spec rs/chip`; `DUMP_TEX=2` dumps every stage per REF frame,
  `DUMP_PS=<handle>` prints the translated combiner, `TRACE_VS=<handle>` traces by D3D shader handle. D3D12 F3
  capture: run with `XBE_CAPTURE_DIR=cap`.

### Analog sticks (2026-10-15)

Sticks were dead in game and menus while buttons worked. The controller init (0x4C950) marks the pads XGetDevices
reports as inserted (byte 0x13C of each 0x140-byte record at [0x806FD8]) and then swaps in a zeroed table; the stick
filter 0x4CF80 zeroes a pad not marked inserted. On the console pads enumerate later and arrive through
XGetDeviceChanges. A hook on 0x4C950 re-marks the opened pads in the new table. Harness pad script: `LS/LSD/LSL/LSR`.

### Curtains, cutscene skipping, profiler (2026-10-15)
- **Curtains** (Berri's room) drew flat fog colour. The library's fog flush (`0x5421D0`, dirty `0x2000`) pushes
  `FOG_ENABLE` and, when on, `FOG_MODE`/`FOG_PARAMS` worked out from the D3D fog states *at that moment*, plus the
  final combiner (`0x288` = `0x130C0300` fogged / `0x0C` plain, `0x28C` = `0x1C80`) unless the bound pixel shader has
  its own (device `+0x784` the shader, `+0x788` its final inputs). Conker changes `D3DRS_FOGENABLE` and then draws from
  its own pushbuffer with no flush in between, so the chip keeps the old push. `ApplyFlush` now computes that push
  (`PushFog`: linear `p0 = end*k+1, p1 = -k`, `k = 1/(end-start)` or 8192; exp `1.5, density*-0.0902`; exp2
  `density*-0.2123`) into the chip fog state, which the renderers always read after the first flush.
- **Skippable cutscenes**: `0x4FF00` is the per-frame "may LT end this in-engine cutscene" test. The title allows only
  cutscenes seen this playthrough (`0x4EBE0`, a 64 bit mask per entry at `0x765608`, bit set when a cutscene ends at
  `0x50877`), never mode `0x18` cutscene 1 (the throne), only cutscene 5 in mode `0x1D`, and not in the last half
  second. The `skippable_cutscenes` setting (F1 menu, applied live; `cutscenes::g_skippable`) or
  `XBE_SKIPPABLE_CUTSCENES` (`run-proton.sh --skippable-cutscenes`) lets LT skip any of them. Verified in the
  harness: a new game skips through the throne, Berri and the next scene into the first level.
- **Function profiler**: build the generated code with `-DXBE_FN_PROFILE` (a separate build dir, e.g. a copy of
  `out/build/win`'s cache with that in `CMAKE_CXX_FLAGS`); every recompiled function counts its calls, each frame's
  counts are summed by whether pad 0's left trigger is held, and F5 writes `fnprof_NN.txt` ("held idle name", after a
  `frames held idle` line). This found the skip code. Swap it in as `winrun/Game.exe` and launch with `--no-build`.
- D3D12 F3 capture now lists per draw the vertex inputs (stream, type, stride, first vertices), the pixel shader key,
  stage 0/1 samplers, fog state and final combiner, and a vertex program's constants on its first draw.
- harness pad script to a new game: FMVs on, `12500 A` (skips the intro FMV), `13500 LEFT`, `14000 A`, `15000 A`; the
  throne cutscene starts around frame 37000-42000.

### Fur (2026-10-15)
Characters are static push buffers loaded from the `.rbm` (the 576-byte reserved NOP first), entered from the scratch
ring with a jump and leaving with one. Each section has several material blocks; the engine picks one per pass by
running the buffer through the library's `RunPushBuffer` (`0x53AE30`, from the node renderers in the vtables at
`0x753DC0..`, e.g. `0x3F3510`) with a D3DFixup that patches the blocks' jump words (`0x53AD90` copies the queued
fixups). The runtime reads the stream lazily, so every run of a buffer in a frame read the last run's patches and the
fur passes were drawn as the base pass. A hook on `0x53AD90` walks what is queued before the patches land
(`d3d::FlushPending`). The fur program (`1B4CFC40E338AEDB`) offsets each vertex along its normal by `c85.x` times
`v5` (a per-vertex length byte); stage 1 is the 256x256 fur mask.

### Shadow flicker (2026-10-15)
Character shadows (e.g. under Conker's leg in the throne cutscene) alternated every frame on D3D12. The engine runs its
visibility tests only once the previous results are in; D3D12 answers them a frame late, so tests ran every other frame.
On test frames the sceptre's shadow caster (a rigid program, matrices `c13..c38`) drew with matrices from other draws and
smeared a bar across the 128x128 shadow map. Cause: the HLE of `SetVertexShaderConstantNotInlineFast` (`0x536C00`) wrote
constants immediately without walking the pending raw push, so raw constant uploads queued before the call landed after
it. The library pushes `LOAD` + data in order and leaves the load pointer past its last register; `LoadConstants` now does
the same. Harness: `REPORT_LAG=1` reproduces the test/no-test alternation, `LIST_CONSTS=1` lists every constant.
D3D12 captures: `XBE_CAPTURE_FRAMES=N` (N frames per F3), `XBE_CAPTURE_VS=<handle>` / `XBE_CAPTURE_RT=<surface>` (targets
around each matching draw).

### Frame rate and aspect (2026-10-15)
Ported from the community XBE patches (widescreen; theboy181's work-in-progress 60 FPS), which `60fps/` (the Xbox One
backward-compatibility patcher) carries over to that runtime's translated code. Its `REVERSE_ENGINEERING.md` §6-7 has
the XBE diff: a `mulss xmm1,[4/3]` inserted at `0x64468`, and `mov edx,2` -> `1` at `0x1F6B22` and `0x2A9DCB`. Here
they are hooks (`src/game/display.cpp`, two `[[hook]]`s in `game.toml`), applied live from the F1 menu.
- **Timing.** `0x2A78D0` builds the present parameters from `edx` (`FullScreen_PresentationInterval`: 2, or 3 in a
  3-4 player split screen) and keeps it in the byte `0x6C0268`; `0x3FB2D0` re-creates the device from a second block at
  `0x7F9ECC`. The frame timer `0x2A7B60` (once a frame, from `0x2A48C9`, before render and `Swap` at `0x2A499B`) sets
  `[0x7FA21C]` dt (s) = TSC microseconds (`rdtsc*3/2200`) * 1e-6, or ticks/60 when `[0x7819D5]` (fixed step, set by
  the multiplayer setup) is set, clamped to 1 s, and `[0x7FA214]` = 1/dt; and `[0x7FA20C]` ticks = interval byte +
  `[0x7F9F30]` (missed blanks, from the swap callback `0x2A8020` at `device+0x1DB4`), at most 60. `[0x7F9F5C]` is
  60.0 (ticks per second), `[0x7F9F58]` 1/60. ~400 places step by ticks (`counter -= ticks`, `fade += ticks*k`); none
  divides by them. Single player runs on measured dt, which is why the XBE patch (interval ONE, one tick a frame) works.
- **The port**: `frame_rate` (30 / 60 / 120, default 60; `XBE_FRAME_RATE`). Above 30 the hook before `0x2A7CBC` takes
  ticks from the frame's microseconds, rounded with the remainder carried (one a frame at 60, 1/0 alternating at 120,
  right after a hitch), so tick-stepped things keep real time at any rate; fixed step mode never gets a tickless frame
  and is held to 60. A wrapper on `0x2A7B60` limits the rate before the timer reads the clock and writes
  `D3DRS_PRESENTATIONINTERVAL` (`0x549BF4` in the state shadow; only the skipped library init writes it) as
  IMMEDIATE, so the runtime's `Swap` pacing only applies `fps_cap`; at 30 it writes 0 back (the device's interval).
  Not proven: per-frame logic that ignores both clocks (the XBE patch's known risk at 60, more at 120); FMV timing.
- **Aspect**: before `0x64468`, `xmm1` = `[camera+0x260]` (aspect, 4:3) and `[camera+0x264]` is the field of view; the
  hook scales `xmm1` by display/(4:3) (the XBE patch's fixed 4/3 is the 16:9 case): Hor+, same vertical view. The frame
  is still 640x480 and the window shows it at that shape (`GameInfo::aspect`, `app::FrameAspect`), so the HUD and menus
  stretch as with the XBE patch, and the horizontal resolution is the height's scale (raise Render %); a per-axis
  render scale in the D3D12 backend would fix that (`w/gw` is assumed uniform in ~10 places). `aspect` takes any
  `W:H`; the harness keeps 4:3 unless `XBE_ASPECT=W:H`, and 30 FPS unless `XBE_FRAME_RATE`. `XBE_TRACE_TICKS=1`
  logs each frame's microseconds and ticks; `display:` lines log rate and aspect changes.

### Shader recording and warm-up (2026-10-16)
Every D3D12 shader and pipeline is made on the frame that first draws with it (FXC, then the driver), and nothing
was kept between runs. Now `runtime/gpu/shader_record.{h,cpp}` records each one the first time it is made, and the
next launch makes them in the background from the start.
- **What a record holds** is the generator inputs, not HLSL or bytecode, so records stay valid when the generators
  change. A vertex shader record is a `VertexEntry`: the program hash (`VertexShader::hash`, 0 for fixed function),
  regs, decode, `FixedKey`, pretransformed and quad. A pixel shader record is the `PixelKey`. A pipeline record is a
  `PipelineEntry`: its shaders' record ids and the `PsoKey` with `vs`/`ps` zeroed, since those are source hashes and
  change with the generators.
- **Files beside the exe.** `shaders.rec` is shareable and holds no microcode. `shader_programs.bin` maps program
  hash to words and stays local. The split is needed because Conker's vertex programs can't be referenced any other
  way: they reach the chip inline (`SET_TRANSFORM_PROGRAM`), never through `CreateVertexShader`. Of 63 frontend chip
  programs (`XBE_DUMP_PROGRAM`), only 2 occur verbatim in `default.xbe` or `dvddata/aid`, so the rest are assembled
  at runtime or stored compressed.
- **Warm-up.** `StartWarmUp` runs from `Init`. It compiles the records on `hardware_concurrency/2` threads (1 to 6)
  at below-normal priority, shaders first and then pipelines. The results sit in `warm_`, which `Compile` and
  `EnsurePipeline` check under a lock on a miss. A vertex shader whose program isn't in `shader_programs.bin` is
  skipped, along with its pipelines. The log reports `shaders: made N of M recorded shaders and P of Q pipelines
  ahead in T ms`.
- **Validity.** `kRecordLayout` covers the record sizes plus `kRecordRevision`; a file with another layout starts
  over. Bump the revision when a record keeps its size but changes meaning.
  - The `PsoKey`'s `zslope` includes the render scale, so a depth-biased pipeline recorded at another Render % doesn't
    match and is rebuilt as before.
  - `Instance() > 1` reads the files without writing them.
  - `shader_warmup = false` in `settings.toml` turns the warm-up off; recording stays on.

### MSAA (2026-10-16)
`msaa` in `settings.toml` (1, 2, 4 or 8; F1 menu "MSAA") multisamples the targets the size of the title's frame
(`guest_w_` x `guest_h_`). Shadow maps and the other smaller targets stay single sampled. Changing it applies live,
like Render %: `RemakeTargets` makes every target again and draws the old picture into the new one. Dropping them
instead, as before, turned the bar's bottles into noise: their environment map is drawn once per level and read from
its target, and guest memory never held it.
- **Two copies per target.** A multisampled `Target` keeps `res` (single sample) and adds `ms`. `res` stays what
  everything reads: textures, `Feedback`, `DepthBytes`, copies, read-backs, captures and the present.
- **What goes to the samples.** A draw goes to the samples only when every bound target has them; otherwise it goes
  to `res` (for example, a colour format the GPU can't multisample).
- **Which copy is newer.** `ms_ahead` and `res_ahead` say which copy is newer:
  - `ResolveSamples` runs before `res` is read or partly written. Colour uses `ResolveSubresource`. Depth and
    stencil take sample 0: depth through `SV_Depth`, and stencil one bit per pass (clear, then 8 passes with REPLACE
    and a one-bit write mask), because stencil export isn't available everywhere under vkd3d-proton.
  - `SpreadSamples` copies `res` back to the samples before the next draw or clear, after a CPU upload, a copy, or a
    depth copy into the target. Conker's depth of field masks by stencil, which is why the stencil travels too.
- **Pipelines.** `PsoKey::samples` carries the sample count into recorded pipelines. Adding it changed
  `kRecordLayout`, so `shaders.rec` from before this change starts over once.
- **Limits.** Occlusion counts divide by the samples, as they did by the render scale. Alpha-tested edges (foliage,
  fur) don't get smoother; that would need alpha to coverage. With a renderer extension, MSAA stays off.

### Grass popping out at medium distance (2026-10-16)
Grass patches vanished all at once at medium distance and came back up close. The engine draws each patch with one of
two fixed-function variants (TrainingLevel, alpha tested, 64x256 DXT): near ones with a wind program (`vs B0101651`
on Proton, `B0001651` on the harness), and the rest with a plain transform (`B0101661`) whose alpha is
`texture * diffuse * TFACTOR.a`, the fade. Both are submitted every frame, so this was never culling (and
`XBE_REPORTS_VISIBLE`, aspect, frame rate and render scale made no difference). The fade comes from `0x83F30`, an
inlined `SetRenderState(TEXTUREFACTOR)`: it pushes `NV097_SET_COMBINER_FACTOR0` (`0xA60`) raw and keeps it in the
library's `PSCONSTANT0_0` shadow (`0x549A20`, render state 10). The image has no `SetRenderState_TextureFactor`,
so `D3DRS_TEXTUREFACTOR` (148) never changes, and the renderers read `tfactor` from it: alpha 0, alpha test failed,
patch gone the moment it switched variant. On the chip TFACTOR is factor 0, so `State::chip_tfactor` now makes a raw
`0xA60` the texture factor until a D3D `TEXTUREFACTOR` or a pixel shader definition takes it back (the `chip_fog`
rule). Found with two D3D12 F3 captures (patch showing, patch gone): same 12 grass draws, one patch moved from `651`
to `661`. Ruled out on the way: the `CONTROL0` LOD clamps (Conker only ever writes 0..15.996).
Harness note: walking in TrainingLevel (pad `LSD`) hit a guest divide error at `0x53A833` (D3D library); not
investigated.

**Wind on distant grass** (enhancement, `grass_wind` setting, default on; F1 "Wind on distant grass";
`XBE_GRASS_WIND=0` off). The patch renderer `0x1A9540` binds the wind program from `[0x6B89A4]` at full strength
and the plain one from `[0x6B89DC]` while `[patch+0x5C] < 255`. Both declarations read the same stride 12 vertices
(texcoord as `v7` / `v2`) with the same constants, so a wrapper in `src/game/hooks.cpp` points `[0x6B89DC]` at the
wind handle for the call and puts it back; the fade (stage 1 `alpha * TFACTOR`, scaled alpha reference) is unchanged.

### The castle window glow (2026-10-16)
The lit window of the Panther King's castle (opening cutscene) drew as flat olive with no halo; the castle also
lacked its glow in lightning. Conker's glow pass is at the end of the frame: a 256x256 mask (`811FC180`) from the
scene and the depth buffer read as `LIN_R8G8B8A8` (A = stencil), downsampled, blurred four times additively
(`8123C200`) and added onto the scene. The **stencil is the glow level**: 0 is discarded (alpha kill on stage 1),
1..15 weight by scene alpha, 16 and up give `(k - 15) / 255` (window `0x30`, castle `0x20`). The scene pass
(`0x69F8B`) sets the ref and turns the stencil on (ALWAYS, REPLACE), draws the level through `0x29CEE0` and turns it
off (`0x6A377`). Three things were wrong:
- The render state HLEs (`SetRenderState_Simple`, `SetRenderStateNotInline`, the complex setters) changed state at
  once, while the raw commands pushed before them were only walked at the next draw: the level drew after the
  stencil was off again. They now walk the pending push first, like the texture, shader and target setters.
- `D3D_InitDefaultState` isn't found in this image, so the stencil ops kept the chip's reset value 0 (`GL_ZERO`).
  Conker never sets the depth fail op: the sky, drawn after the castle and failing depth behind it, zeroed the
  window's stencil. `ApplyDefaultStates` now seeds D3D's stencil defaults (ops KEEP, ALWAYS, masks `0xFF`).
- A raw `NV097_SET_STENCIL_TEST_ENABLE` / `_OP_FAIL` now reaches `kRsStencilEnable` / `kRsStencilFail` (not needed
  here, but the renderers took those two from the render states only).

The D3D12 F3 capture now prints each draw's stencil state (`sten func ref mask wmask ops`, then the chip's own enable
and fail op), a clear's stencil value, and `note stencil enable <v> from <caller>` lines through the new
`Backend::Note()`, in order with the draws.

### The soldier's flag drew one-sided (2026-10-16)
The flag on the soldier's pole in the throne room vanished or showed a sliver from behind (draw with texture
`82429B80`, skinned program `B0101701`, every vertex on bone 0). The renderers culled by the `CULLMODE` /
`FRONTFACE` render states only, while the title also writes `NV097_SET_CULL_FACE_ENABLE` (`0x308`), `_CULL_FACE`
(`0x39C`) and `_FRONT_FACE` (`0x3A0`) raw. Raw writes now set the render states as the chip reads them (`ChipCull`:
the front face's winding when culling `GL_FRONT`, the other one for `GL_BACK`), and the `CullMode` / `FrontFace`
HLEs fill the three registers as the library's `0x537070` / `0x5370F0` push them.

### Open
- **missing loading screen**: the game freezes where a loading screen should show (after the boot splash, when starting
  a new game, between cutscenes); it continues once the level has loaded.
- puddles show Conker's reflection but not the environment (re-check on D3D12).
- `notes/nv2a_methods.txt` predates jump-following; the live vocabulary is 238 methods (re-dump with
  `XBE_DUMP_METHODS=1`, grep `^METHOD`).
- the restored original `default.xbe` differs from the old copy (`iso/default.xbe_`) in 44 bytes (header plus a few
  bytes at `0x5446A`, `0x1E6B23`, `0x299DCC`); it was re-recompiled and runs identically. Those are the file offsets
  of the community widescreen + 60 FPS patch (guest `0x6446A`, `0x1F6B23`, `0x2A9DCC`): the old copy was the patched
  XBE. See [Frame rate and aspect](#frame-rate-and-aspect-2026-10-15).

## Previous blocker (the frontend 3D scene, history)

The menu boots and its **2D overlay renders** (text, sprite particles), but the **3D scene is black** on the
D3D12/Proton path and in the harness reference renderer (captured frames are ~1 % non-black). The geometry is
fine: a draw dump (`CAP=N`, `cap/draws_*.txt`) of a frontend frame shows the scene submitted to the back buffer
(`rt=03ED4000`) with real vertex data (`prim=5`, e.g. `n=1875`, position `(-314.5, 3.75, 1362.5)`, stride 12),
real vertex-shader constants (`c0..c2` world rotation+translation, `c3..c6` projection) and loaded textures. The
reference renderer's `TEST_OPAQUE=1` (a temporary hack that ignores the scene's blend) draws the correct scene
**silhouette**, so the vertex shader, transforms and rasterisation are right.

What the per-pixel probe (`PROBES=x,y`, frame-tagged) shows for a scene draw is
`src (1,1,1,1) -> (0,0,0,0) blend 1 306 0`: the combiner shades the geometry **white**, and the engine's own
blend is `DST_COLOR`/`ZERO`, i.e. `dst = src*dst`. White lighting is a **no-op that preserves the destination**,
so the scene pass is a *lighting multiply* — it expects the destination to already hold the frame's colour
(an albedo/colour pass), which the captured draw stream never writes. `XBE_DUMP_METHODS=1` proves the blend is
the engine's own state (it pushes `0350=0x8006`, `0344=0x306`, `0348=0` just before each scene draw, while the
2D overlays push `0344=0x302`, `0348=0x303` and render), so the fault is a **missing/empty colour pass**, not a
mis-decoded blend.

Related evidence gathered while narrowing this:

- **Textures.** `DUMP_TEX=1` writes every stage-0 texture to `cap/tex_*.tga`. The file-loaded ones decode fine,
  but the scene's `7FF40E40`/`7FF40E80` and the post-process `7FF34D00`/`7FF34D20`/`7FF34CE0`/`D005FD44`
  (640×480) decode **all-zero**, and none of them is ever a render target in the captured stream.
- **Clears.** The frame-begin hook (`sub_00538D50`, which mirrors the original `0x538DE0`) calls
  `Clear(0, NULL, 3, 0, 1.0, 0)`. The runtime reads `flags & 0xF0` as the colour-channel mask and bits 0/1 as
  z/stencil, so `3` clears **z+stencil only**; the only colour clears (flags `0xF0`) are the engine's, to
  **white**, on `0275F900`/`02767980`/`0276FA00`. Worth double-checking that this flag interpretation is right,
  since it decides whether the back buffer's colour is ever cleared.
- **Raw-method vocabulary.** `notes/nv2a_methods.txt` lists all 153 methods the title pushes
  (`XBE_DUMP_METHODS`). Several ranges fall into `Walk()`'s generic `g_state.simple[method>>2]` state but are
  almost certainly not simple render states: `0x0A60-0x0A9C` (16 × `0x3F3F3F3F`, collides with the runtime's
  `kMPsConstant0/1`), `0x0B00-0x0B7C` (32 dwords), `0x1760-0x179C` (the NV2A COMBINER_COLOR ICW/OCW, which the
  runtime reads from other offsets), `0x1B00-0x1BCC`.

### Frame structure (measured)

`cap/draws_<frame>.txt` now interleaves the clears with the draws, so a frontend frame reads:

1. `CLEAR rt=03ED4000 zs=03C7C000 flags=F3 color=00000000` — the back buffer **is** colour-cleared, to
   **black** (`0xF0` is the colour-channel mask, bits 0/1 are z/stencil — confirmed against the library's own
   `D3DDevice_Clear` at `0x53B9F0`, which tests `cl,0xf0` for colour and `cl,1` for z).
2. `CLEAR rt=0275F900/02767980/0276FA00 flags=F0 color=FFFFFF` — three offscreen surfaces cleared **white**.
3. ~2000 draws rendering the 3D scene **opaque** (`blend=0`) into **40+ offscreen render targets**
   (`0277xxxx`/`0278xxxx`/…), 32 draws each, using the frontend camera (`proj 1.34019 … 2.22808`,
   `c0=1/128`), e.g. `vs=B0001361 ps=9D7780 rt=0277EF80 tex0=7FF40E80`.
4. The back-buffer draws: the three `vs=B00C8C91` multiply draws, then particles/UI
   (`B00014C1`, `B0001421`, `B00015F1`, `B00C8D*`, `B00016D1`).
5. A post-process chain of full-screen passes ending at the back buffer
   (`B00014F1 → 011FC180`, `B0001511 → 0125C280`, `B00014D1 → 0123C200`, `B00014A1 → 03ED4000`).

### Render-target objects

`SetRenderTarget`'s args are `(rt, zs)` with `rt` a real `D3DPixelContainer` — `Common = 0x01050001`
(`& 0x70000 == 0x50000`), `Data` at `+4`, `Format` at `+12` — so `Describe` handles it fine. From that the
targets are:

- the pool: **38 × 32×32 `fmt 6` (A8R8G8B8)** surfaces, stride `0x1080` (4096 data + 128 header) — tiny effect
  buffers, *not* screen-sized, so they are an unlikely home for the menu's 3D scene;
- the three cleared-white surfaces: **128×128 `fmt 5` (R5G6B5)**;
- the bloom chain: `011FC180` 256×256, `0125C280` 128×128, `0123C200` 128×256 (`fmt 6`/`7`).

**Do not read a render-target object at `Present` time.** The title reuses those D3D objects, so by then
`R32(header)` is a bare refcount (`0x000006D7`) and `Describe` appears to fail. Logging the same check at
`Draw` time (`BADRT`) reports **zero** such cases — nothing is dropped. (An earlier revision of this note
wrongly claimed offscreen draws were being dropped through this.)

### Verified against xemu (use `xemu/` — the tree is in the workspace)

Cross-checked the runtime's method tables against `hw/xbox/nv2a/nv2a_regs.h`. All of these are **correct**, so the
engine really does set what the draw list shows:

- `0x304` BLEND_ENABLE, `0x33C` ALPHA_FUNC, `0x344` BLEND_FUNC_SFACTOR, `0x348` BLEND_FUNC_DFACTOR,
  `0x350` BLEND_EQUATION, `0x358` COLOR_MASK, `0x35C` DEPTH_MASK
- `0x306` = `BLEND_FUNC_SFACTOR_V_DST_COLOR` — so the scene draws' `blend=1 306 0` is genuinely
  `dst = src * dst` with the source from the combiner
- `0xA60`/`0xA80` = COMBINER_FACTOR0/1, `0xAC0` = COMBINER_COLOR_ICW — the runtime's combiner offsets
  (`kMPsConstant0/1`, `kMPsRgbInputs = 0xAC0`) are right, so the earlier "mismatch" suspicion was wrong
- `0xB80` = SET_TRANSFORM_CONSTANT — the runtime's vertex-constant base is right
- `0xB00` = SET_TRANSFORM_PROGRAM: the title uploads **vertex programs inline** (`0x0B00-0x0B7C`), which `Walk`
  ignores; the runtime instead builds programs from the HLE'd `CreateVertexShader`

Also: `D3DDevice_DrawVerticesUP` (0x53A600) is a normal stdcall with 4 **stack** args, and although the library
body adds bases from `CDevice+0xbcc/+0xbd0`, **args 2/3 are a real pointer and byte stride** in practice
(logged: `a2=D005FAB0`, `a3=0x1C`), so `HostPtr(a[2])`/`a[3]` is correct — that suspicion is closed.

### Method audit: what `Walk()` ignores, and whether it matters

Scripted the whole set: of the **153** methods the title pushes, **73 are handled or read** and **80 are genuinely
ignored** (nothing anywhere reads `simple[m>>2]`). The full list is reproducible by classifying
`notes/nv2a_methods.txt` against `xemu/hw/xbox/nv2a/nv2a_regs.h`. The meaningful ones:

| method | xemu name | live? | impact |
|---|---|---|---|
| `0x1720-0x175C` / `0x1760-0x179C` | VERTEX_DATA_ARRAY_OFFSET / FORMAT | **per draw** | was huge — `g_state.streams[]` went stale (now fixed) |
| `0x1900-0x1940` / `0x1980-0x1A00` | VERTEX_DATA2S / 4S_M | per draw | was huge — draws silently dropped (now fixed) |
| `0x0A20-0x0A2C` | SET_VIEWPORT_OFFSET | **per pass** | benign: always `0x3D000000` = 0.03125 (sub-pixel); scale comes from the `SetViewport` HLE |
| `0x0B00-0x0B7C` | SET_TRANSFORM_PROGRAM | once | inline vertex programs; runtime uses the HLE'd `CreateVertexShader` instead |
| `0x1B00-0x1BCC` | SET_TEXTURE_OFFSET/FORMAT/CONTROL0/BORDER | once | inline texture state; runtime uses the D3D texture object |
| `0x0384/0x0388` | POLYGON_OFFSET_SCALE/BIAS | once | depth bias — z-fighting risk |
| `0x0394/0x0398`, `0x38C/0x390`, `0x0290`, `0x1E6C`, `0x1D78/0x1D84` | CLIP_MIN/MAX, POLYGON_MODE, CONTROL0, SHADOW_DEPTH_FUNC, ZMIN_MAX/AA | once | mostly defaults/zeros |
| `0x0320-0x0338`, `0x30C` | LINE/POLY_SMOOTH, POLY_OFFSET_*_ENABLE, DEPTH_TEST_ENABLE | once | enables, all zero |

So the instinct was right that a lot is ignored, but the audit bounds it: **the only ignored state that was both
live and load-bearing was the vertex data/streams, and those are now handled.** The rest is init-time or
sub-pixel. It is still worth implementing the transform program and inline texture state for faithfulness, but
neither explains a black frame.

Also checked: the three restored full-screen passes (`prim=5 n=3`, a single screen-covering triangle — correct,
not a truncation) write `0275F900`/`02767980`/`0276FA00` **in place, sampling the same surfaces**, so they
preserve the cleared white. Dumped, those textures are still `mean=1` (uniformly white). Nothing in the frame
fills them with scene content, so the multiply still has a white source over a black destination.

### Command-stream jumps were ending the walk (fixed)

The audit's "ignored methods" list only covered *methods*. Auditing the command stream itself found something much
bigger: `Walk()` treated anything that was not a method header as the end of the buffer and stopped, **silently**
dropping everything after it (the only exit was a bare `break`, with no counter and no log).

Instrumented both silent exits (`XBE_TRACE_WALK`) and measured one 8-second run:

```
pushed=170600  stopped  dropped=35310736 bytes   lasthead=01F08E85 (jump=1 call=0 ret=0)
```

**Every single stopping word had `& 3 == 1` — they are NV2A JUMP commands.** xemu's pushbuffer decoder
(`hw/xbox/nv2a/pfifo.c`, `if (method_count) ... else { /* no command active */ }`) accepts five forms:

```
(word & 0xe0000003) == 0x20000000   old jump   target = word & 0x1fffffff
(word & 3) == 1                     jump       target = word & 0xfffffffc
(word & 3) == 2                     call       target = word & 0xfffffffc
word == 0x00020000                  return
(word & 0xe0030003) == 0 or 0x40000000   method header, bit 30 = non-increasing
```

`Walk()` only implemented the last one, so the title's hand-written inline push buffers — which chain their blocks
with jumps, exactly as the "everything is inline pushbuffers" hint suggested — lost the rest of every buffer.

**The targets are real blocks, not garbage.** Dumping one target as physical memory (`| 0x80000000`, the view this
file uses for guest data — a raw read of the unmapped low address segfaults) at `8244EE04` showed a 145-word NOP
packet (`0x42400100` = non-increasing method `0x100`, 144 zero values) and then real content:

```
8244F048: 0243EE80 0243F7C8 024405B4 02441844 0244218C 02442AD4 02442D28 0243EE80   <- 16 stream offsets
8244F08C: 00401760 00000444 00000635 00000845 00000444 00000416 00000114 00000425   <- 0x1760 + 16 formats
8244F0DC: 00041E9C 00000000 00800B00 00000000 00564203 0C36106C 2F200FF8           <- 0xB00 transform program
```

i.e. prebuilt state blocks holding vertex-stream bindings and NV2A vertex program words — the same state the
`0x1720`/`0x1760` decode above had to hand-reconstruct.

Implemented per xemu: jump and old jump re-target the stream, call pushes onto a 16-entry return stack with its
buffer limit, `0x00020000` returns, and a 4M-packet guard bounds the walk. A target inside the buffer the walk was
handed stays bounded by it; anything else is read as physical RAM (`0x80000000-0x84000000`).

Result: **170,600 stops → fewer than 700 per run**, no traps, and the frame structure is unchanged in shape (same
passes, same targets). It does not change what is on screen yet: it restores state the scene's draws depend on,
which is why it is a prerequisite rather than a fix. `XBE_NO_WALK_JUMPS` restores the old behaviour for
comparison.

**Measurement warning.** The frontend is *not* reproducible frame-for-frame: five identical runs (same binary,
same env, `CAP=800`) produced 322/264/388/… draws at frame 800 and reached 3960-4320 total frames, because the
title's animation is driven by the emulated timer and the run is wall-clock limited. So a single-run draw-count
A/B is meaningless here — compare structural properties, or averages over many runs.

**Consequence: the "init-only" methods are now live.** Following jumps means the blocks those jumps reach are
finally parsed, and several methods the earlier audit dismissed as written-once are now written per draw. The
clearest example is the inline texture state (`NV097_SET_TEXTURE_OFFSET`, method `0x1B00`), which nothing in the
runtime reads; in the draw list it used to be constant and now varies:

```
before:  514 draws, nvtex0=02767980 for every one of them
after:   240 draws nvtex0=01602B00, 22 draws nvtex0=02767980, 1 draw nvtex0=01D4A300
```

So the engine really does rebind textures inline, into `simple[0x1B00>>2]`, and `Walk` stores the value while no
consumer exists — the audit's item 3 is not benign after all, it was just unreachable. Note the addresses arrive
as **raw physical** values (`0x01602B00`, `0x01D4A300`, no `0x80000000`): anything that consumes them has to add
that view, exactly as the jump targets do (reading one raw segfaults). This is also what the original
`SwitchTexture` suspicion was pointing at — the HLE in `d3d8.cpp` drops a shadow texture header into
`g_state.textures[stage]`, but the game binds through the inline methods instead.

**Implemented, and currently inert.** `Walk` now builds a `D3DPixelContainer` from the inline offset and format
(`InlineTexture`, one header per distinct pair so `FindImage`'s decode-keyed cache stays warm) and points
`g_state.textures[stage]` at it. The format word needs no translation: the D3D header's Format field *is* the
NV2A word (colour 15:8, dimensionality 7:4, cube bit 2, mip levels 19:16, log2 sizes from bit 20), and the NV2A
colour codes line up with the D3D format numbers (`0x05` R5G6B5, `0x06` A8R8G8B8, `0x0C` DXT1, `0x28` V8U8).

Instrumented the allocations (`XBE_TRACE_WALK` prints `INLINETEX header=… offset=… format=…`) and checked whether
any reaches a draw: **none do.** In the frontend frame every stage is rebound by the HLE'd `SetTexture` *after*
the inline value is stored, so last-write-wins leaves the D3D object in place and the inline header is never
sampled. The consumption is still the correct model — it is what makes the two writers of one register set
agree — but it is not currently load-bearing, and it does not change the frame.

### Indexed geometry was being silently dropped (fixed)

With jumps followed, the method vocabulary grew from 153 to **238** — the earlier list was collected when the walk
stopped at the first jump, so everything inside the jump target blocks had never been seen. Diffing the new
methods against what `Walk` handles turned up `NV097_ARRAY_ELEMENT16` (`0x1800`) and `ARRAY_ELEMENT32`
(`0x1808`) falling through to `simple[]`. `REVERSE_ENGINEERING.md` §4.3/§4.5 is explicit that in Conker the
**faces come from pushbuffers as index arrays** ("A table opens on BEGIN and closes on END"; `INDEX_U16` read
"`<H` every 2 bytes"), and xemu agrees (`pgraph.c` `DEF_METHOD_NON_INC`, `ARRAY_ELEMENT16` stores
`parameter & 0xFFFF` then `parameter >> 16`).

Instrumenting both parsers showed the packets carry bit 30 (non-incrementing) and arrive in
**`D3DDevice_EndPush`**, not `Walk` — a 447-word packet is 894 indices:

```
IDX base=1800 m=1800 count=447 i=0 v=00910114 prim=6 bit30=1
ENDIDX prim=6 raw=0 verts=0 idx=899 stride=28
```

`raw=0 verts=0` is the whole problem: with no inline vertices the END fell through to `EndPrimitive()`, which
opens with `if (!prim || !count) return;` — so **every indexed batch was discarded without a log line**. These
are not small: the batches measured 899, 1431, 834, 643, 445, 367, 289, 248, 125 and 14 indices, all
`prim=6` (triangle fan) against a stream bound with stride 28.

Implemented in `EndPush`: accumulate `ARRAY_ELEMENT16` as two 16-bit indices (low half first) and
`ARRAY_ELEMENT32` as one (ignoring anything over 16 bits, which is not an index), clear them on BEGIN, and at END
issue an indexed draw — the backend already takes 16-bit indices (`backend.h`) and the vertices come from the
streams the title bound with `0x1720`/`0x1760`. The same branch was added to `Walk` for symmetry; the index
methods do not currently reach it, which was verified by instrumenting both call sites.

Result: the frame gains the indexed batches (draws with `n` up to 2743 now appear where the list previously
stopped at the inline-array sizes), four `d3d: inline indexed draw` log lines per run, and no traps.

Separate from the indexed draws, the walk was also still escaping into RAM after some jumps (~200-300 stops per
7 s, last heads like `0xFFFFFFFF`, `0x79FFFFFF`). That is the next section.

### The jump chains now resynchronise (fixed)

Chasing the last ~250 stops per run answered the "where does the chain go wrong" question with bounds, not
contents. Every stop carried `inmain=0` and sat **four bytes past the push pointer**:

```
WALKSTOP head=7FFFFFFF at 835FEFEC ... end=835FEFE8 walk=835FEFE4
WALKSTOP head=D8F00000 at 835F4958 ... end=835F4958 walk=835F4954
```

Tracing the jump history into a stop showed why. The title's frame script in the scratch buffer jumps *out* to a
static push buffer (an asset block in `.gpu`, around `0x817F_xxxx`-`0x8200_xxxx`), executes it, and the block
jumps *back* to the word after the jump site:

```
817EE6EC -> 817EE6F4   817EE75C -> 835FE008   835FE4FC -> 817EE760   817EEDA8 -> 835FE500
835FE9E4 -> 817EEDAC   817EF05C -> 835FE9E8   <- back in, then garbage
```

That is the intended design and it usually returns. But when a chain does *not* return, the walk kept going with
`limit` set to the whole of RAM, scanned forward, and re-entered the scratch **past `end`** — reading stale bytes
of the previous frame's buffer, which look like plausible method headers but are not. That is strictly worse
than the old behaviour: the stale words were executed as commands, so state and draws could be fabricated as
well as lost.

Fixed by tracking `home` — the word after each jump site that is inside the buffer the walk was handed — and, on
an invalid header while outside that buffer, resuming at `home` instead of stopping (bounded to 64 per call).
That is where the block would have returned to, so it restores the rest of the buffer rather than discarding it,
and it stops the walk reading memory the title never wrote.

Result: **stops 250 → 0** in a 7-second run, no traps. The resyncs are few and land usefully (one example pair
resumes directly at `end`, so the loop exits cleanly instead of breaking). All of it is behind `XBE_TRACE_WALK`.

### The scene's vertex layout comes from the stream, not the shader (open)

Chasing why the restored scene draws still contributed nothing found the vertex *fetch*, and the answer inverts
what the earlier session assumed. For a scene draw the harness now reports:

```
v0 stride 6 vb=B0001830 phys=01319000 data=81319000:
   0712FF41 FF880307 03080754 07430000 0000031F 035B0752 0790FCA8
```

and for a 2D draw that works:

```
v0 stride 48 vb=B0001360 phys=039E3400 data=839E3400: 0 0 1 3F800000 ... BCA00000(=-0.0195)
```

The scene's bytes are **packed shorts** — `0xFF41` = -191, `0x0712` = 1810, `0xFF88` = -120, `0x0307` = 775 —
plausible fixed point coordinates, and the bound stride of **6 is exactly SHORT3**, so it is *correct*. The
declaration is what is wrong: it claims FLOAT3 at offset 0, D3DCOLOR at 12 and FLOAT2 at 16 (span 24), a layout
belonging to a different shader.

That means `Walk`'s inline stream bind is half-implemented. `NV097_SET_VERTEX_DATA_ARRAY_FORMAT` (`0x1760`)
carries type (bits 3:0), component count (bits 7:4) and byte stride (31:8), but `Walk` keeps only the stride:

```cpp
W32(h + 4, v & 0x7FFFFFFF);            // 0x1720, the offset
g_state.streams[slot].stride = v >> 8; // 0x1760, and the type and count are dropped
```

So the fetch's per-input offsets and types can only come from the declaration — which for a title that sets its
vertex program inline through `NV097_SET_TRANSFORM_PROGRAM` (`0xB00-0xB7C`, still ignored, see the audit) is
whatever `g_state.vs` was left holding.

Tried and **reverted**: making `StrideOf` prefer the declaration when the bound stride cannot cover it. It looked
right (bound 6 against a declaration reaching offset 16 is impossible) but the measurement says otherwise — the
scene data really is 6-byte shorts, so the bound stride is the trustworthy one and the larger declaration made it
worse. `StrideOf` is back to `st.stride ? st.stride : layout.stream_stride[...]`.

The fix is to keep the whole inline format word and build the input layout from it for streams the title binds
inline, instead of relying on the declaration.

### A slot with count 0 is a constant, not a stream (fixed, 2026-10-15)

The one-line cause of the "random triangles". xemu's test for whether an attribute is a vertex array is on the
**component count**, not the type:

```c
if (!attr->count) { glDisableVertexAttribArray(i); glVertexAttrib4fv(i, attr->inline_value); continue; }
```

The driver binds all sixteen slots' offsets and writes formats like `0x02` - type float, **count 0** - to mean
"no array here". Testing the type (`fmt & 0xF`, which is 2, non-zero) instead of the count turned thirteen
constants into bogus zero-component vertex streams, feeding the scene's program garbage for `v3` through `v15`.
The layout for a chip-program draw went from

```
v0<-s0/35 v1<-s1/45 v2<-s2/44 v3<-s3/02 v4<-s4/02 ... v15<-s15/02
```

to

```
v0<-s0/35 v1<-s1/45 v2<-s2/44
```

which is the whole difference. `gpu/vertex.cpp:InlineInputs` now tests `(fmt >> 4) & 0xF`.

Related and now settled: those scene draws are **HLE'd `DrawVerticesUP`** draws with a programmable shader
(`vs=B0001381`, bit 0 set) - the engine's `BeginPush`/`EndPush` indexed path is never used in this title
(`IDXDRAW` count 0), so `0x1800`/`0x1808` index handling is not what draws the scene.

### Why the pure xemu attribute model draws nothing here (2026-10-15)

xemu takes the vertex layout from the per-slot attributes alone (`gl/vertex.c:pgraph_gl_bind_vertex_attributes`):
slot N is input register N, the address comes from the last `SET_VERTEX_DATA_ARRAY_OFFSET`, and the type, count
and stride come from the format word. Implementing exactly that here turns **every frame black**, and the reason
is the one asymmetry worth remembering: in xemu the D3D library bodies run, so `DrawVerticesUP`'s own body pushes
the matching `OFFSET`/`FORMAT` binds for the array it is about to draw. Here those entry points are HLE'd, so
their bodies never run and the binds are only whatever the engine's own raw pushes left behind - reading them for
an HLE'd draw has nothing to read. The declaration is the stand-in for the bind that draw's library body would
have pushed, which is why `PickLayout` must keep using it for HLE'd draws and the binds only for the engine's own
raw/indexed passes.

Two facts checked while chasing this, both negative and worth not re-litigating:
`SET_CONTEXT_DMA_VERTEX_A/B` (`0x19C`/`0x1A0`) are **never written**, so `dma_vertex_a = 0` and an attribute
offset really is a physical address - the raw-address treatment is correct. And the attribute binds are pushed
per draw (`0x1720` moves every time, `0x1760 = 0x635` = S32K, count 3, stride 6).

### The stale inline vertex layout (fixed, 2026-10-15)

`InlineBound` scanned all 16 stream slots, and the inline `NV097_SET_VERTEX_DATA_ARRAY_FORMAT` binds are
never cleared, so once any slot carried a `fmt` every draw took the inline layout. That is wrong for
`DrawVerticesUP`, which supplies its own vertex array: measured, those draws pass `stride=24/28/44/48` while the
stale bind said SHORT3 / six bytes, so 24 to 48 byte float vertices were read two bytes at a time. That is what
produced the coloured triangles scattered over the screen. `DrawUp` now hides the inline formats for the duration
of the draw, so the UP path reads its declaration against the stride the caller passed; the indexed path, whose
vertices really do come from those bindings, is untouched.

Corroborating facts gathered with `XBE_DUMP_METHODS=1`, `XBE_TRACE_CONST=1`, `XBE_TRACE_WALK=1`:

- A scene draw is `prim=5 n=3` (one triangle per call, ~33k calls) with a five instruction program whose outputs
  are `oPos` then `oT0/oT1/oT2`, and its declaration is `v0<-s0+0/42 v9<-s0+16/42` (float4 at 0 and 16). It reads
  `v9`, so the second input is load bearing, and it only exists if the stride is at least 32.
- `SET_TRANSFORM_CONSTANT` is fine as implemented: the title re-issues `SET_TRANSFORM_CONSTANT_LOAD` per register
  (9M times over ~140 frames), so the pointer never has to be advanced by us. `0x1EA4` really does appear once per
  register, not once per block - do not "fix" this without re-measuring.
- The inline transform program **is** load bearing, and modelling it as "the D3D path already has it" was wrong.
  Measured at `SET_TRANSFORM_PROGRAM_START` over 3045 samples: the chip program and the D3D one are **never equal
  (identical = 0 every time)**, and the chip programs are 11 to 108 instructions against 8 to 22. The engine
  uploads its real programs straight to the chip and never takes them through `CreateVertexShader`.
  xemu is the model to follow here: it has **one** source of truth, `program_data`, and runs
  `program_data[program_start]` up to the instruction that marks itself last. There is no separate "D3D program" at
  the GPU level - and in xemu `SetVertexShader`'s own library body uploads the shader function to the chip, so it
  genuinely supersedes a previous upload. That makes the rule **last writer wins**: `SetVertexShader` clears
  `State::chip_program`, an engine upload claims it back, and `ApplyChipProgram` derives a shader carrying the chip
  program with the *bound shader's declaration* (the chip program says nothing about vertex layout, and the
  declaration is what the D3D path is for).
  Getting this wrong in both directions was instructive: applying the chip program unconditionally broke the 2D
  passes (every frame came back black), and applying it at upload time only was undone by the next
  `SetVertexShader` (5k of 30k scene draws covered).
- `SET_TRANSFORM_PROGRAM` (`0xB00`) does auto-advance its load pointer every fourth dword (xemu pgraph.c), so a
  program longer than the eight instructions one packet holds continues into the next packet at `0xB00` again.
  The accumulation in `Walk` models this now, and `LogProgramStart`/`XBE_DUMP_PROGRAM` can dump program memory.

### The inline vertex layout lands — geometry appears (fixed, 2026-10-15)


The fix above, implemented: `Walk` now keeps the whole `NV097_SET_VERTEX_DATA_ARRAY_FORMAT` word, not just the
stride (`gpu::Stream::fmt` = its low byte, `(count << 4) | type`), and `PickLayout` builds the layout from those
binds when a slot carries one. xemu's type codes map exactly onto the runtime's `TypeSize` kinds
(`0` UB_D3D/D3DCOLOR, `1` S1 normalized shorts, `2` F floats, `4` UB_OGL bytes, `5` S32K plain shorts, `6` CMP
11:11:10), and slot N is input register N with offset 0 and its own binding, so the layout is exact. The
synthesized layout is cached per (program, fmt/stride tuple) because the backend tells shaders apart by pointer.

**Result, confirmed on Proton: coloured triangles render.** The 3D path produces geometry for the first time.
They are in the wrong places, which is the next layer and is expected:

```cpp
InlineInputs(s, *vs);
if (s.vs) { vs->program = s.vs->program; vs->program_type = s.vs->program_type; }
```

The layout is rebuilt from the stream binds, but the **program** is still copied from the stale `g_state.vs`, so
the geometry is fetched correctly and then transformed by whatever vertex program the last `SetVertexShader` left
behind. Next: `NV097_SET_TRANSFORM_PROGRAM` (`0xB00-0xB7C`, 32 words = 8 instructions) with
`NV097_SET_TRANSFORM_PROGRAM_LOAD` (`0x1E9C`) as the instruction index, accumulated and used as the program —
the third audit item (`0xB00`) to turn out load-bearing. Open question for it: how the program's length is
determined (whether the final instruction's bit 31 terminates it, or the load count does).
Also note the `StrideOf` experiment was measured and reverted, so the bound stride remains authoritative.

### The harness cannot answer the visibility question

`ApplyVisibilityMethod` forwards the console's occlusion test to the backend (`ClearReport` / `GetReport`), and
`d3d12.cpp` implements it properly with D3D12 occlusion queries and fence-resolved reports. **`null_backend.cpp`
and `reference_renderer.cpp` implement neither** — they have no `Report` code at all, so they take the base
class no-ops. That matters because the RE notes (`REVERSE_ENGINEERING.md` §4.1/4.2) describe exactly this
mechanism as the game's culling: the leading `0x42400100` NOP is *patched to a jump when the following object is
not visible*. A backend that never answers a report therefore changes which objects the title culls, so harness
frames and Proton frames can legitimately differ in which draws exist. Any harness-based draw analysis has to
carry that caveat, and implementing the report in the reference renderer is the way to remove it.

Closed leads from the same session: `SwitchTexture`'s address is unresolved in the signature scan
(`HAS_D3DDevice_SwitchTexture 0`), so its library body runs and pushes the inline methods — there is nothing to
wire. And the runtime already marks the formats the RE notes call out (`0x00` L8, `0x28` V8U8) as swizzled, so
their FORCE_SWIZZLE workaround is not needed here.

### A whole pass was being silently dropped (fixed)

`Walk()` had no branch for two NV2A vertex-data ranges (xemu `methods.h.inc`):

```
NV097_SET_VERTEX_DATA2S    0x1900, 16 methods    <- was missing
NV097_SET_VERTEX_DATA4S_M  0x1980, 32 methods    <- was missing
```

Both fell through to the generic `simple[]` state, so **every draw that used them produced no geometry and was
dropped without a log line**. That is the title's first pass of every frame: disassembling the render function
at `0x3EAFAE` shows `SetRenderTarget(white surface)` → `SetVertexShader` → `SetPixelShader` →
`SetTransform` → `SetTexture` → inline `0x417FC`+`5` (BEGIN, strip) then `0x1900`/`0x1924` (2-short attribute
writes) → `0x1900` for slot 0 completes each vertex → `D3DDevice_End` (`0x53AD40`).

Those three draws write into **`0275F900` / `02767980` / `0276FA00` — exactly the textures the
`vs=B00C8C91` multiply pass samples** — which is why those samples were stuck at the cleared-white value.
Implemented per xemu `pgraph.c`:

```
2S:   slot = (m-0x1900)/4;  a[0]=(float)(int16)v, a[1]=(float)(int16)(v>>16), a[2]=0, a[3]=1; complete on slot 0
4S_M: i=(m-0x1980)/4, slot=i/2, part=i%2; a[part*2+0..1] = the two shorts; complete on slot 0 part 1
```

Result: **0 draws → 3 draws** targeting those surfaces, so the pass is back. The frame is still ~0.0016 mean
though — the `vs=B00C8C91` multiply still runs over a black-cleared destination, so this alone does not make the
3D scene visible. It does mean those three draws' output now exists for the multiply to read.

**Lesson for next time:** diff every method range the title uses against xemu's `DEF_METHOD_RANGE` list. Dropped
vertex methods lose entire draws silently — `Walk`'s `else` branch stores them as harmless-looking state.

### Vertex streams: inline binds are now decoded (fixed)

The title pushes the NV2A vertex-stream registers inline — `0x1720-0x175C` (`VERTEX_DATA_ARRAY_OFFSET`) and
`0x1760-0x179C` (`VERTEX_DATA_ARRAY_FORMAT`) — **per draw**, and `Walk()` had no branch for either, so
`g_state.streams[]` was stale and most draws read the wrong memory. Now `Walk()` decodes them, using xemu's
authoritative layout (`hw/xbox/nv2a/nv2a_regs.h`, `pgraph.c` — the source tree is in `xemu/`):

```
OFFSET:  slot = (m-0x1720)/4,  data = v & 0x7FFFFFFF   (bit 31 = dma_select, context A/B)
FORMAT:  slot = (m-0x1760)/4,  type = v & 0xF,  count = (v>>4)&0xF,  byte stride = v >> 8
```

The offset goes into a synthetic vertex-buffer header per slot (the same trick `UpVertices` uses) and the byte
stride into `g_state.streams[slot].stride`. The junk-vertex draws are fixed — `vs=B0001361` 32→0,
`vs=B00016E1` 32→0, `vs=B00015E1` 8→0, `vs=B00C8D91` 2→0 — and the stride now matches the hardware
(`00001832` → type F, count 3, stride 24, which is what those draws now read).

Still junk: the text renderer (`vs=B00015F1`/`vs=B00016D1`, `decl v0:0+0/35 v1:0+6/35 v2:0+12/35`) — those
declare 6-byte inputs, which is not a `D3DVSDT` size, so they probably read a stream other than slot 0 (the draw
list only prints slot 0). And **the frame is still ~0.001 mean**: the `vs=B00C8C91` scene draws were already
reading clean data, so this fix does not by itself make the 3D scene appear.

### Where the surfaces come from

`D3DDevice_SetRenderTarget = 0x538E00` (`game.toml`), so the `SETRT` caller log is reliable:

- The 38 pool targets are set from **`0x3F6DAF`**, inside the function at **`0x3F6C60`**, which does
  `push [0x824df4]; push [this+0xe0]; call 0x538E00` — i.e. `SetRenderTarget(pool_buffer, global_depth)`.
  The RT **object is constant** (`7FF40A40` / `7FF40AA0`); only its `+4` data field is re-pointed, so the
  engine renders 38 times through one surface object into a pool. The pool is a regular ~0x1080-byte stride.
- The three white surfaces are set from `0x3EB350` and `0x407FBA` (RT object `7FF41100`).
- There are **no** `CopyRects` calls at all (logged `D3DDevice_CopyRects` never fires).

### The `vs=B00C8C91` draws are a shadow/darkening pass, not the visible scene

The scene draws sample **64×64 and 128×128** light/detail textures (`0276FA00`, `0275F900`) — far too small to be
a screen buffer — and blend `dst = src*dst` with `src` = combiner(white texture, grey diffuse) = white. That is a
*darken the framebuffer by a light map* pass, so `TEST_OPAQUE`'s "silhouette" is the shadow geometry, not the
menu scene. This matches the symptom: nothing the title actually draws to the back buffer is the 3D scene.

The visible scene must therefore come from the **pool**: `0x3F6C60` renders 38 targets (RT object `7FF41FE0`,
data `0277xxxx`, ~0x1080 apart) with the frontend camera, and **nothing ever reads them** — no draw samples
them, and `D3DDevice_CopyRects` never fires. So the composite that should consume the pool (and feed
`7FF34CE0`/`7FF34D00`/`7FF34D20`/`D005FD44`, which are the post-process chain's inputs and also never written)
is missing from the captured stream entirely.

Note the caller addresses in the `SETRT` log are return addresses *inside* the library's small
"set target then release" helpers (e.g. `0x2ACB08` ends with `call 0x538E00; push; call 0x53b870; ret`), so they
point at the helper, not at the title's semantic call site.

Two things stand out and are now the lead:

- **The three surfaces cleared white are exactly the textures the 3D scene samples, and nothing ever draws into
  them.** `0275F900` (128×128) and `0276FA00` (64×64) are `CLEAR`ed to **white** and targeted by **0 draws**;
  they are then read as `tex 128x128 fmt 5 data 8275F900` / `64x64 data 8276FA00` by the `vs=B00C8C91` draws and
  by the 388 `vs=B0001361` draws. So the combiner's stage-0 input is white because the surface is white, and the
  multiply pass resolves to `white × black = black`. The engine does register them as render targets
  (`SETRT rt=7FF41100 data=0275F900/0276FA00`), so the pass that should fill them is missing from the captured
  stream. (Beware the `0x80000000` bit in a texture's data address — it is the swizzle flag; `8276FA00` **is**
  `0276FA00`. Comparing `rt=` against `tex0=` is apples-to-oranges, `tex0=` is the texture *object*.)
- **The 38 offscreen targets (`0277xxxx`) are never read.** Across 57 captured frames there are 42 render
  targets; every one of the 38 `0277xxxx`/`0278xxxx`/`0279xxxx` scene targets is written and never sampled.
  They are set from `0x3F6DB4` through the RT objects `7FF40A40`/`7FF40AA0`.
- **The engine's own state says the `vs=B00C8C91` draws are a multiply.** Disassembly of the title's inline
  draw emitter (`0x409300`, which pushes `0x417FC`/prim 5, then `0x40001818|count<<18`, then the raw vertices)
  and its state helper `0x408410` — reached only from `0x40935f` — shows the blend is selected by the render
  node's byte `[node+0x6c]`:
  `== 0` → `0x344 = 0x306` (`D3DBLEND_DESTCOLOR`) and `0x348 = 0` (`D3DBLEND_ZERO`), i.e. `dst = src*dst`;
  `!= 0` → `0x344 = 0x302`, `0x348 = 0x303` (`SRCALPHA`/`INVSRCALPHA`).
  So the port is faithfully reproducing the engine.

The next questions are roughly in this order:
1. **Find the pass that fills `0275F900` / `02767980` / `0276FA00`.** The engine sets them as render targets but
   no draw reaches them, so either the draws are dropped or `g_state.rt` is stale for them. The RT object is
   `7FF41100`; the setters seen are `003EB350` and `00407FBA`.
2. **Find what reads the 38 `0277xxxx` targets.** Nothing samples them today; they are set from `0x3F6DB4`
   (RT objects `7FF40A40`/`7FF40AA0`), so a composite that should consume them is missing.

## Layout

```
conker-lar/
  Conker Live and Reloaded.iso     the user's disc image (gitignored via iso/)
  iso/                             extracted disc (default.xbe, dvddata/, media/)
  umu-run                          umu-launcher 1.4.1, runs the Windows build under Proton
  XboxXBERecomp/                   the SDK, vendored (git subtree of TGP482/XboxXBERecomp)
  clr_unpack/  xemu/               provided references (unused so far)
  REVERSE_ENGINEERING.md           forum notes for Conker internals
  tools/xbe_disasm.py              disassemble an XBE range by VA (ndisasm)
  tools/xbe_d3d_internals.py       list D3D internals .text calls directly
  notes/d3d_internals_called_from_text.txt
  game/                            the game repo (the port)
    sdk -> ../XboxXBERecomp        symlink the template expects
    assets/ -> iso symlinks        default.xbe, dvddata, media
    config/game.toml               recompiler config ([abi]/[data]/hooks)
    src/game/hooks.cpp             host replacements for D3D internals
    src/common/game_info.cpp       title / window / exe name
    cmake-mingw.cmake              Windows cross toolchain
    third_party/mingw/lib/         libxaudio2.a -> libxaudio2_8.a link shim
    generated/game/                227 MB of recompiled C++ (gitignored)
    out/                           builds (gitignored)
    prefix/                        Proton prefix (gitignored)
    winrun/                        run folder for the exe (gitignored)
```

## Prerequisites

- `extract-xiso`, `clang`/`clang++` 23, `cmake` 4.4, `ninja`, `ndisasm`
- `mingw-w64` (x86_64, posix threads), `llvm` (`ld.lld`, `llvm-ar`)
- For testing the Windows exe: **GE-Proton11-7** and **umu-run 1.4.1**
- The SDK is vendored in `XboxXBERecomp/`, a git subtree of `TGP482/XboxXBERecomp` with its history kept
  (the old fork `giacomozama/XboxXBERecomp`, branch `conker`, is frozen at the vendoring point `a10407e`).

## Reproduce from scratch

`./rebuild.sh` does steps 1-5 below (extract the ISO into `iso/`, link `game/assets`, build
`xbe_symbols`/`xbe_recomp`, write `config/game.toml` if absent, recompile) and takes `--windows` /
`--harness` to also build those targets. The steps are spelled out here for reference.

### 1. Extract the disc
```bash
cd /home/giacomo/Projects/conker-lar
extract-xiso -x "Conker Live and Reloaded.iso" -d iso
```

### 2. Create the game repo
```bash
cp -r XboxXBERecomp/templates/game game
ln -s ../XboxXBERecomp game/sdk
mkdir -p game/assets
ln -s ../../iso/default.xbe game/assets/default.xbe
ln -s ../../iso/dvddata     game/assets/dvddata
ln -s ../../iso/media       game/assets/media
```

### 3. Build the tools (native Linux)
```bash
cd game
cmake -S . -B out/build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS="-march=x86-64-v2" -DCMAKE_CXX_FLAGS="-march=x86-64-v2"
cmake --build out/build/release --target xbe_symbols xbe_recomp -j 16
```

### 4. Write the config
```bash
out/build/release/xbe_symbols assets/default.xbe sdk/xdk --out config/game.toml
```
This finds **122 of 238** XDK functions and **6 of 12** data symbols. Conker's D3D8 is LTCG and many
internals are only matched by constants; `xbe_symbols` writes `[abi]` placeholders (`"?"`) that must be
filled by hand — see [Config](#config).

### 5. Recompile the XBE
```bash
out/build/release/xbe_recomp config/game.toml --xdk sdk/xdk     # -> generated/game
```

### 6a. Test on Linux (headless harness)
```bash
bash sdk/harness/build.sh -r . -g generated/game -o out/harness -n conker
bash sdk/harness/run.sh   -x out/harness/conker -g assets -o out/run -t 20
# logs: out/run/1/run.out and out/run/1/logs/
```

### 6b. Build the Windows exe and run under Proton
```bash
# link-name shim: the runtime links "xaudio2", mingw ships libxaudio2_8.a
mkdir -p third_party/mingw/lib
ln -sf /usr/x86_64-w64-mingw32/lib/libxaudio2_8.a third_party/mingw/lib/libxaudio2.a

cmake -S . -B out/build/win -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake-mingw.cmake \
  -DCMAKE_C_FLAGS="-march=x86-64-v2" -DCMAKE_CXX_FLAGS="-march=x86-64-v2" \
  -DCMAKE_EXE_LINKER_FLAGS="-L$PWD/third_party/mingw/lib -municode -static -lwinpthread" \
  -DXBE_CODEGEN_TOOL=$PWD/out/build/release/xbe_recomp
cmake --build out/build/win --target Game -j 16

mkdir -p winrun && cp out/build/win/Game.exe winrun/ && ln -sf ../assets winrun/assets
cd winrun
WINEPREFIX=$PWD/../prefix \
PROTONPATH=~/.local/share/Steam/compatibilitytools.d/GE-Proton11-7 \
GAMEID=conker-lar \
  ../../umu-run "$PWD/Game.exe"          # umu-run 1.4.1, provided at the repo root
# logs: winrun/logs/game.log
```
`-municode` is needed for the `wWinMain` entry; `-static` links the mingw runtimes so the exe imports only
system DLLs plus `XAudio2_8.dll`. The exe finds `assets/` by walking up from its own directory, so it can run
straight from `winrun/`.

## Config (`game/config/game.toml`)

### `[abi]` — register/stack argument locations (proven by hand)
Conker's D3D8LTCG passes arguments in registers, unlike the reference titles, so each differs from the
signature and must be mapped. Proven with `tools/xbe_disasm.py`:

```toml
CDevice_KickOff = ["edx"]
D3DDevice_DrawVertices = ["stack", "ecx", "eax"]
D3DDevice_GetVertexShaderFunction = ["edx", "eax", "ecx"]
D3DDevice_SelectVertexShader = ["eax", "ebx"]
D3DDevice_SetIndices = ["ebx", "stack"]
D3DDevice_SetPixelShader = ["eax"]
D3DDevice_SetPixelShaderConstant = ["ecx", "stack", "eax"]
D3DDevice_SetShaderConstantMode = ["eax"]
D3DDevice_SetStreamSource = ["eax", "ebx", "stack"]
D3DDevice_SetTexture = ["eax", "stack"]
D3DDevice_SetTextureState_TexCoordIndex = ["esi", "stack"]
D3DDevice_SetTransform = ["eax", "edx"]
D3DDevice_Swap = ["eax"]
D3DResource_IsBusy = ["ecx"]
D3D_InternalRelease = ["esi"]
D3D_MakeRequestedSpace = ["eax", "stack"]
Direct3D_CreateDevice = ["ebx", "edx", "esi", "eax", "stack", "ecx"]
```

### `[data]` — D3D globals found in Conker's XBE
```toml
D3D_Device                  = 0x547540
D3D_pDevice                 = 0x5499E8
D3D_DirtyFlags              = 0x5499F0
D3D_DeferredTextureState    = 0x547330   # [4][32]; 0x5473A0 (reported by the scan) is the TEXCOORDINDEX field
D3D_IndexData               = 0x54A898
D3D_RenderState             = 0x5499F8   # rep movsd 0x39 dwords in SetPixelShader
D3D_Streams                 = 0x547158   # [n*12]; 0x547160 (reported by the scan) is entry 0's buffer field
D3D_SimpleRenderStateEncode = 0x5499F8   # this SDK build never reads it; only needs to be non-zero
D3D_FvfShader               = 0x547040   # CDevice_VertexShader (device+0x794); see progress table #3
```

Additional `[symbols]` hand-mappings (the signature scan misses some Conker internals):
```toml
D3DDevice_DrawVerticesUP    = 0x53A600   # writes NV2A method 0x17FC (primitive) + 0x1818 (inline array)
```

Additional `[symbols]` hand-mappings for the DirectSound surface (see the DirectSound progress table). The
DSOUND library's `.XTLID` ordinals run alphabetically per interface, so `B00xx` names a function by position:
```toml
IDirectSound_CreateSoundStream           = 0x4C67B9   # B003B, stream sibling of B003A CreateSoundBuffer
IDirectSoundBuffer_SetEG                 = 0x4C5074   # B0067
IDirectSoundBuffer_SetLFO                = 0x4C5058   # B006D
IDirectSoundBuffer_SetPitch              = 0x4C503C   # B0076
IDirectSoundBuffer_GetVoiceProperties    = 0x4C519C   # B0055
IDirectSoundStream_GetVoiceProperties    = 0x4C5230   # B0081
IDirectSoundStream_SetLFO                = 0x4C51FA   # B0092
IDirectSoundStream_SetPitch              = 0x4C51F5   # B0099
IDirectSoundStream_SetOutputBuffer       = 0x4C5A03   # B0098, body 0x4C5907
```

`D3D_DeviceInit` (0x53E5D0) is present but **commented out**: running the library's own device init performs
NV2A hardware bring-up (register handshakes, `wbinvd`, a FIFO drain at `0x53ED30`) the runtime cannot answer.
The internals that need it are host-replaced instead.

## Reverse-engineering findings

- **XBE**: title `0x4D530051`, entry `0x0042F0E9`, 27 sections, all 13 libraries at **XDK 1.0.5849** —
  the build the SDK ships `xdk/5849/signatures.toml` for (made from *Splinter Cell: Double Agent*).
- **D3D8 is D3D8LTCG**: the engine calls **library internals directly** (and emits raw NV2A pushbuffers via
  `0x537990`, an 8-byte "append (method,value)" primitive). The runtime already decodes those pushbuffer
  writes in `d3d8.cpp`'s `Walk()`/`g_state.simple[method>>2]`, so they need no replacement.
  `notes/d3d_internals_called_from_text.txt` lists **78** D3D functions `.text` calls that are not in
  `[symbols]` and are hook candidates (`tools/xbe_d3d_internals.py` regenerates it).
- **Miniport**: `[device+0x1C28]` (read as a pointer by `0x54085F`/`0x540B13`) is only ever set by the
  library's hardware init, never by the runtime — hence host-replacing the frame paths that need it.
- **Host replacements** (`src/game/hooks.cpp`):
  - `0x538D50` (per-frame buffer clear): `SetRenderTarget(backbuffer, autoDepth)` + `Clear(target|depth,0,z=1)`
    through `GUEST_CALL(D3DDevice_*)`.
  - `0x543170` (library deferred state flush): no-op that clears the handled dirty bits — the runtime keeps
    its own state and decodes the engine's pushbuffer writes.
  - `0x4C3F2F` / `0x4C3DFE` (DSound helpers that write a console voice's sub-object, `[buffer+0x10]`): no-ops.
  - `0x4C5D4C` (the library's 3D-parameter apply, tail through the null `0x4E1D1C/0x4E1D28` DSP pointers) and
    the listener helpers `0x4C37C1/0x4C37EA/0x4C3821/0x4C3841/0x4C3875/0x4C3897/0x4C9C86` (through
    `0x4E1D20..0x4E1D40`): no-ops.
- **MMIO**: the NV2A (`0xFD000000`) and MCPX APU (`0xFE800000`) register windows are backed by scratch in
  `runtime/kernel/memory.cpp`; Conker's D3D and XACT code read/write them directly.
- **Kernel export 76** `IoQueryVolumeInformation` implemented in `runtime/kernel/xboxkrnl.cpp` (size/device
  class for the mounted volumes).

## Changes to the provided tools (the SDK fork's `conker` branch)

- `cmake/XbeRecomp.cmake` — adds `XBE_CODEGEN_TOOL` so codegen can run with a **native** `xbe_recomp` while
  cross-compiling the game for Windows.
- `runtime/kernel/memory.cpp` / `memory.h` — Linux build needs `<unistd.h>`; backs the NV2A register window
  `0xFD000000` **and** the MCPX APU window `0xFE800000` with scratch on both platforms (Conker's library
  and XACT audio code poke them directly).
- `runtime/gpu/shaders.cpp` — replaces the `1.#INF` / `-1.#INF` HLSL literals with
  `asfloat(0x7F800000u)` / `asfloat(0xFF800000u)`; vkd3d-proton cannot parse the old literals.
- `runtime/xdk/d3d8.cpp` — `Walk()` decodes the engine's inlined transform constants (`0x1EA4`/`0xB80`) and
  stores every other raw method into `g_state.simple[method>>2]`; `SyncDeferred()` reads the whole `[4][32]`
  deferred texture-stage table for a title that inlines the library, leaving the fields whose dedicated
  `SetTextureState_*` HLE writes `g_state` directly untouched. `Direct3D_CreateDevice` also backs the NV2A
  record block the skipped `D3D_DeviceInit` would point `+0x30`/`+0x2488`/`+0x248C`/`+0x2490` at, since
  inlined internals read it (`0x53AE30`). An optional, config-gated `D3D_DeviceInit` call after `CreateDevice`
  is still there (disabled for Conker; kept as a generic hook).
- `runtime/core/diag.h` — `diag::SkipFmv()` (`XBE_SKIP_FMV`): the movie decoder's update is stubbed to its
  error path so the title skips the intro cutscenes; see
  [Skipping the intro FMVs](#skipping-the-intro-fmvs-xbe_skip_fmv).
- `runtime/gpu/render_thread.cpp` — `Threaded::Busy()` no longer reports a resource as busy when its only use
  is in the batch still being built. On Windows (`XBE_RENDER_THREAD=1`) the title polled `D3DResource_IsBusy`
  on a texture it had just drawn with, got `1` forever, and never reached `Swap` — the window froze on the
  splash. Excluding the in-progress batch fixed it (the harness has no render thread, so it never saw it).
- `runtime/xdk/dsound.cpp` — `NewBuffer` requires a `DSBUFFERDESC` (`dwSize == 0x18`) and caps the byte
  count, so the engine's XACT descriptor cannot ask for a multi-GB contiguous block. `DirectSoundCreateStream`
  is split into a shared `NewStream` and a new `IDirectSound_CreateSoundStream` HLE (ordinal `B003B`);
  `IDirectSoundStream_SetOutputBuffer`, the stream/buffer `SetPitch`/`SetLFO`/`SetEG` and the
  buffer/stream `GetVoiceProperties` are HLE'd so their console bodies never run.
- `runtime/kernel/xboxkrnl.cpp` — `IoQueryVolumeInformation` (export 76), now popping its 5 stack arguments.
- `runtime/app` (`app.h`, `settings.cpp`), `runtime/gpu/d3d12.cpp`, `harness/app_headless.cpp` — the `aspect`
  setting takes any `W:H` (`app::Aspect`; `Widescreen()` is 3:2 or wider as before), and `GameInfo::aspect` lets a
  title whose projection the game patches draw any shape: `app::FrameAspect()` is what the window letterboxes to.
- `runtime/gpu/shader_record.{h,cpp}` (new), `runtime/gpu/d3d12.cpp`, `runtime/app` — shaders and pipelines are
  recorded to `shaders.rec` / `shader_programs.bin` and made ahead at the next launch (`shader_warmup` setting); see
  [Shader recording and warm-up](#shader-recording-and-warm-up-2026-10-16).
- `runtime/gpu/d3d12.cpp`, `runtime/gpu/ext.h` (`Target::ms`), `runtime/app` — the `msaa` setting multisamples the
  frame-sized targets; see [MSAA](#msaa-2026-10-16).
- `harness/build.sh`, `harness/run.sh` — executable bit only.

SDK changes are ordinary commits in this repo, next to the game changes they serve. Upstream fixes come in with
`git subtree pull --prefix=XboxXBERecomp https://github.com/TGP482/XboxXBERecomp.git main`; changes can go back
upstream through `git subtree split --prefix=XboxXBERecomp`. `git log -- XboxXBERecomp` lists the SDK history.

## Current state

The post-FMV crash is fixed: two mislocated D3D globals (the device record block the skipped device init sets
up, and the vertex-stream table base `0x547158`) carried the port from a null deref at the end of the intro into
the frontend menu (see [post-FMV crash](#progress-since-the-previous-blocker-the-post-fmv-crash)). The Windows
build reaches the same menu under Proton, and `XBE_SKIP_FMV=1` skips the intro cutscenes so iteration is fast.
The **frontend 3D scene is the open problem**: its draws reach the backend with valid transforms/vertices/textures
but render black, while the 2D overlay renders — see
[the frontend 3D scene](#current-blocker-the-frontend-3d-scene).

The runtime still implements DirectSound as **host-side objects** (`runtime/xdk/dsound.cpp`'s `Voice` map):
the guest objects it hands out are small, zeroed blocks. The engine calls DSound library exports through
their console addresses; each body reached this way is now either HLE'd (if it can be modelled on the host
`Voice`) or skipped from `src/game/hooks.cpp` (if it only writes console voice/DSP registers). The two
patterns to watch for are `this` adjusting to the buffer's voice (`this-0x1c`) or the stream's (`this+4`),
and the `0x4E1D20..0x4E1D40` table of DSP function pointers the library's device init would install.

### Suggested next steps
1. **Make the frontend 3D scene render (top priority).** The scene's geometry, vertex shader, transforms and
   rasterisation are correct (forcing the blend off draws the scene silhouette), and the engine genuinely pushes
   `blend=1 306 0` (`DST_COLOR`/`ZERO`, i.e. `src*dst`) over the black-cleared back buffer, while the 2D overlay
   pushes `302 303` and renders. The scene's combiner shades it **white** (its `ps` definition does reach
   `SetPixelShader`), and its stage-0 texture decodes **black** (`DUMP_TEX`), so: (a) dump the scene's `def`/`kMPs*`
   words and check them against `Combiners::Shade` (and `DecodeLevel`/`FindImage` for the swizzled `R5G6B5`
   texture); (b) find the colour pass the multiply blend is meant to modulate (candidate: the
   `vs=B00015F1`/`B00016D1` draws, or an offscreen-RT composite the port drops/misorders). Tooling: harness `CAP=N`
   writes `cap/draws_<frame>.txt` draw lists, `REF=N` renders frames to `cap/ref_<frame>.tga`, `XBE_DUMP_METHODS=1`
   logs the raw NV2A methods since the last draw and the `SETPS`/`PSDEF` pixel-shader loads, `DUMP_VS=<handle>`
   logs a shader draw's clip output, `PROBES=x,y`/`TRACE=x,y` log a pixel's per-draw combiner values, and
   `DUMP_TEX=1` writes a stage-0 texture to `cap/tex_*.tga`.
2. **Seed D3D8 default render/stage states (secondary).** `D3D_InitDefaultState` is absent from this image,
   so the runtime could apply D3D8's defaults itself (stage 0 `COLOROP=MODULATE`, `COLORARG1=TEXTURE`,
   `COLORARG2=CURRENT`, …) as a fallback for stages the engine does not set.
3. **Keep extending the raw push-buffer decoder** in `Walk()` for whatever other state the engine inlines
   (the API HLEs mostly do not fire for this title), and finish the pixel-shader path (see step 1a).
4. Boot further with pad input (`SCRIPT`/`cmd`) once the scene is visible, and keep host-replacing the remaining
   D3D internals from `notes/d3d_internals_called_from_text.txt`.
5. The DirectSound surface is done for the paths in use (see the DirectSound section); revisit only if a new
   trap surfaces.
6. Rebuild and smoke-test the Windows/Proton exe after config or SDK changes (same commands as
   [Reproduce from scratch](#6b-build-the-windows-exe-and-run-under-proton)); `./run-proton.sh --skip-fmv`
   launches with the cutscenes skipped. The Windows target builds clean after these changes.

### Asides found while fixing
- Kernel HLE exports are `stdcall`: the `KERNEL_EXPORT(ord, name, nargs)` count must equal the pushes at
  *every* call site. A mismatch corrupts guest `esp` one call and crashes somewhere unrelated later —
  which is what the old `0x62A60` blocker really was.
- Conker's XACT region (`0x54xxxx`) and DSOUND (`0x4Cxxxx`) both run console code that pokes memory-mapped
  I/O; anything the runtime does not back faults instead of returning a plausible value.
- The DSOUND library's `.XTLID` ordinals are assigned **alphabetically within each interface** (`B003x`
  `IDirectSound`, `B005x–B007x` `IDirectSoundBuffer`, `B008x–B009x` `IDirectSoundStream`). An export the
  signature scan left unnamed can be identified by counting its position between known anchors — that is how
  `0x4C5A03` was found to be `IDirectSoundStream_SetOutputBuffer`.
- The library installs a table of DSP callbacks at `0x4E1D1C..0x4E1D40` during its own device init
  (`DirectSoundCreate` is HLE'd, so it never runs). Any console path that calls one of those slots traps on
  null; each such caller is a no-op/hook candidate.
- `CDevice_VertexShader` (`+0x794`) is dereferenced by a widely-called D3D internal (`0x536810`, ~68
  callers) as soon as any settable state changes, so it must never be null.

## Gotchas

- The generated C++ is ~227 MB and 132 files; builds are cache-sensitive (`build.sh -G` shares objects).
- `xbe_symbols` **overwrites** `config/game.toml`, including hand-written `[abi]`/`[data]`. Keep a copy, or
  re-merge after regenerating.
- Proton needs a display and Vulkan; this host has `DISPLAY=:0`/Wayland and an NVIDIA GPU.
- `umu-run` (umu-launcher 1.4.1) is provided at the repository root as `./umu-run`; GE-Proton11-7 lives in
  the Steam `compatibilitytools.d` directory.
- `.gitignore` files: `iso/`, `game/{out,assets,prefix,winrun,generated,third_party/mingw/lib}` are ignored;
  the SDK is vendored in `XboxXBERecomp/` (a git subtree), so a plain clone is complete.

### Audio: XACT clock, stream stops, 3D sound (2026-10-15)
- **XACT's clock** reads the APU's free-running 48 kHz sample counter `0xFE80200C` (`0x55485B`). The scratch-backed
  register stayed 0, so every timed XACT event (fades, timed stops, transitions) never ended. `dsound.cpp` now runs it
  from the host clock.
- **Stream stops**: XACT stops a stream voice with `FlushEx` async (flags 1, or 5 with the release envelope) and then
  waits for `GetStatus` to report `ENVELOPECOMPLETE` (`0x80000`, the library's `0x4C8943` reads voice flag `0x2000`)
  before it stops reading the wave; without it the rest of the wave was streamed after every stop. Async flushes now
  stop the voice at once and flush at the next `DirectSoundDoWork`; `GetStatus` reports READY/PLAYING/PAUSED/STARVED
  and ENVELOPECOMPLETE; `Pause(3)` holds a stream that has not started.
- **3D sound**: XACT runs the library's software 3D calculator (`0x4C5D4C`, `0x4C4613` and the listener helpers
  `0x4C37C1..`) through the HRTF function table at `0x4E1D1C..0x4E1D44`, which `DirectSoundUseFullHRTF` fills by calling
  `0x4C753A`. The runtime HLEs that call as a no-op, the table stayed null, and the calculator had been hooked out,
  so every 3D sound played at full volume everywhere (the pub sign's creak). The hooks now install the table and let
  the calculator run; the voice setters `0x4C5235` (stream) / `0x4C51B8` (buffer) hand the descriptor (`+4` distance,
  `+8` cone, `+0x1C` I3DL2 direct volume, `+0x28` azimuth) to `dsound::SetVoice3d`. Azimuth sign assumed positive =
  right.
- `InlineTexture` (`d3d8.cpp`) kept a 64-entry table that reset when full, so a scene with more distinct inline
  textures allocated a header per rebind until system memory ran out; it is a map now.
- Traces: `XBE_TRACE_DSOUND=1` (voice calls, packets with the `.xwb` they came from, per-voice RMS every second,
  3D descriptors), `=all` adds volume/mix calls, `XBE_TRACE_VOICE=<obj>` logs one stream's status polls. Harness runs
  keep the title's saves in their folder: use a fresh one for a reproducible run.
- **Asynchronous reads**: a read on a handle opened without `FILE_SYNCHRONOUS_IO_*` now returns `STATUS_PENDING` and
  completes about 1 ms later, or when the issuing thread waits, sleeps or yields (`files.cpp`, `FlushPendingIo`).
  XACT fills a stream's two slots and then asks whether a packet is still in flight (`0x552DF9` -> `0x54BBC0`); with
  reads that had already completed, a short sound was ended before its only packet played, and a prepared cue (the
  title tune in `m_hungover.xwb`) was dropped and only started much later. Held streams start on `Pause(0)` (rain).
- Harness: with FMVs on, the movie now starts a little later, so a fixed-frame A press can miss it (the intro then
  plays its full 67 s); press A later or more than once.
