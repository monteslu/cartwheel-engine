/* Looks: post passes that restyle the 3D picture, two at a time, blended by weight.
 *
 * A look turns the scene into another kind of picture: glowing characters (ASCII), terminal half blocks (each
 * character cell a pair of square pixels, top and bottom), one-bit dithering with two colours (an ordered pattern or
 * an error-diffusion approximation), or a fragment shader of your own. Make looks once, then pick the pair a
 * pipeline draws and how far it is from the first to the second:
 *
 *   cw_look_desc d = cw_look_default(CW_LOOK_ASCII);
 *   d.ascii.rows = 40;
 *   cw_look ascii = cw_look_new(&d);
 *   cw_look_desc dd = cw_look_default(CW_LOOK_DITHER_ORDERED);
 *   cw_look dots = cw_look_new(&dd);
 *   cw_pipeline *p = cw_pipeline_of(tree);
 *   cw_pipeline_set_looks(p, ascii, (cw_look){ 0 }, 0, NULL);          // ASCII alone
 *   cw_pipeline_set_looks(p, ascii, dots, t, &(cw_look_mix){ .blend = CW_LOOK_WIPE, .softness = 0.1f }); // a wipe, t 0..1
 *   cw_pipeline_set_looks(p, (cw_look){ 0 }, (cw_look){ 0 }, 0, NULL); // the plain scene again
 *
 * A null look is the plain scene, so a fade from the scene into a look is (null, look, t). The pair is drawn by one
 * POST pass, "looks" (order 295: after automatic exposure has measured the scene, before bloom, so a look's bright
 * strokes glow), or with a look of the pair at CW_LOOK_STAGE_DISPLAY by "looks_display" (see "Stages"). Both looks
 * read the same scene colour: a blend costs the two looks and one blend draw, never a second scene. A blend weight of exactly 0 draws the first look alone and 1 the second alone, the same pixels as
 * that look set by itself. With no look set the pass does not run at all: the frame is the one drawn without looks,
 * at no cost.
 *
 * Colours: a look reads the scene as it will be exposed (the PostProcess exposure, exposure_ev and the automatic
 * exposure) and writes scene-linear colour divided by that exposure, so the tone curve and the bloom after it treat
 * a look's colours as emissive surfaces of those values seen at exposure 1: a dither's white (1, 1, 1) shows as the
 * tone curve's white of 1 whatever the scene's exposure, and a value over 1 blooms.
 *
 * Stages (cw_look_desc.stage): SCENE (the default) draws a look before bloom, the tone curve, the grade, anti-aliasing
 * and the lens, in the colours above, so bloom glows on its strokes and the tone curve shapes them. DISPLAY draws it
 * last, in the POST pass "looks_display" (order 9900, after the lens and every POST pass of the game's own before it,
 * before the HUD), on the finished picture: what the look writes is what the screen shows, so every pixel of a dither
 * look there is its dark or its light colour (each shown as that linear colour is on screen, sRGB) and nothing after
 * it (bloom, grain, the vignette, the tone curve) adds a third. A look at DISPLAY reads the finished picture in
 * linear light (0..1, no exposure: the tone curve has done it) and its colours are clipped to 0..1 (they do not
 * bloom); a dither's brightness there is the luminance of the displayed colour itself, not the display-like curve.
 * Each look of a pair is drawn at its own stage; the plain scene of a null look is the picture at the other look's
 * stage. A blend between looks of two stages runs at both: each stage blends its own look with the picture it gets
 * by the pair's weight and mask (so at the DISPLAY stage the picture under the dither is the finished picture of the
 * SCENE look's share), and the blend starts on the first look's own picture and ends on the second's, with no step
 * when the pair is replaced by the end look alone. HUD boxes and the
 * brightness measurement work the same at both stages (measured in linear light at DISPLAY).
 *
 * Grids (characters, half blocks, dither pixels) are anchored to the pixels of each view, from its top left: a still
 * camera gives a still picture, frame after frame (nothing crawls), and every pattern is a function of the frame
 * alone except the holds: the ASCII glyph hold, which keeps last frame's character where a new one fits only a little
 * better, and the half blocks' quantising hold, which keeps last frame's palette colour or level where the new one
 * wins only by a little (a mean on a boundary, under TAA's jitter or grain, holds still).
 * Sizes are in pixels of a 1080 line picture (ASCII and half block rows over the output's height), scaled with the
 * output: the same look keeps its proportions at every resolution and in every split screen view.
 *
 * Split screen: the pass runs in each view, inside its rectangle, and in the one-pass split screen too
 * (cw_pipeline_set_views_array; it reads the view's depth, so with the shared post chain it is one of the per-view
 * passes). Each view keeps its own glyph history.
 *
 * HUD boxes: rectangles of the output (a game's HUD panels) where the looks are dimmed, or left out so the plain scene
 * shows, so text drawn over them reads (cw_pipeline_set_look_boxes). The HUD itself draws after the post chain
 * (OVERLAY), untouched by any look.
 *
 * Looks are art direction, not quality: every graphics preset draws them the same (Auto quality never changes one).
 * They run on GL and WebGPU alike. */
