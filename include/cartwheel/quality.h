/* cartwheel/quality.h: graphics quality presets (Low, Medium, High, Ultra).
 *
 * A preset changes only what the frame costs, never the resolution (that is
 * cw_app_desc.render_scale) and never the simulation:
 *
 *               Low       Medium    High (default)   Ultra
 *   GTAO        off       1 slice,  as authored      3 slices, 8 steps
 *                         4 steps
 *   bloom       3 levels  5 levels  as authored      as authored
 *   shadows     <= 1024,  <= 2048,  as authored      2x (<= 4096), soft
 *               hard PCF  soft      (soft PCF)
 *   antialias   off       FXAA      as authored      SMAA when any
 *   particles   0.4       0.7       1                1
 *   density     0.35      0.65      1                1   (grass, crowd, scatter)
 *
 * "As authored" is what the scene's PostProcess / lights ask for: High draws
 * exactly what the game would without this module. Lower presets cap and
 * thin it; Ultra raises the effects (never the instance counts, which the
 * game authors at their maximum).
 *
 * Density: games read cw_quality_scale("grass"), ("crowd"), ("scatter") or
 * the shorthand cw_quality_density() and thin their instancing; set a
 * change callback to re-thin when the preset changes. Particles: the engine
 * draws cw_quality_scale("particles") of every GPU particle system's
 * particles (they are all still simulated, so counts and seeds agree).
 *
 * The player picks the preset (cw_quality_cycle from a settings entry);
 * persist the choice in the game's save data (cw_quality_saved /
 * cw_quality_restore below).
 *
 * Automatic choice (OFF unless the game turns it on with cw_quality_auto):
 * while the game says it is in play (cw_quality_auto_measure(true)), the
 * engine averages the frame time; after a couple of seconds over budget it
 * steps down one preset, and it never steps up again in that run. A preset
 * the player picked (user = true) turns it off.
 *
 * Determinism: a deterministic run (a test) is always High, with automatic
 * choice off, so every host draws the same frame. Nothing here reaches the
 * simulation. */
#ifndef CW_QUALITY_H
#define CW_QUALITY_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CW_QUALITY_LOW, CW_QUALITY_MEDIUM, CW_QUALITY_HIGH, CW_QUALITY_ULTRA, CW_QUALITY_COUNT } cw_quality;

/* The preset now. */
cw_quality cw_quality_get(void);
/* Choose a preset. user = true: the player chose it (automatic choice stops
 * for good); false: the game or a tool did (automatic choice may still step
 * down from it). Ignored in a deterministic run. Calls the change callback. */
void cw_quality_set(cw_quality q, bool user);
/* "Low", "Medium", "High", "Ultra" */
const char *cw_quality_name(cw_quality q);

/* A preset's scale for a named knob: "particles", "density", "grass",
 * "crowd", "scatter" (the last three are density), 1 for any other name. */
float cw_quality_scale(const char *knob);
float cw_quality_density(void); /* cw_quality_scale("density") */
/* n scaled by a knob, at least 1 when n > 0 */
int cw_quality_count(int n, const char *knob);

/* Called after the preset changes (from cw_quality_set or an automatic step
 * down), on the main thread between frames. One callback; NULL removes it. */
typedef void (*cw_quality_change_fn)(cw_quality q, void *user);
void cw_quality_on_change(cw_quality_change_fn fn, void *user); /* the change callback (NULL removes it) */

/* Automatic choice: off by default (and always in a deterministic run). The game
 * says when the frame time means something (in play, not in a menu or a
 * loading screen); budget_ms <= 0 keeps the default (19 ms: a 60 Hz display
 * that keeps missing frames). */
void cw_quality_auto(bool on, float budget_ms);
void cw_quality_auto_measure(bool in_play); /* every frame: true while the frame time means something */
bool cw_quality_is_auto(void); /* still choosing by itself (no user choice, not a test run) */

/* A settings entry in one call: the next preset in Low, Medium, High,
 * Ultra, Low... (a player's choice). Render-only: call it from update or
 * event handling, never from the fixed-step simulation. The frame's raw
 * presses (cw_input_raw: keys_pressed, pads[].pressed, mouse_pressed) hold
 * for the whole frame, so update sees them as the ticks do. A
 * deterministic run (a test, CW_FRAMES, a replaying host) keeps High: there
 * this is a no-op, so check it in a normal run. */
void cw_quality_cycle(void);
/* "Low", "Medium", "High" or "Ultra": what a settings entry shows */
const char *cw_quality_label(void);
/* The render resolution as a graphics menu shows it: "<lines>p" of the
 * scene's render size (cw_render_size), e.g. "540p", "1080p". The host sets
 * it (RetroArch: Quick Menu > Core Options > Internal Resolution); a game
 * shows it, it does not change it. */
const char *cw_render_res_label(void);

/* Save data: one byte the game keeps in its save (0 = none chosen, 1..4 = the
 * player's preset + 1). cw_quality_restore applies it (at init, after the
 * save loaded). */
uint8_t cw_quality_saved(void);
void cw_quality_restore(uint8_t saved); /* apply a cw_quality_saved byte (0 = none chosen) */

#ifdef __cplusplus
}
#endif

#endif
