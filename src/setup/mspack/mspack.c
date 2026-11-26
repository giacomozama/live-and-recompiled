/* From libmspack (https://github.com/kyz/libmspack, commit 8f2a2a91ae2d12f985a196d481962ca4731f0f4d)
 * (C) 2003-2023 Stuart Caie.
 *
 * libmspack is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License (LGPL) version 2.1
 *
 * For further details, see the file COPYING.LIB distributed with libmspack
 *
 * Modified on 2026-11-26 for live-and-recompiled: the CAB reader and LZX
 * decompressor cut down to what finding one cabinet inside a file and extracting from its LZX folders needs, and
 * merged into this file (from cabd.c, lzxd.c, system.c and their headers); comments removed.
 * This modified file is under the LGPL version 2.1 like the original.
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "mspack.h"

#define __egi32(a,n) (((unsigned int) ((unsigned char *)(a))[n+3] << 24) | \
                      ((unsigned int) ((unsigned char *)(a))[n+2] << 16) | \
                      ((unsigned int) ((unsigned char *)(a))[n+1] <<  8) | \
                      ((unsigned int) ((unsigned char *)(a))[n]))
#define EndGetI32(a) __egi32(a,0)
#define EndGetI16(a) ((((a)[1])<<8)|((a)[0]))


struct mspack_file {
  FILE *fh;
};

static struct mspack_file *msp_open(struct mspack_system *self, const char *filename, int mode) {
  struct mspack_file *file = (struct mspack_file *) malloc(sizeof(struct mspack_file));
  (void) self;
  if (!file) return NULL;
  if ((file->fh = fopen(filename, mode == MSPACK_SYS_OPEN_READ ? "rb" : "wb"))) return file;
  free(file);
  return NULL;
}

static void msp_close(struct mspack_file *file) {
  if (file) {
    fclose(file->fh);
    free(file);
  }
}

static int msp_read(struct mspack_file *file, void *buffer, int bytes) {
  size_t count = fread(buffer, 1, (size_t) bytes, file->fh);
  return ferror(file->fh) ? -1 : (int) count;
}

static int msp_write(struct mspack_file *file, void *buffer, int bytes) {
  size_t count = fwrite(buffer, 1, (size_t) bytes, file->fh);
  return ferror(file->fh) ? -1 : (int) count;
}

static int msp_seek(struct mspack_file *file, off_t offset, int mode) {
  return fseek(file->fh, offset, mode == MSPACK_SYS_SEEK_CUR ? SEEK_CUR : SEEK_SET);
}

static off_t msp_tell(struct mspack_file *file) {
  return (off_t) ftell(file->fh);
}

static void *msp_alloc(struct mspack_system *self, size_t bytes) {
  (void) self;
  return malloc(bytes);
}

static void msp_copy(void *src, void *dest, size_t bytes) {
  memcpy(dest, src, bytes);
}

static struct mspack_system msp_system = {
  &msp_open, &msp_close, &msp_read, &msp_write, &msp_seek, &msp_tell, &msp_alloc, &free, &msp_copy
};

static struct mspack_system *mspack_default_system = &msp_system;

#define cfhead_Signature         (0x00)
#define cfhead_NumFolders        (0x1A)
#define cfhead_NumFiles          (0x1C)
#define cfhead_Flags             (0x1E)
#define cfhead_SIZEOF            (0x24)
#define cfheadext_HeaderReserved (0x00)
#define cfheadext_FolderReserved (0x02)
#define cfheadext_DataReserved   (0x03)
#define cfheadext_SIZEOF         (0x04)
#define cffold_DataOffset        (0x00)
#define cffold_NumBlocks         (0x04)
#define cffold_CompType          (0x06)
#define cffold_SIZEOF            (0x08)
#define cffile_UncompressedSize  (0x00)
#define cffile_FolderOffset      (0x04)
#define cffile_FolderIndex       (0x08)
#define cffile_SIZEOF            (0x10)
#define cfdata_CheckSum          (0x00)
#define cfdata_CompressedSize    (0x04)
#define cfdata_UncompressedSize  (0x06)
#define cfdata_SIZEOF            (0x08)

#define cffoldCOMPTYPE_MASK      (0x000f)
#define cffoldCOMPTYPE_LZX       (0x0003)
#define cfheadPREV_CABINET       (0x0001)
#define cfheadNEXT_CABINET       (0x0002)
#define cfheadRESERVE_PRESENT    (0x0004)

#define CAB_BLOCKMAX (32768)
#define CAB_INPUTMAX (CAB_BLOCKMAX+6144)

#define CAB_DECOMP_BUF (4096)

struct mscabd_decompress_state {
  struct mscabd_folder_p *folder;
  unsigned int offset;
  unsigned int block;
  off_t outlen;
  struct mspack_system sys;
  struct lzxd_stream *state;
  struct mspack_file *infh;
  struct mspack_file *outfh;
  unsigned char *i_ptr, *i_end;
  unsigned char input[CAB_INPUTMAX];
};

struct mscab_decompressor_p {
  struct mscab_decompressor base;
  struct mscabd_decompress_state *d;
  struct mspack_system *system;
  int read_error;
};

struct mscabd_cabinet_p {
  struct mscabd_cabinet base;
  int block_resv;
};

struct mscabd_folder_p {
  struct mscabd_folder base;
  struct mscabd_cabinet_p *cab;
  off_t offset;
};

#define LZX_MIN_MATCH                (2)
#define LZX_MAX_MATCH                (257)
#define LZX_NUM_CHARS                (256)
#define LZX_BLOCKTYPE_INVALID        (0)
#define LZX_BLOCKTYPE_VERBATIM       (1)
#define LZX_BLOCKTYPE_ALIGNED        (2)
#define LZX_BLOCKTYPE_UNCOMPRESSED   (3)
#define LZX_PRETREE_NUM_ELEMENTS     (20)
#define LZX_ALIGNED_NUM_ELEMENTS     (8)
#define LZX_NUM_PRIMARY_LENGTHS      (7)
#define LZX_NUM_SECONDARY_LENGTHS    (249)

#define LZX_PRETREE_MAXSYMBOLS  (LZX_PRETREE_NUM_ELEMENTS)
#define LZX_PRETREE_TABLEBITS   (6)
#define LZX_MAINTREE_MAXSYMBOLS (LZX_NUM_CHARS + 50*8)
#define LZX_MAINTREE_TABLEBITS  (12)
#define LZX_LENGTH_MAXSYMBOLS   (LZX_NUM_SECONDARY_LENGTHS+1)
#define LZX_LENGTH_TABLEBITS    (12)
#define LZX_ALIGNED_MAXSYMBOLS  (LZX_ALIGNED_NUM_ELEMENTS)
#define LZX_ALIGNED_TABLEBITS   (7)
#define LZX_LENTABLE_SAFETY (64)

#define LZX_FRAME_SIZE (32768)

#if (1 << LZX_PRETREE_TABLEBITS) < (LZX_PRETREE_MAXSYMBOLS * 2)
# define LZX_PRETREE_TABLESIZE (LZX_PRETREE_MAXSYMBOLS * 4)
#else
# define LZX_PRETREE_TABLESIZE ((1 << LZX_PRETREE_TABLEBITS) + \
                                (LZX_PRETREE_MAXSYMBOLS * 2))
#endif
#if (1 << LZX_MAINTREE_TABLEBITS) < (LZX_MAINTREE_MAXSYMBOLS * 2)
# define LZX_MAINTREE_TABLESIZE (LZX_MAINTREE_MAXSYMBOLS * 4)
#else
# define LZX_MAINTREE_TABLESIZE ((1 << LZX_MAINTREE_TABLEBITS) + \
                                 (LZX_MAINTREE_MAXSYMBOLS * 2))
#endif
#if (1 << LZX_LENGTH_TABLEBITS) < (LZX_LENGTH_MAXSYMBOLS * 2)
# define LZX_LENGTH_TABLESIZE (LZX_LENGTH_MAXSYMBOLS * 4)
#else
# define LZX_LENGTH_TABLESIZE ((1 << LZX_LENGTH_TABLEBITS) + \
                               (LZX_LENGTH_MAXSYMBOLS * 2))
#endif
#if (1 << LZX_ALIGNED_TABLEBITS) < (LZX_ALIGNED_MAXSYMBOLS * 2)
# define LZX_ALIGNED_TABLESIZE (LZX_ALIGNED_MAXSYMBOLS * 4)
#else
# define LZX_ALIGNED_TABLESIZE ((1 << LZX_ALIGNED_TABLEBITS) + \
                                (LZX_ALIGNED_MAXSYMBOLS * 2))
#endif

struct lzxd_stream {
  struct mspack_system *sys;
  struct mspack_file   *input;
  struct mspack_file   *output;

  off_t   offset;
  off_t   length;

  unsigned char *window;
  unsigned int   window_size;
  unsigned int   num_offsets;
  unsigned int   window_posn;
  unsigned int   frame_posn;
  unsigned int   frame;

  unsigned int   R0, R1, R2;
  unsigned int   block_length;
  unsigned int   block_remaining;

  signed int     intel_filesize;

  unsigned char  intel_started;
  unsigned char  block_type;
  unsigned char  header_read;
  unsigned char  input_end;

  int error;

  unsigned char *inbuf, *i_ptr, *i_end, *o_ptr, *o_end;
  unsigned int  bit_buffer, bits_left, inbuf_size;

  unsigned char PRETREE_len  [LZX_PRETREE_MAXSYMBOLS  + LZX_LENTABLE_SAFETY];
  unsigned char MAINTREE_len [LZX_MAINTREE_MAXSYMBOLS + LZX_LENTABLE_SAFETY];
  unsigned char LENGTH_len   [LZX_LENGTH_MAXSYMBOLS   + LZX_LENTABLE_SAFETY];
  unsigned char ALIGNED_len  [LZX_ALIGNED_MAXSYMBOLS  + LZX_LENTABLE_SAFETY];

  unsigned short PRETREE_table [LZX_PRETREE_TABLESIZE];
  unsigned short MAINTREE_table[LZX_MAINTREE_TABLESIZE];
  unsigned short LENGTH_table  [LZX_LENGTH_TABLESIZE];
  unsigned short ALIGNED_table [LZX_ALIGNED_TABLESIZE];
  unsigned char LENGTH_empty;

  unsigned char  e8_buf[LZX_FRAME_SIZE];
};


#define BITS_TYPE struct lzxd_stream
#define BITS_VAR lzx
#define READ_BYTES do {                 \
    unsigned char b0, b1;               \
    READ_IF_NEEDED; b0 = *i_ptr++;      \
    READ_IF_NEEDED; b1 = *i_ptr++;      \
    INJECT_BITS((b1 << 8) | b0, 16);    \
} while (0)

typedef unsigned int bitbuf_type;

#define BITBUF_WIDTH (sizeof(bitbuf_type) * CHAR_BIT)

#define DECLARE_BIT_VARS \
   unsigned char *i_ptr, *i_end; \
   register bitbuf_type bit_buffer; \
   register int bits_left

#define INIT_BITS do {                          \
    BITS_VAR->i_ptr      = &BITS_VAR->inbuf[0]; \
    BITS_VAR->i_end      = &BITS_VAR->inbuf[0]; \
    BITS_VAR->bit_buffer = 0;                   \
    BITS_VAR->bits_left  = 0;                   \
    BITS_VAR->input_end  = 0;                   \
} while (0)

#define STORE_BITS do {                 \
    BITS_VAR->i_ptr      = i_ptr;       \
    BITS_VAR->i_end      = i_end;       \
    BITS_VAR->bit_buffer = bit_buffer;  \
    BITS_VAR->bits_left  = bits_left;   \
} while (0)

#define RESTORE_BITS do {               \
    i_ptr      = BITS_VAR->i_ptr;       \
    i_end      = BITS_VAR->i_end;       \
    bit_buffer = BITS_VAR->bit_buffer;  \
    bits_left  = BITS_VAR->bits_left;   \
} while (0)

#define ENSURE_BITS(nbits) do {                 \
    while (bits_left < (nbits)) READ_BYTES;     \
} while (0)

#define READ_BITS(val, nbits) do {              \
    ENSURE_BITS(nbits);                         \
    (val) = PEEK_BITS(nbits);                   \
    REMOVE_BITS(nbits);                         \
} while (0)

# define PEEK_BITS(nbits)   (bit_buffer >> (BITBUF_WIDTH - (nbits)))
# define REMOVE_BITS(nbits) ((bit_buffer <<= (nbits)), (bits_left -= (nbits)))
# define INJECT_BITS(bitdata,nbits) ((bit_buffer |= \
    (bitbuf_type)(bitdata) << (BITBUF_WIDTH - (nbits) - bits_left)), \
    (bits_left += (nbits)))

# define READ_IF_NEEDED do {            \
    if (i_ptr >= i_end) {               \
        if (read_input(BITS_VAR))       \
            return BITS_VAR->error;     \
        i_ptr = BITS_VAR->i_ptr;        \
        i_end = BITS_VAR->i_end;        \
    }                                   \
} while (0)

static int read_input(BITS_TYPE *p) {
    int read = p->sys->read(p->input, &p->inbuf[0], (int)p->inbuf_size);
    if (read < 0) return p->error = MSPACK_ERR_READ;

    if (read == 0) {
        if (p->input_end) {
            return p->error = MSPACK_ERR_READ;
        }
        else {
            read = 2;
            p->inbuf[0] = p->inbuf[1] = 0;
            p->input_end = 1;
        }
    }

    p->i_ptr = &p->inbuf[0];
    p->i_end = &p->inbuf[read];
    return MSPACK_ERR_OK;
}

#define TABLEBITS(tbl)      LZX_##tbl##_TABLEBITS
#define MAXSYMBOLS(tbl)     LZX_##tbl##_MAXSYMBOLS
#define HUFF_TABLE(tbl,idx) lzx->tbl##_table[idx]
#define HUFF_LEN(tbl,idx)   lzx->tbl##_len[idx]
#define HUFF_ERROR          return lzx->error = MSPACK_ERR_DECRUNCH

#define HUFF_MAXBITS 16

#define DECLARE_HUFF_VARS \
   DECLARE_BIT_VARS; \
   register int huff_idx; \
   register unsigned short huff_sym

#define READ_HUFFSYM(tbl, var) do {                                 \
    ENSURE_BITS(HUFF_MAXBITS);                                      \
    huff_sym = HUFF_TABLE(tbl, PEEK_BITS(TABLEBITS(tbl)));          \
    if (huff_sym >= MAXSYMBOLS(tbl)) HUFF_TRAVERSE(tbl);            \
    (var) = huff_sym;                                               \
    huff_idx = HUFF_LEN(tbl, huff_sym);                             \
    REMOVE_BITS(huff_idx);                                          \
} while (0)

#define HUFF_TRAVERSE(tbl) do {                                     \
    huff_idx = 1 << (BITBUF_WIDTH - TABLEBITS(tbl));                \
    do {                                                            \
        if ((huff_idx >>= 1) == 0) HUFF_ERROR;                      \
        huff_sym = HUFF_TABLE(tbl,                                  \
            (huff_sym << 1) | ((bit_buffer & huff_idx) ? 1 : 0));   \
    } while (huff_sym >= MAXSYMBOLS(tbl));                          \
} while (0)

static int make_decode_table(unsigned int nsyms, unsigned int nbits,
                             unsigned char *length, unsigned short *table)
{
    register unsigned short sym, next_symbol, end;
    register unsigned int leaf, fill;
    register unsigned char bit_num;
    unsigned int pos         = 0;
    unsigned int table_mask  = 1 << nbits;
    unsigned int bit_mask    = table_mask >> 1;

    for (bit_num = 1; bit_num <= nbits; bit_num++) {
        for (sym = 0; sym < nsyms; sym++) {
            if (length[sym] != bit_num) continue;
            leaf = pos;

            if((pos += bit_mask) > table_mask) return 1;

            for (fill = bit_mask; fill-- > 0;) table[leaf++] = sym;
        }
        bit_mask >>= 1;
    }

    if (pos == table_mask) return 0;

    for (sym = pos; sym < table_mask; sym++) {
        table[sym] = 0xFFFF;
    }

    next_symbol = ((table_mask >> 1) < nsyms) ? nsyms : (table_mask >> 1);
    end = next_symbol + nsyms;

    pos <<= 16;
    table_mask <<= 16;
    bit_mask = 1 << 15;

    for (bit_num = nbits+1; bit_num <= HUFF_MAXBITS; bit_num++) {
        for (sym = 0; sym < nsyms; sym++) {
            if (length[sym] != bit_num) continue;
            if (pos >= table_mask) return 1;

            leaf = pos >> 16;
            for (fill = 0; fill < (bit_num - nbits); fill++) {

                if (table[leaf] == 0xFFFF) {
                    if (next_symbol >= end) return 1;
                    table[(next_symbol << 1)     ] = 0xFFFF;
                    table[(next_symbol << 1) + 1 ] = 0xFFFF;
                    table[leaf] = next_symbol++;
                }

                leaf = table[leaf] << 1;
                if ((pos >> (15-fill)) & 1) leaf++;
            }
            table[leaf] = sym;
            pos += bit_mask;
        }
        bit_mask >>= 1;
    }

    return (pos == table_mask) ? 0 : 1;
}

#define BUILD_TABLE(tbl)                                                \
    if (make_decode_table(MAXSYMBOLS(tbl), TABLEBITS(tbl),              \
                          &HUFF_LEN(tbl,0), &HUFF_TABLE(tbl,0)))        \
    {                                                                   \
        return lzx->error = MSPACK_ERR_DECRUNCH;                        \
    }

#define BUILD_TABLE_MAYBE_EMPTY(tbl) do {                               \
    lzx->tbl##_empty = 0;                                               \
    if (make_decode_table(MAXSYMBOLS(tbl), TABLEBITS(tbl),              \
                          &HUFF_LEN(tbl,0), &HUFF_TABLE(tbl,0)))        \
    {                                                                   \
        for (i = 0; i < MAXSYMBOLS(tbl); i++) {                         \
            if (HUFF_LEN(tbl, i) > 0) {                                 \
                return lzx->error = MSPACK_ERR_DECRUNCH;                \
            }                                                           \
        }                                                               \
            \
        lzx->tbl##_empty = 1;                                           \
    }                                                                   \
} while (0)

#define READ_LENGTHS(tbl, first, last) do {             \
  STORE_BITS;                                           \
  if (lzxd_read_lens(lzx, &HUFF_LEN(tbl, 0), (first),   \
    (unsigned int)(last))) return lzx->error;           \
  RESTORE_BITS;                                         \
} while (0)

static int lzxd_read_lens(struct lzxd_stream *lzx, unsigned char *lens,
                          unsigned int first, unsigned int last)
{
  DECLARE_HUFF_VARS;
  unsigned int x, y;
  int z;

  RESTORE_BITS;

  for (x = 0; x < 20; x++) {
    READ_BITS(y, 4);
    lzx->PRETREE_len[x] = y;
  }
  BUILD_TABLE(PRETREE);

  for (x = first; x < last; ) {
    READ_HUFFSYM(PRETREE, z);
    if (z == 17) {

      READ_BITS(y, 4); y += 4;
      while (y--) lens[x++] = 0;
    }
    else if (z == 18) {

      READ_BITS(y, 5); y += 20;
      while (y--) lens[x++] = 0;
    }
    else if (z == 19) {

      READ_BITS(y, 1); y += 4;
      READ_HUFFSYM(PRETREE, z);
      z = lens[x] - z; if (z < 0) z += 17;
      while (y--) lens[x++] = z;
    }
    else {

      z = lens[x] - z; if (z < 0) z += 17;
      lens[x++] = z;
    }
  }

  STORE_BITS;

  return MSPACK_ERR_OK;
}

static const unsigned int position_slots[7] = {
    30, 32, 34, 36, 38, 42, 50
};
static const unsigned char extra_bits[36] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16
};
static const unsigned int position_base[50] = {
    0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512,
    768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768,
    49152, 65536, 98304, 131072, 196608, 262144, 393216, 524288, 655360,
    786432, 917504, 1048576, 1179648, 1310720, 1441792, 1572864, 1703936,
    1835008, 1966080
};

static void lzxd_reset_state(struct lzxd_stream *lzx) {
  int i;

  lzx->R0              = 1;
  lzx->R1              = 1;
  lzx->R2              = 1;
  lzx->header_read     = 0;
  lzx->block_remaining = 0;
  lzx->block_type      = LZX_BLOCKTYPE_INVALID;

  for (i = 0; i < LZX_MAINTREE_MAXSYMBOLS; i++) lzx->MAINTREE_len[i] = 0;
  for (i = 0; i < LZX_LENGTH_MAXSYMBOLS; i++)   lzx->LENGTH_len[i]   = 0;
}

static struct lzxd_stream *lzxd_init(struct mspack_system *system,
                              struct mspack_file *input,
                              struct mspack_file *output,
                              int window_bits,
                              int input_buffer_size)
{
  unsigned int window_size = 1U << window_bits;
  struct lzxd_stream *lzx;

  if (!system) return NULL;

  if (window_bits < 15 || window_bits > 21) return NULL;

  input_buffer_size = (input_buffer_size + 1) & -2;
  if (input_buffer_size < 2) return NULL;

  if (!(lzx = (struct lzxd_stream *) system->alloc(system, sizeof(struct lzxd_stream)))) {
    return NULL;
  }

  lzx->window = (unsigned char *) system->alloc(system, window_size);
  lzx->inbuf  = (unsigned char *) system->alloc(system, input_buffer_size);
  if (!lzx->window || !lzx->inbuf) {
    system->free(lzx->window);
    system->free(lzx->inbuf);
    system->free(lzx);
    return NULL;
  }

  lzx->sys             = system;
  lzx->input           = input;
  lzx->output          = output;
  lzx->offset          = 0;
  lzx->length          = 0;

  lzx->inbuf_size      = input_buffer_size;
  lzx->window_size     = 1 << window_bits;
  lzx->window_posn     = 0;
  lzx->frame_posn      = 0;
  lzx->frame           = 0;
  lzx->intel_filesize  = 0;
  lzx->intel_started   = 0;
  lzx->error           = MSPACK_ERR_OK;
  lzx->num_offsets     = position_slots[window_bits - 15] << 3;

  lzx->o_ptr = lzx->o_end = &lzx->e8_buf[0];
  lzxd_reset_state(lzx);
  INIT_BITS;
  return lzx;
}

static void lzxd_set_output_length(struct lzxd_stream *lzx, off_t out_bytes) {
  if (lzx && out_bytes > 0) lzx->length = out_bytes;
}

static int lzxd_decompress(struct lzxd_stream *lzx, off_t out_bytes) {
  DECLARE_HUFF_VARS;
  unsigned char *window, *runsrc, *rundest, buf[12];
  unsigned int frame_size, end_frame, window_posn, R0, R1, R2;
  int bytes_todo, this_run, i, j;

  if (!lzx || (out_bytes < 0)) return MSPACK_ERR_ARGS;
  if (lzx->error) return lzx->error;

  i = lzx->o_end - lzx->o_ptr;
  if ((off_t) i > out_bytes) i = (int) out_bytes;
  if (i) {
    if (lzx->sys->write(lzx->output, lzx->o_ptr, i) != i) {
      return lzx->error = MSPACK_ERR_WRITE;
    }
    lzx->o_ptr  += i;
    lzx->offset += i;
    out_bytes   -= i;
  }
  if (out_bytes == 0) return MSPACK_ERR_OK;

  RESTORE_BITS;
  window = lzx->window;
  window_posn = lzx->window_posn;
  R0 = lzx->R0;
  R1 = lzx->R1;
  R2 = lzx->R2;

  end_frame = (unsigned int)((lzx->offset + out_bytes) / LZX_FRAME_SIZE) + 1;

  while (lzx->frame < end_frame) {

    if (!lzx->header_read) {

      j = 0; READ_BITS(i, 1); if (i) { READ_BITS(i, 16); READ_BITS(j, 16); }
      lzx->intel_filesize = (int) (((unsigned int) i << 16) | (unsigned int) j);
      lzx->header_read = 1;
    }

    frame_size = LZX_FRAME_SIZE;
    if (lzx->length && (lzx->length - lzx->offset) < (off_t)frame_size) {
      frame_size = lzx->length - lzx->offset;
    }

    bytes_todo = lzx->frame_posn + frame_size - window_posn;
    while (bytes_todo > 0) {

      if (lzx->block_remaining == 0) {

        if ((lzx->block_type == LZX_BLOCKTYPE_UNCOMPRESSED) &&
            (lzx->block_length & 1))
        {
          READ_IF_NEEDED;
          i_ptr++;
        }

        READ_BITS(lzx->block_type, 3);
        READ_BITS(i, 16); READ_BITS(j, 8);
        lzx->block_remaining = lzx->block_length = (i << 8) | j;

        switch (lzx->block_type) {
        case LZX_BLOCKTYPE_ALIGNED:

          for (i = 0; i < 8; i++) { READ_BITS(j, 3); lzx->ALIGNED_len[i] = j; }
          BUILD_TABLE(ALIGNED);
            /*@fallthrough@*/
        case LZX_BLOCKTYPE_VERBATIM:

          READ_LENGTHS(MAINTREE, 0, 256);
          READ_LENGTHS(MAINTREE, 256, LZX_NUM_CHARS + lzx->num_offsets);
          BUILD_TABLE(MAINTREE);

          if (lzx->MAINTREE_len[0xE8] != 0) lzx->intel_started = 1;

          READ_LENGTHS(LENGTH, 0, LZX_NUM_SECONDARY_LENGTHS);
          BUILD_TABLE_MAYBE_EMPTY(LENGTH);
          break;

        case LZX_BLOCKTYPE_UNCOMPRESSED:

          lzx->intel_started = 1;

          if (bits_left == 0) ENSURE_BITS(16);
          bits_left = 0; bit_buffer = 0;

          for (rundest = &buf[0], i = 0; i < 12; i++) {
            READ_IF_NEEDED;
            *rundest++ = *i_ptr++;
          }
          R0 = EndGetI32(&buf[0]);
          R1 = EndGetI32(&buf[4]);
          R2 = EndGetI32(&buf[8]);
          if (R0 >= lzx->window_size || R1 >= lzx->window_size ||
              R2 >= lzx->window_size)
          {
            return lzx->error = MSPACK_ERR_DECRUNCH;
          }
          break;

        default:
          return lzx->error = MSPACK_ERR_DECRUNCH;
        }
      }

      this_run = lzx->block_remaining;
      if (this_run > bytes_todo) this_run = bytes_todo;

      bytes_todo           -= this_run;
      lzx->block_remaining -= this_run;

      switch (lzx->block_type) {
      case LZX_BLOCKTYPE_ALIGNED:
      case LZX_BLOCKTYPE_VERBATIM:
        while (this_run > 0) {
          int main_element, length_footer, verbatim_bits, aligned_bits, extra;
          int match_length;
          unsigned int match_offset;
          READ_HUFFSYM(MAINTREE, main_element);
          if (main_element < LZX_NUM_CHARS) {

            window[window_posn++] = main_element;
            this_run--;
          }
          else {

            main_element -= LZX_NUM_CHARS;

            match_length = main_element & LZX_NUM_PRIMARY_LENGTHS;
            if (match_length == LZX_NUM_PRIMARY_LENGTHS) {
              if (lzx->LENGTH_empty) {
                return lzx->error = MSPACK_ERR_DECRUNCH;
              }
              READ_HUFFSYM(LENGTH, length_footer);
              match_length += length_footer;
            }
            match_length += LZX_MIN_MATCH;

            switch ((match_offset = (main_element >> 3))) {
            case 0: match_offset = R0; break;
            case 1: match_offset = R1; R1=R0; R0 = match_offset; break;
            case 2: match_offset = R2; R2=R0; R0 = match_offset; break;
            default:
              extra = (match_offset >= 36) ? 17 : extra_bits[match_offset];
              match_offset = position_base[match_offset] - 2;
              if (extra >= 3 && lzx->block_type == LZX_BLOCKTYPE_ALIGNED) {
                if (extra > 3) {
                  READ_BITS(verbatim_bits, extra - 3);
                  match_offset += verbatim_bits << 3;
                }
                READ_HUFFSYM(ALIGNED, aligned_bits);
                match_offset += aligned_bits;
              }
              else if (extra) {
                READ_BITS(verbatim_bits, extra);
                match_offset += verbatim_bits;
              }

              R2 = R1; R1 = R0; R0 = match_offset;
            }

            if ((window_posn + match_length) > lzx->window_size) {
              return lzx->error = MSPACK_ERR_DECRUNCH;
            }

            rundest = &window[window_posn];
            i = match_length;

            if (match_offset > window_posn) {
              if ((off_t)match_offset > lzx->offset)
              {
                return lzx->error = MSPACK_ERR_DECRUNCH;
              }

              j = match_offset - window_posn;
              if (j < 0 || j > (int) lzx->window_size) {
                return lzx->error = MSPACK_ERR_DECRUNCH;
              }
              runsrc = &window[lzx->window_size - j];
              if (j < i) {

                i -= j; while (j-- > 0) *rundest++ = *runsrc++;
                runsrc = window;
              }
              while (i-- > 0) *rundest++ = *runsrc++;
            }
            else {
              runsrc = rundest - match_offset;
              while (i-- > 0) *rundest++ = *runsrc++;
            }

            this_run    -= match_length;
            window_posn += match_length;
          }
        }
        break;

      case LZX_BLOCKTYPE_UNCOMPRESSED:

        rundest = &window[window_posn];
        window_posn += this_run;
        while (this_run > 0) {
          if ((i = i_end - i_ptr) == 0) {
            READ_IF_NEEDED;
          }
          else {
            if (i > this_run) i = this_run;
            lzx->sys->copy(i_ptr, rundest, i);
            rundest  += i;
            i_ptr    += i;
            this_run -= i;
          }
        }
        break;

      default:
        return lzx->error = MSPACK_ERR_DECRUNCH;
      }

      if (this_run < 0) {
        if ((unsigned int)(-this_run) > lzx->block_remaining) {
          return lzx->error = MSPACK_ERR_DECRUNCH;
        }
        lzx->block_remaining -= -this_run;
      }
    }

    if ((window_posn - lzx->frame_posn) != frame_size) {
      return lzx->error = MSPACK_ERR_DECRUNCH;
    }

    if (bits_left > 0) ENSURE_BITS(16);
    if (bits_left & 15) REMOVE_BITS(bits_left & 15);

    if (lzx->o_ptr != lzx->o_end) {
      return lzx->error = MSPACK_ERR_DECRUNCH;
    }

    if (lzx->intel_started && lzx->intel_filesize &&
        (lzx->frame < 32768) && (frame_size > 10))
    {
      unsigned char *data    = &lzx->e8_buf[0];
      unsigned char *dataend = &lzx->e8_buf[frame_size - 10];
      signed int curpos      = (int) lzx->offset;
      signed int filesize    = lzx->intel_filesize;
      signed int abs_off, rel_off;

      lzx->o_ptr = data;
      lzx->sys->copy(&lzx->window[lzx->frame_posn], data, frame_size);

      while (data < dataend) {
        if (*data++ != 0xE8) { curpos++; continue; }
        abs_off = (int) EndGetI32(data);
        if ((abs_off >= -curpos) && (abs_off < filesize)) {
          rel_off = (abs_off >= 0) ? abs_off - curpos : abs_off + filesize;
          data[0] = (unsigned char) rel_off;
          data[1] = (unsigned char) (rel_off >> 8);
          data[2] = (unsigned char) (rel_off >> 16);
          data[3] = (unsigned char) (rel_off >> 24);
        }
        data += 4;
        curpos += 5;
      }
    }
    else {
      lzx->o_ptr = &lzx->window[lzx->frame_posn];
    }
    lzx->o_end = &lzx->o_ptr[frame_size];

    i = (out_bytes < (off_t)frame_size) ? (unsigned int)out_bytes : frame_size;
    if (lzx->sys->write(lzx->output, lzx->o_ptr, i) != i) {
      return lzx->error = MSPACK_ERR_WRITE;
    }
    lzx->o_ptr  += i;
    lzx->offset += i;
    out_bytes   -= i;

    lzx->frame_posn += frame_size;
    lzx->frame++;

    if (window_posn == lzx->window_size)     window_posn = 0;
    if (lzx->frame_posn == lzx->window_size) lzx->frame_posn = 0;

  }

  if (out_bytes) {
    return lzx->error = MSPACK_ERR_DECRUNCH;
  }

  STORE_BITS;
  lzx->window_posn = window_posn;
  lzx->R0 = R0;
  lzx->R1 = R1;
  lzx->R2 = R2;

  return MSPACK_ERR_OK;
}