#ifndef CW_LOOK_H
#define CW_LOOK_H

#include "pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

CW_HANDLE_TYPE(cw_look);

typedef enum {
  CW_LOOK_NONE,            /* the plain scene (as a null look) */
  CW_LOOK_ASCII,           /* glowing characters matched to the shapes in each cell */
  CW_LOOK_HALFBLOCK,       /* half block cells: a top and a bottom colour per character cell */
  CW_LOOK_DITHER_ORDERED,  /* one bit a pixel against a threshold pattern (8x8 Bayer or blue noise) */
  CW_LOOK_DITHER_DIFFUSE,  /* one bit a pixel, the error spread over the neighbours (see cw_look_dither) */
  CW_LOOK_CUSTOM,          /* a fragment shader pair of your own (cw_look_custom) */
  CW_LOOK_KIND_COUNT
} cw_look_kind;

/* ASCII: each character cell of the view is measured in six regions (3 rows x 2 columns, plus the depth edges on its
 * near side) and drawn as the character of `glyphs` whose shape fits best, or, when the cell is flat, as the
 * character of `ramp` for its brightness. The character takes the scene's own hue pushed toward neon (`saturation`);
 * cells with little hue of their own take `tint`. A soft halo of each bright character's colour (`glow`) lies under
 * it; `neon` over 1 makes the strokes bloom. Only printable ASCII (32 to 126) can be drawn. */
typedef struct cw_look_ascii {
  int rows;                /* character rows over the output's height (54); a cell is as wide as the font's advance */
  const char *glyphs;      /* the characters shape matching may pick (NULL: lines, corners, brackets and marks) */
  const char *ramp;        /* the characters flat cells take by brightness, darkest first (NULL: " .:-=+*#%@") */
  const char *font;        /* a TrueType / OpenType asset path (NULL: the built-in Fira Mono) */
  float neon;              /* the characters' brightness (scene-linear at exposure 1; over 1 blooms): 1.6 */
  float saturation;        /* the push toward the cell's own hue: 1.8 */
  float tint[3];           /* the colour of cells without a hue of their own (grey and white): (0.2, 1, 0.45) */
  float glow;              /* the halo's strength under bright characters (0: none): 0.5 */
  float glow_from;         /* the brightness a character starts to glow at (0..1): 0.5 */
  float hold;              /* a cell keeps last frame's character unless a new one fits this many times better (1: no
                            * hold): 1.35. Holds a still picture's characters against noise, TAA jitter and grain */
  float exposure;          /* the scene's gain before measuring: 1 */
  float floor;             /* cells darker than this stay blank (0..1): 0.05 */
  float contrast;          /* sharpens the six regions before matching: 2 */
  float edges;             /* depth edges drawn as strokes (0: brightness alone): 1.5 */
  float flat;              /* a cell whose darkest region is within this share of its brightest is flat: 0.45 */
  float ramp_gain;         /* brightness to ramp position: 1 */
  float flat_bright;       /* the brightness of flat cells: 0.75 */
  float dim_bright;        /* the brightness of cells without a hue (grey world): 0.3 */
  float bright_floor;      /* the dimmest a drawn character gets: 0.35 */
  float peak;              /* 0..1 between a region's mean and its brightest read (thin bright lines keep their light): 0.5 */
} cw_look_ascii;

