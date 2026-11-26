/* From libmspack (https://github.com/kyz/libmspack, commit 8f2a2a91ae2d12f985a196d481962ca4731f0f4d)
 * (C) 2003-2023 Stuart Caie.
 *
 * libmspack is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License (LGPL) version 2.1
 *
 * For further details, see the file COPYING.LIB distributed with libmspack
 *
 * Modified on 2026-11-26 for live-and-recompiled: cut down to the declarations
 * CAB extraction uses, and the structures to the members it keeps; comments removed.
 * This modified file is under the LGPL version 2.1 like the original.
 */

#ifndef LIB_MSPACK_H
#define LIB_MSPACK_H 1

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <stdlib.h>

struct mspack_file;

struct mspack_system {
  struct mspack_file *(*open)(struct mspack_system *self, const char *filename, int mode);
  void (*close)(struct mspack_file *file);
  int (*read)(struct mspack_file *file, void *buffer, int bytes);
  int (*write)(struct mspack_file *file, void *buffer, int bytes);
  int (*seek)(struct mspack_file *file, off_t offset, int mode);
  off_t (*tell)(struct mspack_file *file);
  void *(*alloc)(struct mspack_system *self, size_t bytes);
  void (*free)(void *ptr);
  void (*copy)(void *src, void *dest, size_t bytes);
};

#define MSPACK_SYS_OPEN_READ   (0)
#define MSPACK_SYS_OPEN_WRITE  (1)

#define MSPACK_SYS_SEEK_START  (0)
#define MSPACK_SYS_SEEK_CUR    (1)

#define MSPACK_ERR_OK          (0)
#define MSPACK_ERR_ARGS        (1)
#define MSPACK_ERR_OPEN        (2)
#define MSPACK_ERR_READ        (3)
#define MSPACK_ERR_WRITE       (4)
#define MSPACK_ERR_SEEK        (5)
#define MSPACK_ERR_NOMEMORY    (6)
#define MSPACK_ERR_SIGNATURE   (7)
#define MSPACK_ERR_DATAFORMAT  (8)
#define MSPACK_ERR_CHECKSUM    (9)
#define MSPACK_ERR_DECRUNCH    (11)

extern struct mscab_decompressor *mspack_create_cab_decompressor(struct mspack_system *system);
extern void mspack_destroy_cab_decompressor(struct mscab_decompressor *self);

struct mscabd_cabinet {
  const char *filename;
  off_t base_offset;
  struct mscabd_file *files;
  struct mscabd_folder *folders;
};

struct mscabd_folder {
  struct mscabd_folder *next;
  int comp_type;
  unsigned int num_blocks;
};

struct mscabd_file {
  struct mscabd_file *next;
  char *filename;
  unsigned int length;
  struct mscabd_folder *folder;
  unsigned int offset;
};

struct mscab_decompressor {

  void (*close)(struct mscab_decompressor *self, struct mscabd_cabinet *cab);

  struct mscabd_cabinet *(*search)(struct mscab_decompressor *self, const char *filename);

  int (*extract)(struct mscab_decompressor *self, struct mscabd_file *file, const char *filename);
};

#ifdef __cplusplus
}
#endif

#endif
