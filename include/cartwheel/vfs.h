/* Project files: a writable view of a project for the editor and tools.
 *
 * Reads look in the writable layer first, then fall through to the app's
 * read-only assets (cw_asset_*). Writes:
 *   - natively, with a project directory set (cw_vfs_set_root or CW_PROJECT),
 *     go straight to disk;
 *   - in a cart (carts cannot write files: wasmcart has no write path), stay
 *     in memory, and the whole set of changes moves as ONE archive blob:
 *     cw_vfs_export / cw_vfs_import, reachable through the debug channel
 *     (`vfs export` / `vfs import <base64>`) so romdev or a companion
 *     process can carry a project in and out with no new ABI. (A host
 *     "project directory" capability would be an ABI addition.)
 *
 * Layers, top first: the in-memory layer (every write lands here too, so a
 * native write is read back without touching the disk), the project
 * directory on disk (native, when a root is set), then the assets (natively
 * the mounted .cwpak and the loose asset root, in a cart its bundle). A file
 * removed while a lower, read-only layer still has it (a cart's bundle, an
 * asset root that is not the project) is hidden by a REMOVAL mark in the
 * memory layer: it reads as missing, lists as absent and exports as a
 * removal, so the change travels with the archive.
 *
 * Paths are relative, '/'-separated, no "..". */
#ifndef CW_VFS_H
#define CW_VFS_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

void cw_vfs_set_root(const char *dir); /* native: write-through directory (NULL = memory only) */
const char *cw_vfs_root(void); /* the write-through directory, or NULL; native picks up $CW_PROJECT on first call */

bool cw_vfs_exists(const char *path); /* in any layer (memory, disk root, assets) and not marked removed */
/* malloc'd, NUL-terminated (not counted in *len); NULL when missing */
void *cw_vfs_read(const char *path, size_t *len);
/* Store a copy in the memory layer (and, native with a root, write the file, creating directories). false and
 * logged on a bad path or write error. */
bool cw_vfs_write(const char *path, const void *data, size_t len);
/* Remove a file: from the memory layer and the project directory; a file
 * that a read-only layer still serves gets a removal mark (see above).
 * false when the path names no file. */
bool cw_vfs_remove(const char *path);
/* Files in the writable layer (sorted; removal marks are not listed).
 * Returns the count; names are valid until the next write/remove/import. */
int cw_vfs_list(const char ***names);
int cw_vfs_changed_count(void); /* files + removal marks in the writable layer */

/* ── directories over every layer (the editor's FileSystem view) ──────
 * The asset layer is listed from: natively the pack's index and the loose
 * asset root on disk; in a cart the bundle manifest `cwfiles.txt` that
 * cw_add_app writes into every cart's asset directory (one path per line,
 * the files wasmcart-pack ships), else the pack's res.index. A cart has no
 * other way to list its bundle (wasmcart has no directory API; no ABI is
 * needed for this). */
typedef enum {
  CW_VFS_NONE,   /* missing, or hidden by a removal mark */
  CW_VFS_ASSET,  /* the app's read-only assets (pack, asset root, cart bundle) */
  CW_VFS_DISK,   /* the native project directory */
  CW_VFS_MEMORY, /* the in-memory layer only (a cart's edits, a native editor without a directory) */
} cw_vfs_layer;
typedef struct cw_vfs_entry {
  const char *name;   /* name inside the listed directory (interned) */
  bool is_dir;
  int64_t size;       /* bytes; -1 for a directory */
  cw_vfs_layer layer; /* the layer a read is served from (a directory: the top layer holding anything under it) */
} cw_vfs_entry;
enum {
  CW_VFS_LIST_HIDDEN = 1 << 0, /* include names starting with '.' (.cweditor/, .cwcache/) */
};
/* One directory ("" = the project root), every layer merged, directories
 * first, then files, each sorted by name. The bundle manifest itself is not
 * listed. Returns the count (entries valid until the next call), -1 when no
 * layer has that directory. */
int cw_vfs_list_dir(const char *dir, int flags, const cw_vfs_entry **out);
/* Every file under dir ("" = the whole project), depth first in
 * cw_vfs_list_dir order; cb gets each file's path and entry. Returns the
 * number of files. */
int cw_vfs_walk(const char *dir, int flags, void (*cb)(const char *path, const cw_vfs_entry *e, void *user), void *user);
/* The layer that serves path (CW_VFS_NONE when missing or removed). */
cw_vfs_layer cw_vfs_where(const char *path);
/* A directory in any layer (holding at least one file, or present on disk). */
bool cw_vfs_is_dir(const char *path);
/* Move a file, or a directory with everything under it (into the writable
 * layers; read-only originals get removal marks). Refuses (false, logged)
 * when `to` exists or `from` does not. */
bool cw_vfs_rename(const char *from, const char *to);
/* Drop the memory layer's copy or removal mark of path, so the layers
 * below serve it again (an undo restoring a bundled file exactly). Native
 * project files on disk are not touched. false when memory had nothing. */
bool cw_vfs_revert(const char *path);
/* Bumped by every write, remove, rename, import and clear: a cheap "did the
 * project change" check (it does not see other processes writing the disk:
 * cw_vfs_rescan rereads that). */
uint32_t cw_vfs_generation(void);
/* Forget the cached listing of the asset layer (after cw_asset_set_root, a
 * new pack, or files changed on disk by someone else). */
void cw_vfs_rescan(void);

/* The writable layer as one archive: "CWVFS1\n" then per file
 * "<path>\n<length>\n<bytes>", and per removal mark "<path>\nremoved\n".
 * Deterministic (sorted). cw_alloc'd. */
void *cw_vfs_export(size_t *len);
bool cw_vfs_import(const void *data, size_t len); /* merges; returns false on a malformed archive */
void cw_vfs_clear(void); /* drops the memory layer (edits and removals); disk files are untouched */

/* base64 for moving binary through text channels */
char *cw_base64_encode(const void *data, size_t len);          /* cw_alloc'd */
void *cw_base64_decode(const char *text, size_t *out_len);     /* cw_alloc'd, NULL on bad input */

#ifdef __cplusplus
}
#endif
#endif
