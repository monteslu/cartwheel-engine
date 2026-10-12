/* Shared inside src/resource: the built-in types and the registry. */
#ifndef CW_RESOURCE_INTERNAL_H
#define CW_RESOURCE_INTERNAL_H

#include "cartwheel/core.h"
#include "cartwheel/resource.h"

#define CW_RES_PATH_MAX 1024

/* "res://a/./b.png" -> "a/b.png"; false for "..", empty or too long. */
bool cw__res_canon(const char *in, char *out, size_t cap);

/* built-in type descriptors (types registered in CW_RES_* order) */
extern const cw_res_type_desc cw__res_texture_type, cw__res_mesh_type, cw__res_material_type, cw__res_gltf_type,
    cw__res_audio_type, cw__res_font_type, cw__res_json_type, cw__res_text_type, cw__res_binary_type;

/* textures (texture.c): CPU-side decoded image, worker-safe to produce */
typedef struct cw_tex_level { uint8_t *data; size_t size; int w, h; } cw_tex_level;
typedef struct cw_tex_cpu {
  int width, height, levels;
  uint32_t gl_format;      /* internal format; GL_RGBA8 for uncompressed */
  bool compressed, from_ktx2, gen_mips;
  bool srgb;               /* a KTX2 whose DFD transfer function is sRGB (cwpack texture.color = srgb) */
  const char *format;      /* "rgba8", "bc7", ... */
  cw_tex_level level[16];
} cw_tex_cpu;
bool cw__tex_decode(const char *path, const uint8_t *bytes, size_t len, cw_tex_cpu *out);
void cw__tex_cpu_free(cw_tex_cpu *t);
/* Upload into a three.c texture (main thread): its GL texture is created,
 * filled and handed to three.c; pixels freed; sampler state and colour space
 * (an sRGB internal format for T3_SRGB_COLOR_SPACE) from t's fields, so set
 * them first. On WebGPU three.c uploads the texels (level 0) itself. Returns
 * GPU bytes. */
size_t cw__tex_upload(struct t3_texture *t, cw_tex_cpu *cpu);
/* Choose the KTX2 transcode target for the current GL context (main
 * thread, once, before any KTX2 decode job). */
void cw__tex_pick_ktx2_target(void);
/* A glTF shows the image file at path (its texture resource) as colour
 * (srgb) or data, before it asks for that resource: true when the resource
 * is, or will load, in that colour space (the first claim before it loads
 * decides); false when it is not, and the glTF needs its own upload. */
bool cw__tex_res_claim(const char *path, bool srgb);
/* Move b's GL texture and size into a (hot reload in place); b keeps a's. */
void cw__tex_swap(struct t3_texture *a, struct t3_texture *b);
/* Free a three.c texture whose GL texture we created, even with no renderer. */
void cw__tex_release(struct t3_texture *t);

/* a texture resource's payload (also what glTF texture sub-resources hold) */
typedef struct cw_tex_payload { struct t3_texture *tex; cw_texture_info info; } cw_tex_payload;

/* KTX2 / Basis Universal transcoder (basis_glue.cpp) */
enum { CW_KTX2_RGBA32, CW_KTX2_BC7, CW_KTX2_ASTC4x4, CW_KTX2_ETC2, CW_KTX2_BC3 };
bool cw__ktx2_is(const uint8_t *bytes, size_t len);
bool cw__ktx2_transcode(const uint8_t *bytes, size_t len, int target, cw_tex_cpu *out, char *err, size_t err_cap);
void cw__ktx2_init(void);

/* glTF (gltf.c): hot reload in place re-links instances */
void cw__gltf_shutdown(void);

uint64_t cw__res_frame_index(void);
/* payload when READY and of type t */
void *cw__res_get_typed(cw_res h, cw_res_type t);

/* hot reload (hotreload.c), called from cw_res_frame */
void cw__res_hotreload_tick(void);
int cw__res_reload_changed(void);

#endif
