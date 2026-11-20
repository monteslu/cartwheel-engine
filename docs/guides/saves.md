# Saves

A game's save data is one block of bytes the host keeps between runs: a file natively, the save region in a cart. This page is about laying that block out so that a game update never costs a player their progress.

Headers: [cartwheel/save.h](header:save.h) (the slot store), [cartwheel/shell.h](header:shell.h) (the game shell's save), [cartwheel/platform.h](header:platform.h) (`save` in the app description, `cw_save_commit`). Unit suite: `unit_save` (gate `unit-save`).

## The problem

Laid out as one C struct, a save breaks the day the struct changes: the old bytes no longer mean what the new code reads. The usual answer, "the save is from another version, start over", throws away what the player earned.

There are two ways out, and both are here: a struct that is allowed to grow, and named slots.

## A struct that grows (the game shell)

A game on the [game shell](guide:game-shell) names a struct for its own data and gets it back with `cw_shell_game_save()`. Three rules keep an old save alive:

- **Grow the struct at its end and change nothing else.** A save from a build where the struct was smaller (or larger) keeps the bytes both have; the new fields read zero. No version change, no code.
- **Change `game_save_version` only when the layout changes in a way a copy cannot follow** (a field removed, moved or resized), and give `game_save_migrate`: it receives the old version and bytes and fills the new struct.

```c
static bool migrate(uint16_t old_version, const void *old, size_t old_size, void *out, size_t out_size, void *user) {
  if (old_version != 1) return false;      /* a version this build cannot read: start over */
  const save_v1 *a = old;
  save_v2 *b = out;                        /* zeroed */
  b->best = a->best;                       /* was 32 bits, now 64 */
  b->unlocked = a->unlocked;
  return true;
}
cw_shell_init(&(cw_shell_desc){ .title = "My Game", .game_save_size = sizeof(save_v2), .game_save_version = 2,
                                .game_save_migrate = migrate, .start = start_match });
```

- **The settings are never lost to a game change.** Volumes, graphics and text size live beside the game's data; a version the game cannot read starts the game's data over and leaves them alone.

Write the struct, then `cw_shell_save_commit()`.

## Named slots (the save store)

The store lays the block out as slots: each has a name, a version, a size and its own checksum, and is read and written by name. Use it for anything that is more than one struct: three save files, per-level records, options a later update adds.

```c
cw_save_store *st = cw_shell_save_store();      /* cw_shell_desc.game_save_slots = true */
progress p;
cw_save_read(st, "progress", PROGRESS_VERSION, &p, sizeof p, migrate_progress, NULL);
p.coins += 10;
cw_save_write(st, "progress", PROGRESS_VERSION, &p, sizeof p);
cw_shell_save_commit();
```

A game without the shell gives its app a buffer (`cw_app_desc.save`, `save_size`) and asks for `cw_save_app_store`, or binds a store to any block with `cw_save_open`; `cw_save_commit` flushes it.

`cw_save_read` always fills the struct, whatever was stored, and says what happened:

| Result | What was stored | What the struct holds |
|---|---|---|
| `CW_SAVE_NONE` | no such slot | zeros: a new game |
| `CW_SAVE_OK` | the same version and size | the bytes |
| `CW_SAVE_RESIZED` | the same version, another size | the bytes both have, zeros after them |
| `CW_SAVE_MIGRATED` | an older version | what the migrate hook made of it |
| `CW_SAVE_DISCARDED` | an older version, and no hook (or the hook gave up) | zeros; the stored bytes stay until the slot is next written |
| `CW_SAVE_NEWER` | a later version of the game wrote it | zeros; the stored bytes are left alone |

So the same two rules hold per slot: grow a struct at its end freely; change a slot's version only with a hook that reads the older ones.

Other calls: `cw_save_erase`, `cw_save_info` (a slot's version and size), `cw_save_count` and `cw_save_name` (list them: a load menu), `cw_save_room` (how large a new slot may still be), `cw_save_clear`. A write that does not fit is refused and changes nothing. A slot costs 12 bytes plus its name and data, each rounded up to 4; the shell's store is `CW_SHELL_GAME_SAVE_MAX` bytes.

### Damage

Every slot carries its own checksum. Opening a store drops a slot whose checksum fails and cuts off everything after a slot whose size makes no sense; the slots before it survive. On the shell, the slots are also kept out of the settings' checksum, so a block the host captured between a slot write and the next commit still loads, settings and slots both.

### Moving an existing game to slots

A game that already shipped with a struct keeps its players: on the first run over an old save the hook gets the old bytes and an empty store to write slots into.

```c
static void import(cw_save_store *st, uint16_t old_version, const void *old, size_t old_size, void *user) {
  if (old_version == 3 && old_size == sizeof(old_save)) cw_save_write(st, "career", 1, old, old_size);
}
cw_shell_init(&(cw_shell_desc){ .title = "My Game", .game_save_slots = true, .game_save_import = import, .start = start_match });
```

Without the shell, `cw_save_open` and `cw_save_app_store` take a `legacy` hook that does the same for a raw block.

## What is in the block

The format is fixed and little-endian, the same on every host: `"CWS1"`, the bytes used, then the slots, each a 12 byte header (name length, version, size, checksum), the name and the data. Nothing in the store allocates, reads a clock or touches a file, and it needs nothing from the host beyond the save data every app already has. A test run starts from zeros and writes nothing, so tests never depend on an earlier run.
