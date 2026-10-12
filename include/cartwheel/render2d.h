/* cartwheel 2D renderer: a batched, immediate-mode-style submission API.
 *
 * Every frame, between cw_r2d_begin() and cw_r2d_end(), the game submits
 * sprites, shapes, text, tilemaps and lights into canvas LAYERS. Nothing is
 * drawn while submitting: cw_r2d_end() sorts what was submitted (layer, then
 * z, then y when the layer y-sorts, then submission order; the sort is
 * stable), cuts it into batches and draws. A batch only ends where the GL
 * state has to change: a different texture ARRAY, blend mode, clip rect or
 * shader (tilemaps, lit layers). Every texture lives in a GL_TEXTURE_2D_ARRAY
 * page, so sprites from many atlas pages share one draw, and solid shapes,
 * SDF text and additive sprites batch with textured sprites (the fragment
 * shader branches on a per-vertex mode). A typical frame is a handful of
 * draws.
 *
 * Coordinates: canvas pixels, origin top-left, y down. Angles in radians,
 * positive = clockwise on screen. Colours are straight (not premultiplied)
 * 0..1 floats; the renderer premultiplies internally. All textures are stored
 * premultiplied, and blending is premultiplied-alpha throughout.
 *
 * GL: OpenGL ES 3.0 / WebGL2 through cartwheel/gl.h, the same code natively
 * and in carts. The renderer needs a current context (call from init()
 * onward) and owns no context state between passes: each cw_r2d_end() sets
 * everything it uses (framebuffer, viewport, program, VAO, blend, scissor,
 * stencil, depth and cull off, texture unit 0..2) and leaves those changed.
 * Sharing the context with three.c: call t3_renderer_reset_bindings(r)
 * after any 2D pass and before the next t3_renderer_render(), so three.c's
 * GL state cache matches GL again (the 2D renderer never touches three.c's
 * programs, so the cheaper reset is enough). Draw three.c INTO a 2D render
 * target with t3_renderer_set_framebuffer(r, cw_target_fbo(rt)) plus
 * t3_renderer_set_size(r, w, h); the target then draws as a sprite.
 *
 * Costs: submission is O(vertices) on the calling thread (cw_r2d_sprites
 * spreads big arrays over the job pool); cw_r2d_end() is one radix sort when
 * the submission is not already in order, one upload per stream (vertices,
 * indices; orphaned each pass, a ring of 3 buffers), and one draw call per
 * batch. Memory is tagged CW_MEM_RENDER2D.
 *
 * Threads: call everything from the main (GL) thread. */
#ifndef CW_RENDER2D_H
#define CW_RENDER2D_H

#include "base.h"
#include "math.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── value types ─────────────────────────────────────────────────────── */

typedef struct cw_color { float r, g, b, a; } cw_color; /* straight alpha, 0..1 */
/* straight-alpha colour, components 0..1 */
static inline cw_color cw_rgba(float r, float g, float b, float a) { return (cw_color){ r, g, b, a }; }
/* 0xRRGGBBAA */
static inline cw_color cw_hex(uint32_t c) {
  return (cw_color){ (float)(c >> 24) / 255.0f, (float)((c >> 16) & 255) / 255.0f, (float)((c >> 8) & 255) / 255.0f,
                     (float)(c & 255) / 255.0f };
}
#define CW_WHITE ((cw_color){ 1, 1, 1, 1 })
#define CW_BLACK ((cw_color){ 0, 0, 0, 1 })

typedef struct cw_rect { float x, y, w, h; } cw_rect;
/* x, y = top-left; w, h in the same units */
static inline cw_rect cw_rect_make(float x, float y, float w, float h) { return (cw_rect){ x, y, w, h }; }

/* 2D affine transform: x' = a*x + c*y + tx, y' = b*x + d*y + ty. */
typedef struct cw_xform2d { float a, b, c, d, tx, ty; } cw_xform2d;
/* a = d = 1, the rest 0 */
static inline cw_xform2d cw_xform2d_identity(void) { return (cw_xform2d){ 1, 0, 0, 1, 0, 0 }; }
/* transform a point (translation included) */
static inline cw_vec2 cw_xform2d_apply(const cw_xform2d *m, cw_vec2 p) {
  return (cw_vec2){ m->a * p.x + m->c * p.y + m->tx, m->b * p.x + m->d * p.y + m->ty };
}
/* m * n: apply n first, then m. */
cw_xform2d cw_xform2d_mul(const cw_xform2d *m, const cw_xform2d *n);
cw_xform2d cw_xform2d_invert(const cw_xform2d *m); /* identity when m is singular (determinant 0) */
/* translate(pos) * rotate(rot) * scale(scale): the usual node transform */
cw_xform2d cw_xform2d_trs(cw_vec2 pos, float rot, cw_vec2 scale);

