# XDK 5849 behaviour the runtime reproduces

Addresses are in default.xbe. sc4_adv.xbe links the same 5849 libraries.

## Pixel shader constants

`D3DDevice_SetPixelShaderConstant` 0x4A5ED0 packs each float4 into a D3DCOLOR (clamped to 0..1, scaled by 255, SSE `minps`/`maxps` at 0x4A5F16) and stores it in the device (+0x8E4). It then pushes the value only to the combiner stages whose constant mapping in the current shader definition (current shader at device +0x784) selects that register: per register mask table 0x5B52E0 compared with the definition's C0 mapping word (+0xE4) at 0x4A5F7C and C1 mapping word (+0xE8) at 0x4A604D, one method per matching stage: PSConstant0 0x0A60 + 4 * stage from 0x4A5F8D, PSConstant1 0x0A80 + 4 * stage from 0x4A605E. Stages mapped to other registers keep their values.

## SetPixelShader pushes the definition constants

`D3DDevice_SetPixelShader` 0x4A5CD0 pushes the definition's own PSConstant0/PSConstant1 dwords as they are: method 0x0A60 with 32 dwords copied from definition +0x28 at 0x4A5DF5 to 0x4A5E07. Constants set earlier through SetPixelShaderConstant are not reapplied: device +0x8E4 has no reference in section D3D other than the store at 0x4A5F5C.

## Lazy texture mode fixup

The lazy state routine at 0x4A4480 (0x4A4840 is inside it, per the disassembly of default.xbe) builds the shader stage program (method 0x1E70, pushed at 0x4A4959). With a pixel shader set it takes the definition's PSTextureModes (kept at device +0x790 by SetPixelShader, 0x4A5D95) and fixes each stage's mode from the stage's texture word at device +0xC + 4 * stage, 0x4A4756 to 0x4A494B:

- sign bit set (observed as no texture bound): modes other than 4, 5, 0xA, 0x11 become 0 (0x4A47EF to 0x4A4803)
- modes 1 to 3: cube bit (`test dl, 4`) gives 3, bit 30 or format class 0x30 gives 2, else 1 (0x4A4807 to 0x4A4837)
- modes 0xD, 0xE: 0xE with the cube bit, else 0xD (0x4A483E to 0x4A4854)

## Swap rotates surface memory, not surface objects

`D3DDevice_Swap` 0x49CC50 calls 0x49CB00 (from 0x49CCF6), which walks the device frame buffer array (count at +0x1A10, surfaces from +0x1A14) and moves each surface's Data (+4) and Lock (+8) to the next one, the last one's going to the first (0x49CB01 to 0x49CB44). The surface objects stay where they are, so pointers a title keeps to the back buffer stay valid.

## CopyRects reads the source address as is

`D3DDevice_CopyRects` 0x49AB20 is a GPU blit: it pushes the source and destination surface Data (+4) straight into methods 0x6308/0x6300 at 0x49AC1D to 0x49AC40. A source the title filled with the CPU in contiguous memory is copied like any other, so the runtime copies guest memory rather than a host render target.

## CreateDevice runs the default state routine

`Direct3D_CreateDevice` 0x49D210 calls the device init 0x4A2220 at 0x49D269. That routine sets the render target to the back buffer (+0x1A14, `D3DDevice_SetRenderTarget` call at 0x4A2657), calls `D3D_InitDefaultState` 0x4A1A80 at 0x4A265C, then clears (0x4A2670).

## Stream packet callbacks run in DirectSoundDoWork

`DirectSoundDoWork` 0x4CEF33 calls `CDirectSound_DoWork` 0x4CDCA6, which runs 0x4D2751: it empties the deferred command list at +0x4B0 and calls each command's owner (vtable +0x24) with the lock let go. A voice has six command slots from +0xD0, 0x20 bytes each, flags at +8; the stream constructor sets bit 0 on slots 0 to 3 (0x4D33CC to 0x4D33E1). The scheduler 0x4D2CC5 puts a slot with bit 0 on the +0x4B0 list (0x4D2648) and any other on the DPC path (0x4D2B9A). So a stream's completions reach the title from its own `DirectSoundDoWork` call, never beside its other stream calls; the runtime runs them there, and from a thread of its own only when the title has not called it for a while.

## SetFormat sets the pitch back to the format's rate

`CDirectSoundVoice_SetFormat` (sc4_adv.xbe 0x2BD4D7) calls the settings' SetFormat 0x2BD451, which ends by storing the pitch of the new format's sample rate (+0x10, through 0x2BB6B5) at +0x18 (0x2BD4C3 to 0x2BD4CC), then `CMcpxVoiceClient_SetPitch` 0x2C2806 (0x2BD512). A frequency set earlier is gone, so the runtime clears its own.

## SetAllParameters copies the 3D block

