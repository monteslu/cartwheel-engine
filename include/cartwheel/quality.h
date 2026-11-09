/* cartwheel/quality.h: graphics settings, the presets that set them, and
 * the automatic choice.
 *
 * Two independent choices:
 *
 *   the graphics preset (Very Low, Low, Medium, High, Ultra, Auto, or Custom
 *   once a setting was changed on its own) decides how much the engine draws;
 *
 *   Resolution (the 3D scene's share of the output, 50% to 100%) is the
 *   player's alone: no preset sets it, Auto leaves it (unless the game opts
 *   in, cw_quality_full_resolution below), a preset with any resolution still
 *   shows the preset's name (High at 75% is "High", the resolution row says
 *   75%), and the save keeps both. The output's own size (a 720p window on a
 *   4K screen, a 4K display) is the host's: its window or display settings
 *   (RetroArch: Quick Menu > Core Options > Internal Resolution). The
 *   resolution row is the 3D scene's share of that output.
 *
 * Each setting is something the player can pick on its own; a preset picks
 * them all:
 *
 *                    Very Low   Low      Medium   High (default)  Ultra
 *   ambient occl.    off        off      on       on              on
 *   shadows          Minimal    Low      Medium   High            Ultra
 *   anti-aliasing    FXAA       FXAA     SMAA     Full            Full
 *   effects detail   Very Low   Low      Medium   High            Ultra
 *
 * High draws exactly what the game would without this module ("as
 * authored": what the scene's PostProcess and lights ask for).
 *
 * Resolution: the 3D scene (and its post) renders at that share of the
 * output's size and is scaled up with a filter (Catmull-Rom); the 2D window
 * passes (HUD, menus, text) draw at the output's full size over it. It
 * applies on top of cw_app_desc.render_scale (the output size); an app with
 * pixel_scale draws at its own size and ignores it.
 *
 * Ambient occlusion: GTAO as the scene's PostProcess asks for it (On) or
 * none (Off); its sample count follows effects detail (1 slice of 4 steps on
 * Medium and below, as authored on High, 3 slices of 8 steps on Ultra).
 *
 * Shadows:
 *   Minimal  the sun only (omni and spot lights cast none), one cascade,
 *            maps of 512 to 1024 (half the authored size), hard edges, half
 *            the shadow distance, and casters small on screen culled (below).
 *            It keeps the sun's shadow under characters and props, so nothing
 *            floats.
 *   Low      maps of 1024 at most, hard edges, 2 cascades, small casters culled
 *   Medium   1024 at most, soft, 3 cascades, tiny casters culled
 *   High     as the lights ask
 *   Ultra    twice that (up to 4096), soft, the cascades' distance x1.5
 * Caster culling: a mesh whose bounding sphere's radius over its distance
 * to the camera is under the setting's share (Minimal 0.02, about 40
 * pixels across at 1080p and a 60 degree view; Low 0.012, Medium 0.006;
 * High and Ultra none) draws into no shadow map this frame. The scene's big
 * shapes keep casting, and so does anything near the camera.
 *
 * Anti-aliasing: Off, FXAA, SMAA, or Full (as the PostProcess asks, the
 * scene's MSAA included). FXAA and SMAA drop the scene's MSAA (PostProcess
 * msaa) and put that post filter in its place (a scene that asks for none
 * at all keeps none; one that asks for FXAA keeps FXAA on SMAA). Off and
 * FXAA also draw without TAA (and its velocity pass). A game
 * whose look needs MSAA keeps it on every value but Off with
 * cw_quality_keep_msaa(true). WebGPU multisamples 1x or 4x only, so no
 * value goes above what the scene asks.
 *
 * Effects detail (Very Low, Low, Medium, High, Ultra):
 *
 *                       Very Low  Low     Medium  High         Ultra
 *   particles drawn     0.25      0.4     0.7     1            1
 *   density             0.2       0.35    0.65    1            1      (grass, crowd, scatter)
 *   LOD error budget    x3        x2      x1.5    x1           x0.75  (coarser meshes sooner)
 *   clustered lights    x0.25     x0.375  x0.5    x1           x1     (of LightingSettings.max_per_cluster)
 *   bloom               off       3 lvls  5 lvls  as authored  as authored (Low: its first level from 4 reads)
 *   volumetric fog      off       off     96x48   160x64       240x96 (cells across x slices; off: plain height fog)
 *   distortion buffer   off       off     half    full         full   (cartwheel/explosions.h)
 *   explosion lights    0         0.25    0.5     1            1      ("flash_lights")
 *   skin shading        plain     plain   on      on           on     (cartwheel/skin_shading.h)
 *   lighting model      plain     full    full    full         full   ("shading": 0 plain, 1 full)
 *   SSR                 off       off     half    as authored  x1.5   (march steps)
 *   depth of field      off       off     on      on           on
 *   SSGI                off       off     off     off          on
 *   HRTF voices         0.25      0.25    0.5     1            1      (cartwheel/audio.h)
 *   HDR scene           R11G11B10 R11G11B10 RGBA16F RGBA16F    RGBA16F (half the bytes; where the device renders it)
 *   sky re-render       8 frames  4       2       each change  each change (at most once per; ProceduralSky)
 *   water               Low's grid halved, no reflection, refraction or caustics on Very Low (cartwheel/water.h)
 *   terrain             the low layer set and the coarsest far ring on Very Low and Low; on Very Low each
 *                       layer read once (no stochastic tiling) and the coarser levels from 0.6 of their
 *                       distance (cartwheel/terrain.h)
 *   post passes         on Very Low and Low the height fog and FXAA run inside the tone mapping pass, on Low
 *                       with bloom's composite (below)
 *
 * The plain lighting model (Very Low): MeshStandardMaterial and MeshPhysicalMaterial keep their colour,
 * maps, roughness, metalness, normals, emission and light maps, and are lit with Lambert diffuse and a
 * Blinn-Phong highlight sized by the roughness. The environment map lights them with its diffuse light
 * only (no reflection is sampled; the specular colour takes that light through an analytic estimate, so
 * metals keep their tint), and clearcoat, sheen, iridescence and anisotropy are left out. A material with
 * transmission keeps the full model. Each lit material carries the "plain" shading part
 * (cartwheel/shading.h) on Very Low, so a game's program tables need those states: the survey
 * (tools/programs.mjs) runs every preset.
 *
 * Very Low and Low post: the height fog is applied inside the tone mapping pass instead of as its own blend
 * over the frame, and FXAA runs there too (FXAA 3.11's console variant: fewer reads, no edge search), its edge
 * test on a display brightness estimated from the HDR scene and its blend weighted so a bright light's
 * edge smooths as its display value would. On Low bloom's glow is composited in the same pass (gathered
 * from the scene before the fog). Two full-frame passes fewer; the picture differs from the separate passes
 * only at the edges FXAA smooths.
 *
 * The small HDR scene (Very Low and Low): the scene renders into R11G11B10 float (no alpha channel, less
 * precision in the darks than RGBA16F) where the device can render and blend it: GL with an R11F_G11F_B10F
 * framebuffer that is complete, WebGPU with the rg11b10ufloat-renderable feature (natively the engine asks for
 * it; in a cart the host's device decides). Elsewhere RGBA16F as above.
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
 * setting other than Resolution shows the preset as "Custom". Persist the
 * choice in the game's save data (cw_quality_saved / cw_quality_restore
 * below).
 *
 * Auto: the engine picks, and holds the frame's cost under a budget. While
 * the game says it is in play (cw_quality_auto_measure(true) every frame) it
 * averages the cost: the GPU time of the frame's passes where the host
 * times them cheaply (native WebGPU's timestamps), else the frame-to-frame
 * time on the real clock (GL, a cart: a frame the GPU cannot finish in time
 * arrives late, even where the host delivers a fixed step). It walks a
 * ladder of single changes, so a step never swaps a whole preset at once:
 *
 *   High; High with shadows Medium; Medium; Medium with ambient occlusion
 *   off; Low; Low with effects Very Low; Very Low
 *
 * (each rung's game levers are its preset's). After a couple of seconds over
 * budget it steps down one rung. On a big overshoot (the average of the
 * first second and a half at a rung over 1.6 x the budget) it jumps to the
 * rung expected to fit, so a weak device is at its level within a few
 * seconds of play instead of stuttering through every rung. Where it
 * measures GPU time it also steps back up when the cost has stayed well
 * under budget, never into a rung that was over it in this run. It never
 * changes Resolution: a game that calls cw_quality_full_resolution(false)
 * lets it go on below Very Low at 85%, 75%, 67%, 60% and 50%. A game makes
 * Auto the starting choice with cw_quality_auto(true, budget); the player
 * can always pick it. The rung it reached is saved, and the next run starts
 * one rung above it: that rung is saved only once it has held for a couple
 * of seconds of play, and a run where it goes over budget steps back down
 * and saves the old one. One bad session does not last for good. A game
 * that keeps the word itself writes it from the change callback too, so a
 * step is saved (the shell's save does this by itself).
 *
 * Determinism: a deterministic run (a test) starts at High whatever the
 * save holds, with Auto off, so every host draws the same frame; a choice
 * made in it (from the menus, by input every host replays alike) applies.
 * A test host with a real clock (tools/run-cart.mjs on Chromium and
 * wasmcart-native) asks for the same with the debug request `quality lock`.
 * Nothing here reaches the simulation. */