/* ── module ──────────────────────────────────────────────────────────── */

/* Compiles the shaders and creates the streams. Every other call initializes
 * lazily, so calling this is optional; it returns false (and logs why) when
 * a shader fails to build. */
bool cw_r2d_init(void);
/* Frees every texture, font, tilemap and GL object the renderer made. */
void cw_r2d_shutdown(void);

/* ── textures ────────────────────────────────────────────────────────────
 * A cw_texture is a rectangle inside a GL_TEXTURE_2D_ARRAY. Loose images are
 * packed at runtime into shared 2048x2048 atlas pages (one array per filter
 * mode, grown a page at a time), with a 1-pixel extruded border so linear
 * filtering never bleeds a neighbour in. Draws of anything in the same array
 * batch together. Standalone textures (repeat wrap, images bigger than a
 * page, render targets) get an array of their own. */
CW_HANDLE_TYPE(cw_texture);

typedef enum { CW_FILTER_LINEAR, CW_FILTER_NEAREST } cw_filter;

typedef struct cw_texture_desc {
  cw_filter filter;   /* NEAREST for pixel art */
  bool standalone;    /* own array instead of an atlas page */
  bool repeat;        /* GL_REPEAT wrap (implies standalone): UVs past 1 tile */
  bool premultiplied; /* pixels are already premultiplied: skip conversion */
} cw_texture_desc;

/* PNG / JPEG from the asset bundle (cw_asset_load), decoded by stb_image to
 * RGBA8 and premultiplied. desc may be NULL (linear, atlas). A missing or
 * undecodable file logs the path and returns the null handle; drawing a null
 * handle draws a solid colour. */
cw_texture cw_texture_load(const char *asset_path, const cw_texture_desc *desc);
/* PNG / JPEG bytes (copied; caller keeps data); null handle (logged) when undecodable */
cw_texture cw_texture_from_memory(const void *data, size_t len, const cw_texture_desc *desc);
/* w*h RGBA8, row 0 = top, straight alpha unless desc->premultiplied. Copied. */
cw_texture cw_texture_from_pixels(int w, int h, const uint8_t *rgba, const cw_texture_desc *desc);
/* Replace the pixels (same size and alpha convention as creation). */
void cw_texture_update(cw_texture t, const uint8_t *rgba);
/* Attach a tangent-space normal map (w*h RGBA8 matching the texture's size,
 * OpenGL convention: +y up in the image is green). Lit layers shade sprites
 * of this texture with it; without one a sprite is lit flat. Atlas textures
 * only. Returns false (logged) on a size mismatch. */
bool cw_texture_set_normal_map(cw_texture t, const uint8_t *rgba);
/* as cw_texture_set_normal_map from a PNG asset; must decode to the texture's size */
bool cw_texture_load_normal_map(cw_texture t, const char *asset_path);
/* pixels; 0 x 0 for a null or destroyed handle; w or h may be NULL */
void cw_texture_size(cw_texture t, int *w, int *h);
bool cw_texture_valid(cw_texture t); /* false for a null or destroyed handle */
/* Atlas space is not reclaimed (pages only grow); standalone arrays and
 * targets are freed. */
void cw_texture_destroy(cw_texture t);

/* ── render targets ──────────────────────────────────────────────────────
 * A target is a standalone texture you can draw into (cw_r2d_begin with
 * .target) and then draw as a sprite. Content follows GL's orientation
 * (row 0 = bottom); the renderer flips it back when sampling, so a target
 * drawn as a sprite shows what was drawn into it upright, whether a 2D pass
 * or three.c filled it. */
typedef struct cw_target_desc {
  int width, height;
  cw_filter filter;
  bool depth; /* depth24 + stencil8 renderbuffer (three.c needs one) */
} cw_target_desc;
/* Create a render target (free with cw_texture_destroy). Null handle (logged) for a non-positive size or no GL
 * context. */
