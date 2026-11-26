# libmspack (part)

The CAB reader and LZX decompressor of [libmspack](https://github.com/kyz/libmspack), (C) Stuart Caie, under the
GNU Lesser General Public License version 2.1 ([COPYING.LIB](COPYING.LIB)). The Linux Setup uses them to take the
font out of Microsoft's `trebuc32.exe`, which `tools/trebuc.cmake` downloads (its licence is in `THIRD_PARTY.txt`).

Taken from `libmspack/mspack` at commit `8f2a2a91ae2d12f985a196d481962ca4731f0f4d` and cut down to what that one
file needs, in `mspack.c` and `mspack.h`; it is not kept up to date with upstream. Each file's header says what was changed. Setup calls only what
`mspack.h` declares, and the names and signatures there are upstream's.