#ifndef CW_QUALITY_H
#define CW_QUALITY_H

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cheapest first, so q <= CW_QUALITY_LOW reads "Low or cheaper". */
typedef enum {
  CW_QUALITY_VERY_LOW,
  CW_QUALITY_LOW,
  CW_QUALITY_MEDIUM,
  CW_QUALITY_HIGH,
  CW_QUALITY_ULTRA,
  CW_QUALITY_COUNT
} cw_quality;

/* The preset now: the one picked, Auto's current rung's preset, or (Custom)
 * the preset the settings were last set from. */
cw_quality cw_quality_get(void);
/* Choose a preset (every setting but Resolution). user = true: the player
 * chose it (Auto stops); false: the game or a tool did (Auto, when on, may
 * still change it). Calls the change callback. */
void cw_quality_set(cw_quality q, bool user);
/* "Very Low", "Low", "Medium", "High", "Ultra" */
const char *cw_quality_name(cw_quality q);

/* ── the settings ──────────────────────────────────────────────────── */

typedef enum {
  CW_GFX_RESOLUTION, /* CW_GFX_RES_*: 50%, 60%, 67%, 75%, 85%, 100% of the output */
  CW_GFX_SSAO,       /* CW_GFX_SSAO_*: Off, On */
  CW_GFX_SHADOWS,    /* CW_GFX_SHADOWS_*: Minimal, Low, Medium, High, Ultra */
  CW_GFX_AA,         /* CW_GFX_AA_*: Off, FXAA, SMAA, Full */
  CW_GFX_EFFECTS,    /* a cw_quality: Very Low, Low, Medium, High, Ultra */
  CW_GFX_ENGINE_COUNT
} cw_gfx_setting;
/* Each setting's values by name (cw_gfx_get, cw_gfx_set, scope caps): a value's number can move when one is added. */
enum { CW_GFX_RES_50, CW_GFX_RES_60, CW_GFX_RES_67, CW_GFX_RES_75, CW_GFX_RES_85, CW_GFX_RES_100 };
enum { CW_GFX_SSAO_OFF, CW_GFX_SSAO_ON };
enum { CW_GFX_SHADOWS_MINIMAL, CW_GFX_SHADOWS_LOW, CW_GFX_SHADOWS_MEDIUM, CW_GFX_SHADOWS_HIGH, CW_GFX_SHADOWS_ULTRA };
enum { CW_GFX_AA_OFF, CW_GFX_AA_FXAA, CW_GFX_AA_SMAA, CW_GFX_AA_FULL };
enum { CW_GFX_MAX_LEVERS = 4, CW_GFX_LEVER_MAX_VALUES = 4 };