cw_texture cw_target_create(const cw_target_desc *desc);
/* The framebuffer object, for t3_renderer_set_framebuffer. */
unsigned cw_target_fbo(cw_texture t);
/* Reallocates (contents lost). */
void cw_target_resize(cw_texture t, int width, int height);

/* ── cameras ─────────────────────────────────────────────────────────────
 * A plain value the game owns. The view maps world -> canvas pixels:
 *   screen = anchor + offset + R(-rotation) * ((world - position*parallax) * zoom)
 * where anchor is the viewport's centre (or its top-left corner with
 * anchor_top_left). Zero-initialized fields mean zoom 1, no rotation. */
typedef struct cw_camera2d {
  cw_vec2 position;     /* world point shown at the anchor */
  float zoom;           /* >1 magnifies; 0 = 1 */
  float rotation;       /* radians: the WORLD appears rotated by -rotation */
  cw_vec2 offset;       /* extra canvas-pixel offset of the anchor (shake) */
  bool anchor_top_left; /* position sits at the viewport's top-left, not its centre */
  bool use_limits;      /* keep the view inside `limits` (world rect) */
  cw_rect limits;
} cw_camera2d;

/* World -> canvas transform for a viewport (canvas pixels) and a parallax
 * factor ((0,0) = (1,1); (0.5, 0.5) scrolls at half speed). */
cw_xform2d cw_camera2d_view(const cw_camera2d *cam, cw_rect viewport, cw_vec2 parallax);
/* world -> canvas pixels (parallax 1) */
cw_vec2 cw_camera2d_world_to_screen(const cw_camera2d *cam, cw_rect viewport, cw_vec2 world);
/* canvas pixels -> world (parallax 1) */
cw_vec2 cw_camera2d_screen_to_world(const cw_camera2d *cam, cw_rect viewport, cw_vec2 screen);
/* Exponential smoothing toward target: position moves 1 - e^(-speed*dt) of
 * the way per call, frame-rate independent and deterministic (cw_exp).
 * speed <= 0 snaps. Call cw_camera2d_apply_limits after it to keep limits. */
void cw_camera2d_follow(cw_camera2d *cam, cw_vec2 target, float speed, float dt);
/* Clamp position so the view (of viewport size) stays inside limits. */
void cw_camera2d_apply_limits(cw_camera2d *cam, cw_rect viewport);

/* ── passes ──────────────────────────────────────────────────────────────
 * One pass draws into one target. Passes do not nest: finish a target's pass
 * before drawing it in another. */
typedef enum {
  CW_SCALE_NONE,    /* canvas = the target's pixels */
  CW_SCALE_INTEGER, /* virtual canvas, scaled by the largest whole factor that fits, centred (pixel-perfect) */
  CW_SCALE_FIT,     /* virtual canvas, scaled to fit keeping aspect, centred */
  CW_SCALE_STRETCH, /* virtual canvas stretched over the whole window */
} cw_scale_mode;

typedef struct cw_r2d_pass_desc {
  cw_texture target;    /* null = the window (framebuffer 0) */
  bool clear;
  cw_color clear_color;
  /* window passes: draw at virtual_width x virtual_height (nearest-filtered
   * canvas) and scale it to the window by `scale`; 0 = no virtual canvas */
  int virtual_width, virtual_height;
  cw_scale_mode scale;
  cw_color letterbox;   /* bars around a scaled canvas (alpha forced to 1) */
  float time;           /* seconds: animated tiles (pass a tick-derived time for replays) */
  cw_color ambient;     /* lit layers: light where no light reaches; all 0 = black */
} cw_r2d_pass_desc;

/* Start a pass (desc NULL = window, no clear). Resets layer, z, blend, transform and clips; layer descs persist
 * across passes. Asserts when a pass is open; an invalid .target logs and falls back to the window. */
void cw_r2d_begin(const cw_r2d_pass_desc *desc);
/* Sorts, batches and draws everything submitted since cw_r2d_begin. */
void cw_r2d_end(void);
/* Canvas size of the current pass: the virtual size, the target's size, or
 * for a plain window pass the window's LOGICAL size (cw_window_size), which
 * the pass scales to the drawable (cw_app_desc.render_scale, display
 * density): draw in logical units, it rasterizes at full resolution. */