static void lzxd_free(struct lzxd_stream *lzx) {
  struct mspack_system *sys;
  if (lzx) {
    sys = lzx->sys;
    sys->free(lzx->inbuf);
    sys->free(lzx->window);
    sys->free(lzx);
  }
}

static void cabd_free_decomp(struct mscab_decompressor_p *self) {
  if (!self->d) return;
  if (self->d->state) lzxd_free(self->d->state);
  if (self->d->infh) self->system->close(self->d->infh);
  self->system->free(self->d);
  self->d = NULL;
}

static void cabd_close(struct mscab_decompressor *base, struct mscabd_cabinet *cab) {
  struct mscab_decompressor_p *self = (struct mscab_decompressor_p *) base;
  struct mspack_system *sys;
  struct mscabd_folder *fol, *nfol;
  struct mscabd_file *fi, *nfi;

  if (!self || !cab) return;
  sys = self->system;
  cabd_free_decomp(self);
  for (fi = cab->files; fi; fi = nfi) {
    nfi = fi->next;
    sys->free(fi->filename);
    sys->free(fi);
  }
  for (fol = cab->folders; fol; fol = nfol) {
    nfol = fol->next;
    sys->free(fol);
  }
  sys->free(cab);
}

static char *cabd_read_string(struct mspack_system *sys, struct mspack_file *fh) {
  off_t base = sys->tell(fh);
  char buf[256], *str;
  int len, i;

  if ((len = sys->read(fh, &buf[0], 256)) <= 0) return NULL;
  for (i = 0; i < len && buf[i]; i++) {}
  if (i == len || i == 0) return NULL;
  len = i + 1;
  if (sys->seek(fh, base + (off_t) len, MSPACK_SYS_SEEK_START)) return NULL;
  if (!(str = (char *) sys->alloc(sys, len))) return NULL;
  sys->copy(&buf[0], str, len);
  return str;
}