/* Settings shown: the engine's, then the game's levers (ids
 * CW_GFX_ENGINE_COUNT + i, in the order they were added). */
int cw_gfx_count(void);
const char *cw_gfx_name(int s);        /* "Resolution", "Ambient occlusion", ... or the lever's name */
int cw_gfx_value_count(int s);         /* how many values s has */
int cw_gfx_get(int s);                 /* its value now (an index) */
/* its value as a menu shows it: "75%  1440x810", "On", "SMAA", ... */
const char *cw_gfx_value_label(int s);
/* The player sets one (value clamped). Resolution changes nothing else; any
 * other setting turns the preset Custom unless the settings now match one.
 * Auto stops (Resolution leaves it on). */
void cw_gfx_set(int s, int value);
/* dir +1 / -1: the next / previous value (wrapping) */
void cw_gfx_step(int s, int dir);
/* The resolution setting's share of the output (0.5 .. 1), as drawn now (a scope may lower it). */
float cw_gfx_render_scale(void);

/* A setting as it is drawn now: cw_gfx_get lowered by the open scopes (below); the same for a game lever. */
int cw_gfx_effective(int s);

/* ── scopes: one scene asks for less ────────────────────────────────── */

/* A scene or mode that does not need everything (a 2D mini-game, a scene with no use for ambient occlusion) caps
 * settings for as long as it is on screen: caps[CW_GFX_RESOLUTION ... CW_GFX_EFFECTS] each hold the highest value
 * allowed by name (CW_GFX_SHADOWS_MINIMAL, CW_QUALITY_VERY_LOW, ...; -1 leaves that setting alone). A scope only
 * ever LOWERS what the player chose, so a player on Low stays on Low and the menu still shows the player's choice.
 * Scopes nest (four at most); the lowest cap wins. Render-only, like the rest of this header: call between frames.
 * Returns false when it is not opened. */