cw_vec2 cw_r2d_canvas_size(void);
/* Window coordinates (logical, as the platform reports the pointer) ->
 * canvas coordinates for the last window pass (undoes virtual canvas
 * scaling and letterboxing; identity for a plain window pass). Valid after a
 * window pass began; a render-target pass since then does not change the
 * answer. */
cw_vec2 cw_r2d_window_to_canvas(cw_vec2 window_px);
cw_vec2 cw_r2d_canvas_to_window(cw_vec2 canvas_px); /* inverse of cw_r2d_window_to_canvas */
/* Where the scaled canvas lands in the window, in drawable pixels. */
cw_rect cw_r2d_canvas_rect(void);
/* Drawable pixels per canvas unit of the current pass (1 for render targets
 * and virtual canvases; the pixel ratio for a plain window pass). */
float cw_r2d_pixel_scale(void);

/* ── canvas layers ───────────────────────────────────────────────────────
 * CW_R2D_MAX_LAYERS layers, drawn in index order (0 first). Each has its own
 * camera, so a world layer and a HUD layer coexist; settings persist across
 * passes and frames (a scene tree's canvas draw puts back those it used).
 * Layer 0 with no camera is the default: screen space. */
#define CW_R2D_MAX_LAYERS 32

typedef struct cw_r2d_layer_desc {
  bool has_camera;      /* false = screen space (identity) */
  cw_camera2d camera;
  cw_vec2 parallax;     /* camera position factor; (0,0) = (1,1) */
  cw_rect viewport;     /* canvas pixels; w == 0 = the whole canvas. Draws are clipped to it. */
  bool y_sort;          /* within equal z: sort by y (sprite pivot / shape anchor), lower first */
  bool pixel_snap;      /* round transformed vertices to whole canvas pixels */
  bool lit;             /* drawn through the 2D light pass (see lights) */
} cw_r2d_layer_desc;

/* layer 0..CW_R2D_MAX_LAYERS-1 (asserts); NULL resets it; persists until changed */
void cw_r2d_layer_set(int layer, const cw_r2d_layer_desc *desc);
const cw_r2d_layer_desc *cw_r2d_layer_get(int layer); /* pointer into the renderer's table; asserts on a bad index */
/* Update only the camera (has_camera becomes true). */
void cw_r2d_layer_set_camera(int layer, const cw_camera2d *cam);
/* Subsequent submissions go into this layer (default 0). */
void cw_r2d_use_layer(int layer);
/* The layer's world <-> canvas mapping in the current (or last) pass. */
cw_vec2 cw_r2d_layer_screen_to_world(int layer, cw_vec2 canvas_px);
cw_vec2 cw_r2d_layer_world_to_screen(int layer, cw_vec2 world); /* world -> canvas pixels */

/* ── draw state (applies to submissions that follow, reset by begin) ──── */
typedef enum {
  CW_BLEND_ALPHA,    /* premultiplied over */
  CW_BLEND_ADD,      /* additive; batches with ALPHA (no state change) */
  CW_BLEND_MULTIPLY, /* dst * src (own batch) */
  CW_BLEND_OPAQUE,   /* replace (own batch) */
} cw_blend;
void cw_r2d_set_blend(cw_blend b); /* for later submissions until the pass ends (cw_r2d_begin resets to ALPHA) */
/* z within a layer (higher draws later); sprites add their own z. -32768..32767 */
void cw_r2d_set_z(int z);
/* Local transform applied before the layer camera to everything submitted
 * (sprites, shapes, text): what the node layer pushes. NULL = identity. */
void cw_r2d_set_transform(const cw_xform2d *m);
/* Clip to a canvas-pixel rect (intersected with the enclosing clip). Max
 * depth 32; up to 254 distinct clips per pass. */
void cw_r2d_push_clip(cw_rect canvas_rect);
void cw_r2d_pop_clip(void); /* asserts without a matching push */

/* ── sprites ─────────────────────────────────────────────────────────── */
typedef struct cw_sprite {
  cw_texture texture; /* null = solid colour rectangle */
  cw_rect src;        /* texture pixels (atlas region); w == 0 = whole texture */
  cw_vec2 pos;        /* where the pivot lands (layer space) */
  cw_vec2 size;       /* drawn size; 0 = src size */
  cw_vec2 scale;      /* (0,0) = (1,1) */
  cw_vec2 pivot;      /* 0..1 inside the sprite, (0,0) = top-left, (0.5,0.5) = centre */
  float rotation;     /* radians clockwise, about the pivot */
  cw_color color;     /* modulate; {0,0,0,0} (zero-init) = white */
  bool flip_x, flip_y;
  int z;              /* added to cw_r2d_set_z */
} cw_sprite;