static int cabd_read_headers(struct mspack_system *sys, struct mspack_file *fh,
                             struct mscabd_cabinet_p *cab, off_t offset)
{
  int num_folders, num_files, folder_resv = 0, flags, i, fidx;
  struct mscabd_folder_p *fol;
  struct mscabd_folder *linkfol = NULL, *ifol;
  struct mscabd_file *file, *linkfile = NULL;
  unsigned char buf[64];

  cab->base.base_offset = offset;
  if (sys->seek(fh, offset, MSPACK_SYS_SEEK_START)) return MSPACK_ERR_SEEK;
  if (sys->read(fh, &buf[0], cfhead_SIZEOF) != cfhead_SIZEOF) return MSPACK_ERR_READ;
  if (EndGetI32(&buf[cfhead_Signature]) != 0x4643534D) return MSPACK_ERR_SIGNATURE;

  num_folders = EndGetI16(&buf[cfhead_NumFolders]);
  num_files   = EndGetI16(&buf[cfhead_NumFiles]);
  flags       = EndGetI16(&buf[cfhead_Flags]);
  if (!num_folders || !num_files) return MSPACK_ERR_DATAFORMAT;

  if (flags & (cfheadPREV_CABINET | cfheadNEXT_CABINET)) return MSPACK_ERR_DATAFORMAT;

