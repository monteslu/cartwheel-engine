/* cartwheel/quality.h: graphics settings, the presets that set them, and
 * the automatic choice.
 *
 * Each setting is something the player can pick on its own; a preset picks
 * them all:
 *
 *                     Low       Medium    High (default)  Ultra
 *   resolution        67%       75%       100%            100%
 *   ambient occl.     off       on        on              on
 *   shadows           Low       Medium    High            Ultra
 *   anti-aliasing     FXAA      FXAA      SMAA            SMAA
 *   effects detail    Low       Medium    High            Ultra
 *
 * Resolution: the 3D scene (and its post) renders at that share of the
 * output's size and is scaled up with a filter (Catmull-Rom); the 2D window
 * passes (HUD, menus, text) draw at the output's full size over it. It is the
 * biggest lever: GPU time of the scene and its post goes with its pixel
 * count. It applies on top of cw_app_desc.render_scale (the output size);
 * an app with pixel_scale draws at its own size and ignores it.
 *
 * Ambient occlusion: GTAO as the scene's PostProcess asks for it (On) or
 * none (Off); its sample count follows effects detail.
 *
 * Shadows: Low (maps of 1024 at most, hard PCF), Medium (1024 at most,
 * soft), High (as the lights ask, soft), Ultra (twice that, up to 4096,
 * soft).
 *
 * Anti-aliasing: Off, FXAA, or SMAA (as the PostProcess asks: a scene that
 * asks for FXAA or none keeps it).
 *
 * Effects detail (Low, Medium, High, Ultra): the share of GPU particles
 * drawn (0.4, 0.7, 1, 1), the density games thin their instancing by (0.35,
 * 0.65, 1, 1: grass, crowd, scatter), the bloom chain (3, 5 levels, as
 * authored), GTAO's samples (1 slice of 4 steps, 1 of 4, as authored, 3
 * slices of 8 steps) and the LOD error budget (x2, x1.5, x1, x1: coarser
 * meshes sooner).
 *
 * "As authored" is what the scene's PostProcess / lights ask for: High
 * draws exactly what the game would without this module.
 *
 * Game settings: a game adds its own render-only levers (cw_quality_lever_add:
 * a cheaper material at distance, say) with the value each preset picks;
 * they show in the graphics menu after the engine's, and are saved with them.
 *
 * Density: games read cw_quality_scale("grass"), ("crowd"), ("scatter") or
 * the shorthand cw_quality_density() and thin their instancing; set a change
 * callback to re-thin when a setting changes. Particles: the engine draws
 * cw_quality_scale("particles") of every GPU particle system's particles
 * (they are all still simulated, so counts and seeds agree).
 *
 * The player picks a preset, Auto, or each setting (the graphics menu,
 * cartwheel/quality_menu.h, or the game shell's settings); changing one
 * setting shows the preset as "Custom". Persist the choice in the game's
 * save data (cw_quality_saved / cw_quality_restore below).
 *
 * Auto: the engine picks, and holds the frame's cost under a budget. While
 * the game says it is in play (cw_quality_auto_measure(true) every frame) it
 * averages the cost: the GPU time of the frame's passes where the host
 * times them cheaply (native WebGPU's timestamps), else the frame-to-frame
 * time (GL, a cart: a frame the GPU cannot finish in time arrives late). After a couple of
 * seconds over budget it steps down one level of its ladder:
 *
 *   High 100%, High 85%, Medium 75%, Medium 67%, Low 67%, Low 60%, Low 50%
 *
 * (resolution first: it costs the picture least for what it saves). Where
 * it measures GPU time it also steps back up when the cost has stayed well
 * under budget, never into a level that was over it in this run. A game
 * makes Auto the starting choice with cw_quality_auto(true, budget); the
 * player can always pick it. The level it reached is saved, so the next run
 * starts there.
 *
 * Determinism: a deterministic run (a test) starts at High whatever the
 * save holds, with Auto off, so every host draws the same frame; a choice
 * made in it (from the menus, by input every host replays alike) applies.
 * Nothing here reaches the simulation. */
#ifndef CW_QUALITY_H
#define CW_QUALITY_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CW_QUALITY_LOW, CW_QUALITY_MEDIUM, CW_QUALITY_HIGH, CW_QUALITY_ULTRA, CW_QUALITY_COUNT } cw_quality;

/* The preset now: the one picked, Auto's current level, or (Custom) the
 * preset the settings were last set from. */
cw_quality cw_quality_get(void);
/* Choose a preset (every setting). user = true: the player chose it (Auto
 * stops); false: the game or a tool did (Auto, when on, may still change
 * it). Calls the change callback. */
void cw_quality_set(cw_quality q, bool user);
/* "Low", "Medium", "High", "Ultra" */
const char *cw_quality_name(cw_quality q);

/* ── the settings ──────────────────────────────────────────────────── */