/* A null texture draws a solid rect of the sprite's size and colour. Cost: one quad appended. */
void cw_r2d_sprite(const cw_sprite *s);
/* n sprites at once; big arrays fill vertices in parallel on the job pool.
 * Same result as n cw_r2d_sprite calls. */
void cw_r2d_sprites(const cw_sprite *s, int n);

/* Nine-slice: the corners keep their size, edges stretch one way, the centre
 * both. Margins are texture pixels; `margin_scale` scales them on screen
 * (0 = 1). */
typedef struct cw_nine_slice {
  cw_texture texture;
  cw_rect src;                    /* w == 0 = whole texture */
  float left, top, right, bottom; /* margins, texture pixels */
  cw_rect dst;
  cw_color color;                 /* {0,0,0,0} = white */
  float margin_scale;
  bool hollow;                    /* skip the centre */
} cw_nine_slice;
void cw_r2d_nine_slice(const cw_nine_slice *n); /* margins shrink to fit dst; an invalid texture draws a solid rect */

/* ── shapes (solid colour; batch with sprites) ──────────────────────── */
void cw_r2d_rect(cw_rect r, cw_color c);
void cw_r2d_rect_lines(cw_rect r, float width, cw_color c); /* stroke inside the rect */
/* width in canvas pixels; zero-length lines draw nothing */
void cw_r2d_line(cw_vec2 a, cw_vec2 b, float width, cw_color c);
/* Mitred joins (bevelled past a 4x miter), butt caps; closed joins the ends. */
void cw_r2d_polyline(const cw_vec2 *pts, int n, float width, bool closed, cw_color c);
/* segments <= 0 picks by radius */
void cw_r2d_circle(cw_vec2 center, float radius, int segments, cw_color c);
/* ring of the given width centred on radius */
void cw_r2d_circle_lines(cw_vec2 center, float radius, float width, int segments, cw_color c);
/* Simple polygon (no self-intersections), either winding, concave allowed:
 * ear-clipping triangulation, O(n^2). */
void cw_r2d_polygon(const cw_vec2 *pts, int n, cw_color c);
void cw_r2d_polygon_lines(const cw_vec2 *pts, int n, float width, cw_color c); /* closed cw_r2d_polyline */
/* Batched shapes: one submission each however many segments (HUD panels,
 * gauges, badges cost one call, not one per triangle).
 * Rounded rect: radius clamps to half the shorter side; `segments` per
 * corner (<= 0 picks by radius). _lines strokes `width` inside the edge. */
void cw_r2d_round_rect(cw_rect r, float radius, int segments, cw_color c);
void cw_r2d_round_rect_lines(cw_rect r, float radius, float width, int segments, cw_color c); /* the outline, width inside the edge */
/* Ring segment between radii r0 and r1 (r0 0 = a pie slice) from angle a0 to
 * a1 (radians, clockwise on screen, either direction); `segments` over the
 * sweep (<= 0 picks by radius and sweep). Two trig calls whatever the count. */
void cw_r2d_arc(cw_vec2 center, float r0, float r1, float a0, float a1, int segments, cw_color c);
/* Triangle fan: center joined to each edge pts[i]..pts[i+1], and closed
 * joins the last back to the first. Any outline that is star-shaped about
 * center (stars, torn panels, badges); no triangulation. */
void cw_r2d_fan(cw_vec2 center, const cw_vec2 *pts, int n, bool closed, cw_color c);
/* Triangulate a simple polygon into out_idx (3*(n-2) entries); returns the
 * index count, 0 if degenerate. For tools and tests. */
int cw_triangulate(const cw_vec2 *pts, int n, uint32_t *out_idx);

/* ── arbitrary geometry ─────────────────────────────────────────────────
 * uv is 0..1 over the texture's whole rect (past 1 only on repeat
 * textures). A null texture draws vertex colours. */
typedef struct cw_vertex2d { cw_vec2 pos, uv; cw_color color; } cw_vertex2d;
/* Vertices and indices are copied (caller keeps them); asserts on an index >= nv. Batches like sprites. */
void cw_r2d_triangles(cw_texture tex, const cw_vertex2d *v, int nv, const uint32_t *idx, int ni);
/* One quad: corners in order (any shape), with their UVs. */
void cw_r2d_quad(cw_texture tex, const cw_vec2 pos[4], const cw_vec2 uv[4], cw_color c);