  if (flags & cfheadRESERVE_PRESENT) {
    if (sys->read(fh, &buf[0], cfheadext_SIZEOF) != cfheadext_SIZEOF) return MSPACK_ERR_READ;
    folder_resv     = buf[cfheadext_FolderReserved];
    cab->block_resv = buf[cfheadext_DataReserved];
    if (sys->seek(fh, (off_t) EndGetI16(&buf[cfheadext_HeaderReserved]), MSPACK_SYS_SEEK_CUR)) {
      return MSPACK_ERR_SEEK;
    }
  }

  for (i = 0; i < num_folders; i++) {
    if (sys->read(fh, &buf[0], cffold_SIZEOF) != cffold_SIZEOF) return MSPACK_ERR_READ;
    if (folder_resv && sys->seek(fh, (off_t) folder_resv, MSPACK_SYS_SEEK_CUR)) return MSPACK_ERR_SEEK;
    if (!(fol = (struct mscabd_folder_p *) sys->alloc(sys, sizeof(struct mscabd_folder_p)))) {
      return MSPACK_ERR_NOMEMORY;
    }
    fol->base.next       = NULL;
    fol->base.comp_type  = EndGetI16(&buf[cffold_CompType]);
    fol->base.num_blocks = EndGetI16(&buf[cffold_NumBlocks]);
    fol->cab             = cab;
    fol->offset          = offset + (off_t) ((unsigned int) EndGetI32(&buf[cffold_DataOffset]));
    if (!linkfol) cab->base.folders = &fol->base;
    else linkfol->next = &fol->base;
    linkfol = &fol->base;
  }