/* HALFBLOCK: character cells `rows` to the output's height, each a pair of square pixels (the upper half block
 * character's top and bottom): each pixel is the mean of the scene over its square. Optionally quantised: `levels`
 * per channel, or the nearest colour of a palette; a pixel keeps last frame's colour while it stays within 0.03 display
 * steps of the nearest (a level: 0.15 of a step past the middle), so a still picture holds still at either stage. */
typedef struct cw_look_halfblock {
  int rows;                /* character rows over the output's height (54): 108 pixels down */
  int levels;              /* per channel, in display steps (0: none; 6 is a 216 colour cube) */
  const float *palette;    /* linear RGB triples, nearest in display steps (NULL: none; copied); wins over levels */
  int palette_count;       /* up to CW_LOOK_MAX_PALETTE */
} cw_look_halfblock;
#define CW_LOOK_MAX_PALETTE 64

typedef enum {
  CW_DITHER_BAYER,         /* the 8x8 Bayer matrix (the classic cross hatch) */
  CW_DITHER_BLUE_NOISE,    /* a 64x64 blue noise tile (no visible pattern) */
} cw_dither_pattern;

/* DITHER (both kinds): every dither pixel is one of two colours. Its brightness is the scene's luminance over its
 * square, through a display-like curve (scene 0.18 is about 0.42; at the DISPLAY stage the displayed colour's own
 * luminance), then `contrast` and `bias`.
 * The tone options (all off in cw_look_default, which keeps the brightness above exactly) shape it further, in this
 * order: saturated colours by their value (`hue_value`), grey by its luminance times a gain with the highlights kept
 * (`grey_gain`), an unsharp mask (`sharpen`), `gamma`, then `contrast` and `bias`, then outlines from the depth
 * (`outline`) taken as the brighter of the two. A setting left at 0 is its default (as cw_look_default sets it), so
 * a desc of zeros is today's dither. A bright, crisp one-bit picture from a game's finished frame:
 *
 *   cw_look_desc d = cw_look_default(CW_LOOK_DITHER_ORDERED);
 *   d.stage = CW_LOOK_STAGE_DISPLAY;
 *   d.dither.pattern = CW_DITHER_BLUE_NOISE;
 *   d.dither.pixel = 1, d.dither.output_pixels = true;     // one dither pixel per screen pixel at any resolution
 *   d.dither.hue_value = 1.15f, d.dither.grey_gain = 0.8f;  // neon hues read bright, the grey world a little darker
 *   d.dither.sharpen = 0.9f, d.dither.outline = 1;          // crisp edges, silhouettes and creases drawn light
 *   d.dither.gamma = 1.3f, d.dither.contrast = 1.3f;
 *
 * ORDERED compares it with `pattern` at the pixel.
 * DIFFUSE approximates error diffusion in a way a GPU does in parallel and that holds still: it starts from the blue
 * noise threshold and makes `sweeps` passes of a direct binary search (each pixel turns on or off when that brings
 * the picture, blurred as an eye sees it, closer to the scene; in four interleaved phases). Error never travels along
 * a scan line as in serial error diffusion, so a change in one place does not shuffle the dots across the screen. */