/* ── text ────────────────────────────────────────────────────────────────
 * Fonts come from TTF/OTF via stb_truetype. Two modes:
 *   SDF:    single-channel signed distance fields (not multi-channel MSDF:
 *           corners round slightly at very large sizes; one channel is what
 *           stb_truetype generates). One bake serves every size; glyphs are
 *           baked on first use into the linear atlas.
 *   BITMAP: coverage bitmaps at one pixel size into the nearest atlas, for
 *           pixel fonts (optionally thresholded to 1-bit); draw at integer
 *           multiples of that size.
 * Layout is left-to-right with kerning (kern table and GPOS pair
 * adjustments), '\n' breaks, greedy word wrap at spaces and between CJK
 * ideographs/kana (with the common kinsoku rules: no break before closing
 * punctuation or small kana; a word longer than the wrap width breaks between
 * characters), and left/centre/right alignment. Measurement runs the same
 * layout code as drawing, so the UI can rely on it to the pixel.
 *
 * Fallback fonts: a codepoint the font lacks is taken from its own fallback
 * chain (cw_font_set_fallbacks), then the global chain
 * (cw_font_set_global_fallbacks; the l10n module sets it per locale), baked
 * at the same em size; no font having it draws the font's .notdef and counts
 * in the stats' glyphs_missing.
 *
 * Complex scripts: text containing right-to-left or complex-shaping
 * codepoints (Hebrew, Arabic, Syriac, Thaana, the Indic scripts, Thai, Lao,
 * Tibetan, Myanmar, Khmer, combining marks) is shaped by kb_text_shape (zlib
 * license, deps/kb): OpenType GSUB/GPOS (joining forms, ligatures, mark
 * placement), bidi runs (no explicit embedding/isolate controls) reordered
 * per line, the same fallback chain. Lines break in logical order and are
 * then reordered, as UAX #9 does. Pure LTR text never takes this path. */
CW_HANDLE_TYPE(cw_font);

typedef enum { CW_FONT_SDF, CW_FONT_BITMAP } cw_font_mode;
typedef struct cw_font_desc {
  cw_font_mode mode;
  float bake_size;     /* SDF: em size glyphs are baked at (0 = 48); BITMAP: the pixel size (0 = 16) */
  int sdf_padding;     /* SDF: distance range in baked pixels (0 = 6) */
  bool threshold;      /* BITMAP: 1-bit glyphs (crisp pixel fonts) */
} cw_font_desc;

/* Loads (and keeps) the font file from the asset bundle. desc NULL = SDF. */
cw_font cw_font_load(const char *asset_path, const cw_font_desc *desc);
/* bytes are copied; null handle (logged) when not a readable TTF/OTF */
cw_font cw_font_from_memory(const void *ttf, size_t len, const cw_font_desc *desc);
void cw_font_destroy(cw_font f); /* fallback chains naming it drop it; null or stale handle is a no-op */

typedef enum { CW_ALIGN_LEFT, CW_ALIGN_CENTER, CW_ALIGN_RIGHT } cw_text_align;
/* Paragraph direction for bidi text (complex path only): AUTO takes the
 * first strong character's direction. */
typedef enum { CW_TEXT_DIR_AUTO, CW_TEXT_DIR_LTR, CW_TEXT_DIR_RTL } cw_text_dir;
typedef struct cw_text_style {
  float size;           /* pixel size of the em (0 = the font's bake size) */
  cw_color color;       /* {0,0,0,0} = white */
  float wrap_width;     /* 0 = no wrapping */
  cw_text_align align;  /* within wrap_width, or within the widest line when not wrapping */
  float line_spacing;   /* multiple of the font's line height (0 = 1) */
  float letter_spacing; /* extra pixels after every glyph */
  int32_t direction;    /* cw_text_dir (complex-script text only) */
  /* Outline and drop shadow, drawn under the text from the same layout (one
   * submission, batched with it). SDF fonts grow and soften the glyphs in
   * the shader: a round outline of any width up to the font's distance range
   * (sdf_padding baked px at bake_size: 6 at 48 = 12.5% of the size by
   * default; a wider one clamps with a warning naming the padding to load the
   * font with). Bitmap fonts stamp the glyphs at every whole-pixel offset
   * within the width (softness ignored). Widths in px at this size. */
  float outline_width;      /* 0 = none */
  cw_color outline_color;   /* {0,0,0,0} = opaque black */
  cw_color shadow_color;    /* alpha 0 = no shadow */
  cw_vec2 shadow_offset;    /* px */
  float shadow_spread;      /* px the shadow grows round the glyphs */
  float shadow_softness;    /* px of blur at the shadow's edge (SDF) */
} cw_text_style;