  for (i = 0; i < num_files; i++) {
    if (sys->read(fh, &buf[0], cffile_SIZEOF) != cffile_SIZEOF) return MSPACK_ERR_READ;

    fidx = EndGetI16(&buf[cffile_FolderIndex]);
    if (fidx >= num_folders) return MSPACK_ERR_DATAFORMAT;
    if (!(file = (struct mscabd_file *) sys->alloc(sys, sizeof(struct mscabd_file)))) {
      return MSPACK_ERR_NOMEMORY;
    }
    for (ifol = cab->base.folders; fidx--;) ifol = ifol->next;
    file->next     = NULL;
    file->folder   = ifol;
    file->length   = EndGetI32(&buf[cffile_UncompressedSize]);
    file->offset   = EndGetI32(&buf[cffile_FolderOffset]);
    file->filename = cabd_read_string(sys, fh);
    if (!file->filename) {
      sys->free(file);
      return MSPACK_ERR_DATAFORMAT;
    }
    if (!linkfile) cab->base.files = file;
    else linkfile->next = file;
    linkfile = file;
  }
  return MSPACK_ERR_OK;
}

static struct mscabd_cabinet *cabd_search(struct mscab_decompressor *base, const char *filename) {
  struct mscab_decompressor_p *self = (struct mscab_decompressor_p *) base;
  struct mscabd_cabinet_p *cab;
  struct mspack_system *sys;
  struct mspack_file *fh;
  unsigned int last4 = 0;
  unsigned char c;
  off_t pos = 0;