bool cw_quality_scope_push(const char *name, const int8_t caps[CW_GFX_ENGINE_COUNT]);
void cw_quality_scope_pop(void); /* closes the innermost scope */
int cw_quality_scope_count(void); /* how many are open */

/* The value a game lever takes on each preset (an index into its values, cheapest first). Passed by value, so a
 * call written for the four presets before Very Low existed fails to build instead of reading a shifted array. */
typedef struct {
  uint8_t very_low, low, medium, high, ultra;
} cw_quality_lever_presets;

/* A game's own render-only setting (at most CW_GFX_MAX_LEVERS, each with up
 * to CW_GFX_LEVER_MAX_VALUES values, cheapest first): its name, its values'
 * labels (kept by pointer: static strings) and the value each preset picks.
 * Call in init, before cw_quality_restore (the saved value applies to it).
 * Returns its setting id (for cw_gfx_get), -1 when full.
 *
 *   static const char *const EDGES[] = { "Off", "On" };
 *   lever = cw_quality_lever_add("Edge smoothing", EDGES, 2,
 *     (cw_quality_lever_presets){ .very_low = 0, .low = 0, .medium = 1, .high = 1, .ultra = 1 }); */
int cw_quality_lever_add(const char *name, const char *const *values, int value_count, cw_quality_lever_presets presets);

/* Keep the scene's MSAA (PostProcess msaa) on every anti-aliasing value but Off: for a game whose look needs it
 * (thin lines that FXAA and SMAA cannot hold). Off by default: FXAA and SMAA drop it. Render-only. */
void cw_quality_keep_msaa(bool on);

/* ── reading the settings ──────────────────────────────────────────── */

/* A scale for a named knob, from effects detail: "particles", "density",
 * "grass", "crowd", "scatter" (the last three are density), "lights",
 * "distortion", "flash_lights", "skin", "hrtf_voices" (the table above), 1
 * for any other name; "dispersion" is 0 (distortion is colourless). */
float cw_quality_scale(const char *knob);
float cw_quality_density(void); /* cw_quality_scale("density") */
/* n scaled by a knob, at least 1 when n > 0, at most n */
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
/* Full resolution (on by default): Auto never changes Resolution (no
 * preset ever does). false lets Auto go on below Very Low by lowering it:
 * Very Low at 85%, 75%, 67%, 60%, 50%. Call in init, before
 * cw_quality_restore. */
void cw_quality_full_resolution(bool on);
/* cw_quality_full_resolution is on */
bool cw_quality_is_full_resolution(void);
bool cw_quality_is_auto(void); /* Auto is choosing (not a test run) */
bool cw_quality_is_custom(void); /* the settings match no preset (and Auto is off) */
/* Auto's measured cost in ms (averaged; GPU time or frame time, as above),
 * 0 before it has measured */
float cw_quality_auto_cost(void);

/* ── a settings entry ──────────────────────────────────────────────── */

/* The next choice in Very Low, Low, Medium, High, Ultra, Auto, Very Low...
 * (a player's choice; from Custom, the preset after the one it came from).
 * Render-only: call it from update or event handling, never from the
 * fixed-step simulation. The frame's raw presses (cw_input_raw:
 * keys_pressed, pads[].pressed, mouse_pressed) hold for the whole frame, so
 * update sees them as the ticks do. */
void cw_quality_cycle(void);
/* the previous choice in that order */
void cw_quality_cycle_back(void);
/* "Very Low", "Low", "Medium", "High", "Ultra", "Auto" or "Custom": what a settings entry shows */
const char *cw_quality_label(void);
/* The render resolution as a graphics menu shows it: "<lines>p" of the
 * scene's render size (cw_render_size, the resolution setting applied),
 * e.g. "720p", "1080p". The output's size is the host's (RetroArch: Quick
 * Menu > Core Options > Internal Resolution). */
const char *cw_render_res_label(void);

/* Save data: a word the game keeps in its save (0 = nothing to keep). It holds
 * the choice (a preset, Auto and the rung it reached, also when Auto is the
 * game's own default and has stepped down, or every setting of a Custom
 * choice, the game's levers too) and the Resolution with it;
 * cw_quality_restore applies it (at init, after the save loaded and the
 * levers were added; not in a deterministic run). It takes 30 bits: keep
 * four bytes. A word written by an older engine (a byte 1..4, an Auto or a
 * Custom word) still restores the choice it held. */
uint32_t cw_quality_saved(void);
void cw_quality_restore(uint32_t saved); /* apply a cw_quality_saved word (0 = none chosen) */

#ifdef __cplusplus
}
#endif

#endif