typedef struct cw_text_metrics {
  float width, height;  /* box of the laid-out text (height = lines * line height) */
  int lines;
  float ascent, descent, line_height; /* at this size; descent is negative */
} cw_text_metrics;

/* Measure without drawing. O(length). */
cw_text_metrics cw_font_measure(cw_font f, const char *utf8, const cw_text_style *style);
/* Advance of one codepoint (no kerning) at size px. */
float cw_font_advance(cw_font f, uint32_t codepoint, float size);
/* x of the caret before byte offset `byte` on a single unwrapped line (for
 * text fields); byte past the end = full width. */
float cw_font_caret_x(cw_font f, const char *utf8, int byte, float size);
/* Fallback chain of f (copied; replaces the old one; n 0 clears). Fonts in a
 * chain must outlive their use in it (a destroyed one is skipped). */
void cw_font_set_fallbacks(cw_font f, const cw_font *chain, int n);
int cw_font_fallbacks(cw_font f, cw_font *out, int cap); /* returns the chain length */
/* Fallbacks every font tries after its own chain. */
void cw_font_set_global_fallbacks(const cw_font *chain, int n);
bool cw_font_has_glyph(cw_font f, uint32_t codepoint); /* this font alone */
bool cw_font_covers(cw_font f, uint32_t codepoint);    /* this font or one of its fallbacks */
/* The text takes the complex-script path (RTL or shaping codepoints). */
bool cw_text_is_complex(const char *utf8);
/* Draw with the box's top-left at pos (layer space; the first baseline sits
 * at pos.y + ascent). Returns the metrics. */
cw_text_metrics cw_r2d_text(cw_font f, const char *utf8, cw_vec2 pos, const cw_text_style *style);

/* ── tilemaps ────────────────────────────────────────────────────────────
 * Grid maps in chunks of CW_TILE_CHUNK x CW_TILE_CHUNK tiles per layer; each
 * chunk is static geometry in its own buffer, rebuilt only when one of its
 * cells is edited. Drawing culls chunks to the layer's view; each map layer
 * is one draw per visible chunk, ordered with everything else by the canvas
 * layer and z it was submitted at. Animated tiles cycle in the vertex shader
 * from the pass time: no rebuilds. */
#define CW_TILE_CHUNK 32
CW_HANDLE_TYPE(cw_tilemap);

/* Cell value: tile id + 1 in the low 24 bits (0 = empty), transform flags in
 * the high bits (Tiled's convention, so Tiled maps load unchanged). */
#define CW_TILE_FLIP_X 0x80000000u
#define CW_TILE_FLIP_Y 0x40000000u
#define CW_TILE_FLIP_D 0x20000000u /* swap x/y (transpose) */
#define CW_TILE_ROT90 (CW_TILE_FLIP_D | CW_TILE_FLIP_X)  /* clockwise */
#define CW_TILE_ROT180 (CW_TILE_FLIP_X | CW_TILE_FLIP_Y)
#define CW_TILE_ROT270 (CW_TILE_FLIP_D | CW_TILE_FLIP_Y)
#define CW_TILE_ID_MASK 0x00ffffffu
#define CW_TILE(id) ((uint32_t)(id) + 1u)

typedef struct cw_tilemap_desc {
  int width, height;      /* tiles */
  int layers;             /* 1..16 */
  cw_texture tileset;     /* an image texture (not a render target) */
  int tile_w, tile_h;     /* tileset cell size, texture pixels */
  int margin, spacing;    /* tileset border / gap between cells, pixels */
  int columns;            /* tiles per tileset row; 0 = from the texture width */
  float cell_w, cell_h;   /* world size of one tile; 0 = tile_w/tile_h */
} cw_tilemap_desc;