  if (!self) return NULL;
  sys = self->system;
  if (!(fh = sys->open(sys, filename, MSPACK_SYS_OPEN_READ))) return NULL;

  while (sys->read(fh, &c, 1) == 1) {
    pos++;
    last4 = (last4 >> 8) | ((unsigned int) c << 24);
    if (last4 != 0x4643534D) continue;
    if (!(cab = (struct mscabd_cabinet_p *) sys->alloc(sys, sizeof(struct mscabd_cabinet_p)))) break;
    memset(cab, 0, sizeof(*cab));
    cab->base.filename = filename;
    if (!cabd_read_headers(sys, fh, cab, pos - 4)) {
      sys->close(fh);
      return &cab->base;
    }
    cabd_close(base, &cab->base);
    if (sys->seek(fh, pos, MSPACK_SYS_SEEK_START)) break;
  }

  sys->close(fh);
  return NULL;
}

static unsigned int cabd_checksum(unsigned char *data, unsigned int bytes, unsigned int cksum) {
  unsigned int len, ul = 0;

  for (len = bytes >> 2; len--; data += 4) {
    cksum ^= EndGetI32(data);
  }

  switch (bytes & 3) {
  case 3: ul |= (unsigned int) *data++ << 16; /*@fallthrough@*/
  case 2: ul |= (unsigned int) *data++ <<  8; /*@fallthrough@*/
  case 1: ul |= *data;
  }
  return cksum ^ ul;
}

