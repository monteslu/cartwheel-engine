/* .cwpak: the native asset pack cwpack writes and the native platform layer
 * reads (src/platform/native.c). All integers little-endian.
 *
 *   header   64 bytes (cwpak_header)
 *   data     entries' bytes, each starting on a CWPAK_ALIGN boundary, in
 *            index order
 *   index    count x cwpak_entry (48 bytes), sorted by path (bytewise), so a
 *            lookup is a binary search
 *   strings  the paths, NUL-terminated, in index order
 *
 * An entry is stored raw or compressed whole, by its flags: one raw-deflate
 * stream (CWPAK_DEFLATE, version 1 packs), one zstd frame (CWPAK_ZSTD) or one
 * lz4 block (CWPAK_LZ4, raw_size bytes out); cwpack keeps the compressed
 * form only when it saves at least a tenth. Readers take versions 1 and 2. hash is FNV-1a 32 over
 * the uncompressed bytes. Nothing in the file depends on time or on the
 * machine, so identical inputs give byte-identical packs. */
#ifndef CW_RESOURCE_CWPAK_H
#define CW_RESOURCE_CWPAK_H

#include <stdint.h>

#define CWPAK_MAGIC "CWPAK\r\n\x1a"
#define CWPAK_VERSION 2u
#define CWPAK_VERSION_MIN 1u
#define CWPAK_ALIGN 64u
#define CWPAK_DEFLATE 1u
#define CWPAK_ZSTD 2u
#define CWPAK_LZ4 4u

typedef struct cwpak_header {
  char magic[8];
  uint32_t version;
  uint32_t count;
  uint64_t index_offset;
  uint64_t strings_offset;
  uint64_t strings_size;
  uint32_t align;
  uint32_t flags;
  uint8_t reserved[16];
} cwpak_header;

typedef struct cwpak_entry {
  uint64_t uid;      /* the resource UID (cwpack's res.index has the same) */
  uint64_t offset;   /* from the start of the file */
  uint64_t size;     /* stored bytes */
  uint64_t raw_size; /* bytes after inflating (== size when raw) */
  uint32_t path_offset, path_len; /* into the strings block */
  uint32_t flags;
  uint32_t hash;     /* FNV-1a 32 of the raw bytes */
} cwpak_entry;

_Static_assert(sizeof(cwpak_header) == 64, "cwpak header is 64 bytes");
_Static_assert(sizeof(cwpak_entry) == 48, "cwpak entry is 48 bytes");

#endif