/* Cells start empty. Null handle (logged) unless the tileset is a loaded image (not a target), sizes are
 * positive and layers is 1..16. */
cw_tilemap cw_tilemap_create(const cw_tilemap_desc *desc);
void cw_tilemap_destroy(cw_tilemap m); /* null or stale handle is a no-op */
void cw_tilemap_set(cw_tilemap m, int layer, int x, int y, uint32_t cell); /* out of range ignored */
uint32_t cw_tilemap_get(cw_tilemap m, int layer, int x, int y);           /* 0 out of range */
/* Fill a layer from width*height cells (row-major). */
void cw_tilemap_set_layer(cw_tilemap m, int layer, const uint32_t *cells);
/* Tile `id` animates through ids id..id+frames-1 at fps. Takes effect when
 * the chunks holding it rebuild (immediately for cells set afterwards). */
void cw_tilemap_set_animation(cw_tilemap m, int id, int frames, float fps);
/* Submit one map layer at `origin` (layer space) into the current canvas
 * layer, z and clip. Tint multiplies. */
void cw_r2d_tilemap(cw_tilemap m, int map_layer, cw_vec2 origin, cw_color tint);
/* World position -> cell coordinates (floor). */
void cw_tilemap_world_to_cell(cw_tilemap m, cw_vec2 origin, cw_vec2 world, int *x, int *y);

/* ── 2D lights ───────────────────────────────────────────────────────────
 * Layers marked lit are drawn into an offscreen colour buffer (and, when a
 * texture with a normal map was drawn, a normal buffer), then composited as
 * colour * light. The light buffer starts at the pass's ambient colour and
 * each light adds itself (additive, radial falloff, one quad). Shadows are
 * analytic: each occluder edge facing away from the light is extruded away
 * from it into the stencil buffer, and the light is masked by it (hard
 * shadows, exact at any resolution; 2 draws per shadowed light). The light
 * buffer is RGBA8 storing light/2, so lights can overbright up to 2x.
 *
 * Lights and occluders are submitted into the current layer (its camera
 * places them) and affect the run of consecutive lit layers they are in. */
typedef struct cw_light2d {
  cw_vec2 pos;          /* layer space */
  float radius;         /* layer units; light is 0 at the radius */
  cw_color color;       /* {0,0,0,0} = white */
  float energy;         /* 0 = 1 */
  float falloff;        /* exponent of (1 - d/radius); 0 = 2 */
  float height;         /* above the plane, layer units, for normal maps; 0 = radius/4 */
  bool shadows;         /* cast shadows from occluders */
} cw_light2d;
/* current layer must be lit (else warned and ignored); asserts outside a pass; uses the current transform */
void cw_r2d_light(const cw_light2d *l);
/* Occluder outline (layer space). closed joins the ends. */
void cw_r2d_occluder(const cw_vec2 *pts, int n, bool closed);

/* ── stats ───────────────────────────────────────────────────────────────
 * Totals for the current frame (reset by the first cw_r2d_begin of each
 * engine frame), complete once the frame's last pass ended. */
typedef struct cw_r2d_stats {
  uint32_t passes;
  uint32_t draw_calls;     /* every glDraw* the renderer issued */
  uint32_t batches;        /* draws of the sorted stream (sprites/shapes/text) */
  uint32_t vertices;       /* submitted to the stream */
  uint32_t indices;
  uint32_t commands;       /* sprites + shapes + text runs + tilemap layers */
  uint32_t sorted;         /* passes that needed the sort */
  uint32_t tile_chunks;    /* chunks drawn */
  uint32_t chunk_rebuilds;
  uint32_t lights;
  uint32_t atlas_pages;    /* pages across atlas arrays (all time) */
  uint32_t texture_uploads;
  /* complex-script runs laid out this frame, by every measure and draw:
   * shaped now or reused from the shape cache (cached text still counts) */
  uint32_t shaped_runs;
  uint32_t glyphs_missing; /* distinct codepoints no font in the chain had (all time) */
  uint32_t shape_cache_hits; /* complex layouts this frame served by the shape cache (no shaping) */
} cw_r2d_stats;
const cw_r2d_stats *cw_r2d_get_stats(void); /* renderer-owned, updated as passes run; never NULL */
/* Register every stats field as a debug field "<prefix>.<name>" (u32). */
void cw_r2d_debug_register(const char *prefix);

#ifdef __cplusplus
}
#endif
#endif