typedef struct cw_look_dither {
  float pixel;             /* a dither pixel's size in pixels of a 1080 line picture (at least one pixel): 2 */
  float dark[3];           /* the colour of the off pixels (scene-linear, at exposure 1): black */
  float light[3];          /* the colour of the on pixels: white (1, 1, 1) */
  cw_dither_pattern pattern; /* ORDERED: the threshold pattern; BAYER */
  float contrast;          /* about the middle: 1 */
  float bias;              /* added to the brightness (-1..1): 0 */
  int sweeps;              /* DIFFUSE: refinement sweeps (0..8; each is four passes over the dither pixels): 2 */
  /* pixel counts pixels of the output, not of a 1080 line picture: 1 is one dither pixel a screen pixel at every
   * resolution (and in every split screen view): false */
  bool output_pixels;
  /* the tone options (see above). The colours they read are the scene's through the display-like curve (at DISPLAY
   * the displayed colour), each channel 0..1, over the dither pixel's square */
  float hue_value;         /* a saturated colour's brightness is its value (brightest channel) times this (0: off): 0 */
  float hue_from, hue_to;  /* the saturation ((max - min) / max) over which a colour goes from grey to hue: 0.22, 0.5 */
  float grey_gain;         /* the luminance times this (1: as it is): 1. Highlights keep their own brightness: */
  float keep_from, keep_to; /* the gain fades out over this luminance (a blast's white core stays white): 0.6, 0.95 */
  float sharpen;           /* the unsharp mask's gain (0: off): 0. The blur it sharpens against is four dither pixels */
  float sharpen_radius;    /* `sharpen_radius` straight out (each 0.15) and four diagonal at half of it (0.1): 2 */
  float outline;           /* the brightness of depth outlines (0: off): 0. An outline is where 1 / linear depth */
  float outline_from, outline_to; /* bends (its Laplacian over itself) from this to this, smoothly: 0.02, 0.08 */
  float gamma;             /* the brightness to this power before the contrast (1: none): 1 */
} cw_look_dither;

/* CUSTOM: your look as a GLSL ES 3.00 function and its WGSL twin (a cart runs whichever backend the host gives it, so
 * give both). Write
 *   GLSL: vec3 look(vec2 px)          WGSL: fn look(px: vec2f) -> vec3f
 * returning the colour at view pixel px (from the view's top left, pixel centres at .5), in the exposed space the
 * look reads (see "Colours" above; at the DISPLAY stage linear light, shown as it is). The engine defines, in both
 * languages:
 *   cw_look_size()        the view's size in pixels (vec2)
 *   cw_look_scene(px)     the exposed scene colour at view pixel px (bilinear), vec3; at DISPLAY the finished picture
 *   cw_look_scale()       pixels per pixel of a 1080 line picture (float)
 *   cw_look_param(i)      params[4 i .. 4 i + 3] as a vec4 (i 0..3)
 *   cw_look_time()        cw_time seconds (float); the frame index is cw_look_frame()
 *   cw_look_stage()       0 at the SCENE stage, 1 at DISPLAY (float)
 *   cw_look_depth(px)     the depth at view pixel px as the depth buffer holds it (0 near .. 1 far; 1 where nothing
 *                         was drawn), the same on GL and WebGPU (float)
 *   cw_look_linear_depth(px) the view-space distance there (float; over 1e30 where nothing was drawn)
 *   cw_look_bayer(px)     the 8x8 Bayer threshold at view pixel px (0..1, as the ORDERED dither's) (float)
 *   cw_look_blue_noise(px) the 64x64 blue noise threshold there (0..1, as BLUE_NOISE's), anchored to the view (float)
 *   cw_look_to_display(c), cw_look_from_display(d) linear light to sRGB display values and back (vec3, clipped 0..1)
 * HUD boxes are applied after it. */
typedef struct cw_look_custom {
  const char *glsl;        /* copied */
  const char *wgsl;        /* copied */
  float params[16];
} cw_look_custom;

/* Where in the post chain a look is drawn (see "Stages" above). */
typedef enum {
  CW_LOOK_STAGE_SCENE,     /* before bloom and the tone curve (order 295) */
  CW_LOOK_STAGE_DISPLAY,   /* last, on the finished picture (order 9900): what the look writes is on screen */
} cw_look_stage;

typedef struct cw_look_desc {
  cw_look_kind kind;
  /* the kind's own settings (only that member is read; start from cw_look_default(kind)) */
  cw_look_ascii ascii;
  cw_look_halfblock halfblock;
  cw_look_dither dither;   /* both dither kinds */
  cw_look_custom custom;
  cw_look_stage stage;     /* SCENE */
} cw_look_desc;