static int cabd_sys_read_block(struct mspack_system *sys, struct mscabd_decompress_state *d, int *out) {
  unsigned char hdr[cfdata_SIZEOF];
  unsigned int cksum;
  int len;

  d->i_ptr = d->i_end = &d->input[0];

  if (sys->read(d->infh, &hdr[0], cfdata_SIZEOF) != cfdata_SIZEOF) return MSPACK_ERR_READ;
  if (d->folder->cab->block_resv &&
      sys->seek(d->infh, (off_t) d->folder->cab->block_resv, MSPACK_SYS_SEEK_CUR))
  {
    return MSPACK_ERR_SEEK;
  }

  len = EndGetI16(&hdr[cfdata_CompressedSize]);
  *out = EndGetI16(&hdr[cfdata_UncompressedSize]);
  if (len > CAB_INPUTMAX || *out > CAB_BLOCKMAX || !*out) return MSPACK_ERR_DATAFORMAT;
  if (sys->read(d->infh, d->i_end, len) != len) return MSPACK_ERR_READ;

  if ((cksum = EndGetI32(&hdr[cfdata_CheckSum]))) {
    unsigned int sum2 = cabd_checksum(d->i_end, (unsigned int) len, 0);
    if (cabd_checksum(&hdr[4], 4, sum2) != cksum) return MSPACK_ERR_CHECKSUM;
  }

  d->i_end += len;
  return MSPACK_ERR_OK;
}

