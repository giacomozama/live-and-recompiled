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