/* A desc of that kind with every setting at the default noted above. */
cw_look_desc cw_look_default(cw_look_kind kind);
/* Make a look (programs and glyph atlases are made on the first frame that draws it). A null look on a bad desc (a
 * CUSTOM without the backend's shader, an unknown kind; logged). Looks belong to no pipeline: any pipeline (a
 * SubViewport's too) can draw any look. Needs the GPU (init or later). */
cw_look cw_look_new(const cw_look_desc *d);
/* Change a look's settings (its kind too) from the next frame: a beat pulsing the glow, the rows growing. false on a
 * bad desc (the look keeps its old settings) or a dead handle. */
bool cw_look_set(cw_look l, const cw_look_desc *d);
/* The look's settings now (false for a dead handle). */
bool cw_look_get(cw_look l, cw_look_desc *out);
/* Free a look; pipelines drawing it draw the plain scene in its place. */
void cw_look_free(cw_look l);

/* How the second look comes in. */
typedef enum {
  CW_LOOK_FADE,            /* a cross fade: every pixel mix(a, b, t) */
  CW_LOOK_DISSOLVE,        /* each pixel turns over when a smooth noise mask passes it (anchored to the view's pixels) */
  CW_LOOK_WIPE,            /* a straight edge sweeps across the view along `angle` */
} cw_look_blend;

typedef struct cw_look_mix {
  cw_look_blend blend;     /* FADE */
  float softness;          /* DISSOLVE / WIPE: the edge's width, a share of the sweep (0: hard): 0 */
  float angle;             /* WIPE: the direction the edge moves, radians (0: left to right, pi/2: bottom to top) */
  float scale;             /* DISSOLVE: the noise's grain, pixels of a 1080 line picture (0: 24) */
  uint32_t seed;           /* DISSOLVE: another noise */
} cw_look_mix;

/* The looks this pipeline draws from the next frame: a, b, and t from a (0) to b (1), blended as `how` says (NULL: a
 * cross fade). A null handle is the plain scene; a and b both null (or the same look and t of 0) draw no look and run
 * nothing. t is clamped to 0..1. A blend is the game's to time: a fade over half a second or more swells rather than
 * flashes (a full-screen change of brightness back and forth more than three times a second is a strobe). */
void cw_pipeline_set_looks(cw_pipeline *p, cw_look a, cw_look b, float t, const cw_look_mix *how);
/* What cw_pipeline_set_looks set (any pointer may be NULL). */
void cw_pipeline_looks(cw_pipeline *p, cw_look *a, cw_look *b, float *t, cw_look_mix *how);

/* A HUD box: a rectangle of the whole output (fractions, x / y from the top left, as cw_view's), where the looks are
 * drawn darker by `dim` (0..1: 1 is black), or, with `clean`, not drawn at all (the plain scene shows). ASCII dims
 * every character cell the box touches. */
typedef struct cw_look_box {
  float x, y, w, h;
  float dim;
  bool clean;
} cw_look_box;
#define CW_LOOK_MAX_BOXES 16
/* The boxes from the next frame (copied; n 0 clears them, past CW_LOOK_MAX_BOXES are dropped). A game sets them every
 * frame its HUD moves; they stay until set again. */
void cw_pipeline_set_look_boxes(cw_pipeline *p, const cw_look_box *boxes, int n);

/* Measure the looks' brightness: each frame the pass also reduces each look's output to its mean luminance (in the
 * exposed space, the first view's) on the GPU and reads it back a frame later. Off by default; on, it costs four tiny
 * draws a look and a 1x1 read a frame (on GL a short wait for the previous frame's GPU work). */
void cw_pipeline_measure_looks(cw_pipeline *p, bool on);
/* The latest measured mean luminance of the first and the second look's output (the plain scene's for a null look),
 * as of a frame or two ago: out[i] is -1 for a look that was not drawn then (b while t is 0). false while nothing
 * has arrived. A game matches two looks' brightness across a blend with it (ASCII is darker than the scene). */
bool cw_pipeline_look_luminance(cw_pipeline *p, float out[2]);

#ifdef __cplusplus
}
#endif
#endif