static int cabd_sys_read(struct mspack_file *file, void *buffer, int bytes) {
  struct mscab_decompressor_p *self = (struct mscab_decompressor_p *) file;
  unsigned char *buf = (unsigned char *) buffer;
  struct mspack_system *sys = self->system;
  int avail, todo = bytes, outlen;

  while (todo > 0) {
    avail = self->d->i_end - self->d->i_ptr;
    if (avail) {
      if (avail > todo) avail = todo;
      sys->copy(self->d->i_ptr, buf, (size_t) avail);
      self->d->i_ptr += avail;
      buf  += avail;
      todo -= avail;
    }
    else {
      if (self->d->block++ >= self->d->folder->base.num_blocks) {
        self->read_error = MSPACK_ERR_DATAFORMAT;
        break;
      }
      self->read_error = cabd_sys_read_block(sys, self->d, &outlen);
      if (self->read_error) return -1;
      self->d->outlen += outlen;

      if (self->d->block >= self->d->folder->base.num_blocks) {
        lzxd_set_output_length(self->d->state, self->d->outlen);
      }
    }
  }
  return bytes - todo;
}

static int cabd_sys_write(struct mspack_file *file, void *buffer, int bytes) {
  struct mscab_decompressor_p *self = (struct mscab_decompressor_p *) file;
  self->d->offset += bytes;
  if (self->d->outfh) {
    return self->system->write(self->d->outfh, buffer, bytes);
  }
  return bytes;
}

static int cabd_init_decomp(struct mscab_decompressor_p *self, struct mscabd_folder_p *fol) {
  struct mspack_system *sys = self->system;
  struct mspack_file *fh = (struct mspack_file *) self;
  unsigned int ct = (unsigned int) fol->base.comp_type;

  cabd_free_decomp(self);
  if ((ct & cffoldCOMPTYPE_MASK) != cffoldCOMPTYPE_LZX) return MSPACK_ERR_DATAFORMAT;

  if (!(self->d = (struct mscabd_decompress_state *) sys->alloc(sys, sizeof(*self->d)))) {
    return MSPACK_ERR_NOMEMORY;
  }
  self->d->folder    = fol;
  self->d->offset    = 0;
  self->d->block     = 0;
  self->d->outlen    = 0;
  self->d->sys       = *sys;
  self->d->sys.read  = &cabd_sys_read;
  self->d->sys.write = &cabd_sys_write;
  self->d->outfh     = NULL;
  self->d->i_ptr     = self->d->i_end = &self->d->input[0];
  self->d->state     = lzxd_init(&self->d->sys, fh, fh, (int) (ct >> 8) & 0x1f, CAB_DECOMP_BUF);
  self->d->infh      = sys->open(sys, fol->cab->base.filename, MSPACK_SYS_OPEN_READ);
  if (!self->d->state) return MSPACK_ERR_NOMEMORY;
  if (!self->d->infh) return MSPACK_ERR_OPEN;
  if (sys->seek(self->d->infh, fol->offset, MSPACK_SYS_SEEK_START)) return MSPACK_ERR_SEEK;
  self->read_error = MSPACK_ERR_OK;
  return MSPACK_ERR_OK;
}

static int cabd_extract(struct mscab_decompressor *base, struct mscabd_file *file, const char *filename) {
  struct mscab_decompressor_p *self = (struct mscab_decompressor_p *) base;
  struct mscabd_folder_p *fol;
  struct mspack_system *sys;
  struct mspack_file *fh;
  unsigned int maxlen;
  off_t bytes;
  int error = MSPACK_ERR_OK;

  if (!self || !file) return MSPACK_ERR_ARGS;
  sys = self->system;
  fol = (struct mscabd_folder_p *) file->folder;

  maxlen = fol->base.num_blocks * CAB_BLOCKMAX;
  if (file->offset > maxlen || file->length > (maxlen - file->offset)) return MSPACK_ERR_DECRUNCH;

  if (!self->d || (self->d->folder != fol) || (self->d->offset > file->offset)) {
    if ((error = cabd_init_decomp(self, fol))) {
      cabd_free_decomp(self);
      return error;
    }
  }

  if (!(fh = sys->open(sys, filename, MSPACK_SYS_OPEN_WRITE))) return MSPACK_ERR_OPEN;

  self->d->outfh = NULL;
  if ((bytes = file->offset - self->d->offset)) error = lzxd_decompress(self->d->state, bytes);
  if (!error && file->length) {
    self->d->outfh = fh;
    error = lzxd_decompress(self->d->state, file->length);
  }

  sys->close(fh);
  self->d->outfh = NULL;
  return (error == MSPACK_ERR_READ) ? self->read_error : error;
}

struct mscab_decompressor *mspack_create_cab_decompressor(struct mspack_system *sys) {
  struct mscab_decompressor_p *self;

  if (!sys) sys = mspack_default_system;
  if (!(self = (struct mscab_decompressor_p *) sys->alloc(sys, sizeof(*self)))) return NULL;
  self->base.close   = &cabd_close;
  self->base.search  = &cabd_search;
  self->base.extract = &cabd_extract;
  self->system       = sys;
  self->d            = NULL;
  self->read_error   = MSPACK_ERR_OK;
  return &self->base;
}

void mspack_destroy_cab_decompressor(struct mscab_decompressor *base) {
  struct mscab_decompressor_p *self = (struct mscab_decompressor_p *) base;
  if (self) {
    cabd_free_decomp(self);
    self->system->free(self);
  }
}