Proven in Black (title 45410083, the same 5849 DSOUND), which sets its 3D sounds with nothing else. `IDirectSoundBuffer_SetAllParameters` 0x21A18B reaches 0x219BCA, which copies 18 dwords from the DS3DBUFFER at +4 into the voice's 3D settings (`[[this + 0x10] + 0xB4]`) at +8 (0x219BDC to 0x219CDE). In default.xbe the single setters write the same block: position at +8 (0x4CF50C), velocity at +0x14 (0x4CF55E), minimum distance at +0x38 (0x4CF4D8), maximum distance at +0x3C (0x4CF4A5). So the structure has the position at +4, the distances at +0x34 and +0x38 and the mode at +0x3C; the library's own default block (Black 0x2243A8) agrees: size 0x4C, 1.0 at +0x34, 1e9 at +0x38. Mode 2 turns the 3D work off (0x2190AB, 0x219108 and 0x219145 skip each calculation), mode 1 takes the position as listener relative (0x21BBCA skips the listener subtraction).

## StopEx

`IDirectSoundBuffer_StopEx` (Black 0x2197AF) reaches 0x220A0A. With a time stamp it queues the stop (command 4 through vtable +0x1C). Without one it calls 0x2207A1 with the flags: none stops at once, as Stop does (0x218B2B passes 0); bit 0 keys the voice off so its envelope releases (0x21EED8 writes the voice to FE8202F8 and FE82012C); bits 0 and 1 on a looping voice first play on without the loop (0x220688 with play flags 0, from 0x2207D0). The runtime stops at once in every case.

## Buffer positions count from the play region

Proven in Black (title 45410083), which gives every buffer its whole sound heap with `SetBufferData` and picks a wave with `SetPlayRegion`. `IDirectSoundBuffer_SetPlayRegion` 0x21A1EF reaches 0x219E66: a zero length means to the end of the buffer (0x219EB3), start plus length past the buffer size (+0xC0) fails with 0x88780032, then 0x219E0F stores start at +0xC4, length at +0xC8 and resets the loop to start 0 (+0xCC) and the same length (+0xD0); 0x220738 clears the kept position (+0x148) and restarts a playing voice from the region start. The voice's base address is the data plus the play start (0x21FE54). `SetLoopRegion` 0x218BF4 checks the loop against the play length (+0xC8), not the buffer size, and a zero length means to the end of the play region. Play (0x21FF09) programs the voice end as the play length, or the loop start and loop end when looping, all counted from that base, and `SetCurrentPosition` 0x220831 and `GetCurrentPosition` 0x22027E use the same origin. `SetBufferData` 0x21A480 does nothing when data and size are unchanged, otherwise 0x21A3F4 resets the play region to the whole buffer. default.xbe holds the same code with the same offsets (store 0x4CF668, `SetLoopRegion` 0x4CE1F5, voice base 0x4D59C1, play 0x4D5A76, `SetBufferData` 0x4CFF89) but does not link `SetPlayRegion`, so its play region is always the whole buffer.

## An ADPCM block is 64 frames

The byte and sample conversions (Black 0x21D1DB and 0x21D1F4, default.xbe 0x4D2E4B and 0x4D2E64) use 64 samples for each block of the format's block align when the tag is 0x69 (`shl eax, 6`, `shr eax, 6`). The 65th decoded frame of a block is not played.

## Headroom is taken off the volume

`SetVolume` (Black 0x2188D8, default.xbe 0x4CDEEB) stores volume minus headroom (+0x20) at +0x1C, `SetHeadroom` (0x218907, 0x4CDF1A) moves +0x1C by the change. The settings constructor (0x21A97D, 0x4D041C) starts the headroom at 600, or at 0 for a buffer with flags 0x182000 (mix in, effect in) or 0x200010 (3D).

## Mix bins belong to channels in runs

The slot volume builder (Black 0x21E94D, default.xbe 0x4D45CB) builds the volume of each of a voice's eight bin slots: the bin count (+0x24) divided by the channel count gives the slots of each channel, channel 0 the first run, channel 1 the next. A slot's attenuation is the bin's volume (+0x30 by bin number) plus +0x1C. `SetMixBins` (0x2198BB, 0x4CF1D9) replaces the list and its volumes, `SetMixBinVolumes` (0x2187DF, 0x4CDDF2) only writes volumes by bin number. Bins 6 to 9 are the crosstalk front left, front right, back left and back right (the 3D default list, Black 0x2243F4 and default.xbe 0x4EB94C, is 6, 8, 7, 9, 10).

## Speaker config

`IDirectSound_GetSpeakerConfig` (Black 0x2183DF, default.xbe 0x4CDB34) returns 0 for stereo, 1 for mono, 2 for surround, with 0x10000 for AC3, from the console's audio setting (0x4D0304 fills the global from 0x452505). Black 0x8C360 reads it that way. The runtime mixes to two channels and answers stereo.
