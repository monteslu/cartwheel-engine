/* The save store: named, versioned slots inside a game's save data, so a game can grow what it saves without
 * losing what players already have.
 *
 * A game's save data is one block of bytes the host keeps (cw_app_desc.save: a file natively, the wasmcart
 * save region in a cart). Laid out as one C struct, any change to that struct orphans every existing save.
 * The store lays the same block out as SLOTS instead: each has a name ("progress", "options", "slot1"), a
 * version number, a size and its own checksum, and is read and written by name.
 *
 *   static uint8_t region[8192];                       // cw_app_desc.save = region, .save_size = sizeof region
 *   cw_save_store st;
 *   cw_save_open(&st, region, sizeof region, NULL, NULL);
 *   progress p;
 *   cw_save_read(&st, "progress", PROGRESS_VERSION, &p, sizeof p, migrate_progress, NULL);
 *   ...
 *   cw_save_write(&st, "progress", PROGRESS_VERSION, &p, sizeof p);
 *   cw_save_commit();                                  // cartwheel/platform.h: flush the block now
 *
 * A game on the game shell (cartwheel/shell.h) sets cw_shell_desc.game_save_slots and uses
 * cw_shell_save_store(): the store then lives in the shell's block beside the settings.
 *
 * ── growing a save ───────────────────────────────────────────────────
 * Reading a slot fills the caller's struct whatever was stored:
 *   - no such slot: zeros (CW_SAVE_NONE). A new game.
 *   - the same version and size: the bytes (CW_SAVE_OK).
 *   - the same version, another size: the bytes both have, zeros for the rest (CW_SAVE_RESIZED). So a struct
 *     that only GROWS AT ITS END needs no version change and no code: old saves load, the new fields read 0.
 *   - an older version: the migrate hook gets the old version and bytes and fills the new struct
 *     (CW_SAVE_MIGRATED). Without a hook, or when the hook returns false, the struct is zeros
 *     (CW_SAVE_DISCARDED); the stored bytes stay until the slot is next written, so a later build with a hook
 *     can still read them.
 *   - a newer version (a save from a later build of the game): zeros (CW_SAVE_NEWER), stored bytes left alone.
 * Change a slot's version only when its layout changes in a way a prefix copy cannot follow (a field removed,
 * reordered or resized), and write the hook that reads each older version.
 *
 * ── damage ───────────────────────────────────────────────────────────
 * Every slot has its own checksum. Opening the store drops a slot whose checksum fails and everything after a
 * slot whose bounds make no sense; the slots before it survive. A block that is not a store at all (zeros: a
 * first run; or a game's older raw struct) is formatted empty, after the `legacy` hook has had the old bytes
 * to turn into slots.
 *
 * ── the format (little-endian, fixed) ────────────────────────────────
 *   0   "CWS1"
 *   4   u32  bytes used, this header included
 *   8   slots, each: u8 name length, u8 0, u16 version, u32 size, u32 checksum (FNV-1a of name then data),
 *       name, zeros to a multiple of 4, data, zeros to a multiple of 4
 * A slot costs 12 bytes plus its name and data, each rounded up to 4. Names are 1..63 bytes.
 *
 * Nothing here allocates beyond a temporary copy of a slot being migrated (cw_frame_alloc), reads a clock or
 * touches a file: the store is plain memory the caller owns, the same on every host. Not thread safe. */
#ifndef CW_SAVE_H
#define CW_SAVE_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_save_store {
  uint8_t *base;  /* the block; NULL until opened */
  size_t size;    /* its size in bytes */
} cw_save_store;

typedef enum {
  CW_SAVE_NONE,      /* no such slot: out is zeros */
  CW_SAVE_OK,        /* read as stored */
  CW_SAVE_RESIZED,   /* same version, another size: the shared prefix, zeros after it */
  CW_SAVE_MIGRATED,  /* an older version, converted by the hook */
  CW_SAVE_DISCARDED, /* an older version with no hook (or the hook refused): out is zeros */
  CW_SAVE_NEWER,     /* written by a later version of the game: out is zeros */
} cw_save_result;

/* Convert an older version of a slot: old/old_size are the stored bytes (version old_version), out/out_size
 * the struct being read, zeroed. Return false to give up (the read then reports CW_SAVE_DISCARDED). */
typedef bool (*cw_save_migrate_fn)(uint16_t old_version, const void *old, size_t old_size, void *out, size_t out_size,
                                   void *user);
/* The block held something that is not a store (a game's save from before it used one): old/old_size are
 * those bytes. Write what should be kept into slots of st (already formatted empty). */
typedef void (*cw_save_legacy_fn)(cw_save_store *st, const void *old, size_t old_size, void *user);

/* Bind a store to a block of size bytes (at least 16) and check it: a valid store keeps its slots (damaged
 * ones are dropped, see above), anything else becomes an empty store (after `legacy`, when given and the block
 * was not all zeros). Returns false (logged) only for a NULL or too small block. */
bool cw_save_open(cw_save_store *st, void *block, size_t size, cw_save_legacy_fn legacy, void *user);
/* Read a slot into out (size bytes, always fully written): see the rules at the top. */
cw_save_result cw_save_read(cw_save_store *st, const char *slot, uint16_t version, void *out, size_t size,
                            cw_save_migrate_fn migrate, void *user);
/* Write (or replace) a slot. false (logged, the store unchanged) when it does not fit or the name is not 1..63
 * bytes. Call cw_save_commit (platform.h) or cw_shell_save_commit to flush the block. */
bool cw_save_write(cw_save_store *st, const char *slot, uint16_t version, const void *data, size_t size);
bool cw_save_erase(cw_save_store *st, const char *slot); /* false when there was no such slot */
/* A slot's stored version and size without reading it; false when absent. Either pointer may be NULL. */
bool cw_save_info(const cw_save_store *st, const char *slot, uint16_t *version, size_t *size);
int cw_save_count(const cw_save_store *st); /* slots in the store */
/* The i-th slot's name (storage order: oldest write first), copied into buf; false when out of range. */
bool cw_save_name(const cw_save_store *st, int index, char *buf, size_t cap);
size_t cw_save_used(const cw_save_store *st); /* bytes used, header included */
/* The largest data size a NEW slot with this name could still be given. */
size_t cw_save_room(const cw_save_store *st, const char *slot);
void cw_save_clear(cw_save_store *st); /* erase every slot */

/* A store over the app's own save data (cw_app_desc.save), opened on first use; NULL (logged) when the app
 * has none or it is under 16 bytes. For games that do not use the game shell. `legacy` as in cw_save_open, used
 * by the call that opens it. */
cw_save_store *cw_save_app_store(cw_save_legacy_fn legacy, void *user);

#ifdef __cplusplus
}
#endif
#endif