typedef enum {
  CW_GFX_RESOLUTION, /* 50%, 60%, 67%, 75%, 85%, 100% of the output */
  CW_GFX_SSAO,       /* Off, On */
  CW_GFX_SHADOWS,    /* Low, Medium, High, Ultra */
  CW_GFX_AA,         /* Off, FXAA, SMAA */
  CW_GFX_EFFECTS,    /* Low, Medium, High, Ultra */
  CW_GFX_ENGINE_COUNT
} cw_gfx_setting;
enum { CW_GFX_MAX_LEVERS = 4, CW_GFX_LEVER_MAX_VALUES = 4 };

/* Settings shown: the engine's, then the game's levers (ids
 * CW_GFX_ENGINE_COUNT + i, in the order they were added). */
int cw_gfx_count(void);
const char *cw_gfx_name(int s);        /* "Resolution", "Ambient occlusion", ... or the lever's name */
int cw_gfx_value_count(int s);         /* how many values s has */
int cw_gfx_get(int s);                 /* its value now (an index) */
/* its value as a menu shows it: "75%  1440x810", "On", "SMAA", ... */
const char *cw_gfx_value_label(int s);
/* The player sets one (value clamped): the preset becomes Custom unless the
 * settings now match one, and Auto stops. */
void cw_gfx_set(int s, int value);
/* dir +1 / -1: the next / previous value (wrapping) */
void cw_gfx_step(int s, int dir);
/* The resolution setting's share of the output (0.5 .. 1). */
float cw_gfx_render_scale(void);

/* A game's own render-only setting (at most CW_GFX_MAX_LEVERS, each with up
 * to CW_GFX_LEVER_MAX_VALUES values, cheapest first): its name, its values'
 * labels (kept by pointer: static strings) and the value each preset picks
 * (preset[CW_QUALITY_LOW] ...). Call in init, before cw_quality_restore (the
 * saved value applies to it). Returns its setting id (for cw_gfx_get), -1
 * when full. */
int cw_quality_lever_add(const char *name, const char *const *values, int value_count,
                         const uint8_t preset[CW_QUALITY_COUNT]);

/* ── reading the settings ──────────────────────────────────────────── */

/* A scale for a named knob, from effects detail: "particles", "density",
 * "grass", "crowd", "scatter" (the last three are density), 1 for any other
 * name. */
float cw_quality_scale(const char *knob);
float cw_quality_density(void); /* cw_quality_scale("density") */
/* n scaled by a knob, at least 1 when n > 0 */
int cw_quality_count(int n, const char *knob);

/* Called after any setting changes (a preset, a setting, an Auto step), on
 * the main thread between frames. One callback; NULL removes it. The
 * argument is cw_quality_get(). */
typedef void (*cw_quality_change_fn)(cw_quality q, void *user);
void cw_quality_on_change(cw_quality_change_fn fn, void *user); /* the change callback (NULL removes it) */

/* ── Auto ──────────────────────────────────────────────────────────── */

/* on: Auto is the choice until the player picks something else (a save
 * with a choice in it wins). budget_ms <= 0 keeps the default: 19 ms of
 * frame time (a 60 Hz display that keeps missing frames), or three quarters
 * of it of GPU time where the GPU is timed. Off in a deterministic run. */
void cw_quality_auto(bool on, float budget_ms);
void cw_quality_auto_measure(bool in_play); /* every frame: true while the frame time means something */
bool cw_quality_is_auto(void); /* Auto is choosing (not a test run) */
bool cw_quality_is_custom(void); /* the settings match no preset (and Auto is off) */
/* Auto's measured cost in ms (averaged; GPU time or frame time, as above),
 * 0 before it has measured */
float cw_quality_auto_cost(void);

/* ── a settings entry ──────────────────────────────────────────────── */

/* The next choice in Low, Medium, High, Ultra, Auto, Low... (a player's
 * choice; from Custom, the preset after the one it came from).
 * Render-only: call it from update or event handling, never from the
 * fixed-step simulation. The frame's raw presses (cw_input_raw:
 * keys_pressed, pads[].pressed, mouse_pressed) hold for the whole frame, so
 * update sees them as the ticks do. */
void cw_quality_cycle(void);
/* the previous choice in that order */
void cw_quality_cycle_back(void);
/* "Low", "Medium", "High", "Ultra", "Auto" or "Custom": what a settings entry shows */
const char *cw_quality_label(void);
/* The render resolution as a graphics menu shows it: "<lines>p" of the
 * scene's render size (cw_render_size, the resolution setting applied),
 * e.g. "720p", "1080p". The output's size is the host's (RetroArch: Quick
 * Menu > Core Options > Internal Resolution). */
const char *cw_render_res_label(void);

/* Save data: a word the game keeps in its save (0 = none chosen). It holds
 * the choice (a preset, Auto and the level it reached, or every setting of a
 * Custom choice, the game's levers too); cw_quality_restore applies it (at
 * init, after the save loaded and the levers were added; not in a
 * deterministic run). It fits in 13 bits
 * (two bytes) for a game with no levers, 21 bits (three bytes) with four.
 * A byte 1..4 from an older save is still a preset. */
uint32_t cw_quality_saved(void);
void cw_quality_restore(uint32_t saved); /* apply a cw_quality_saved word (0 = none chosen) */

#ifdef __cplusplus
}
#endif

#endif
