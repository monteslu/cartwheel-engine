/* showcase world generation (see world.h). */
#include "world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/math.h"

/* ── deterministic noise ────────────────────────────────────────────── */

static uint32_t hash2(int32_t x, int32_t z, uint32_t seed) {
  uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)z * 0xd8163841u ^ seed * 0xcb1ab31fu;
  h ^= h >> 15;
  h *= 0x2c1b3c6du;
  h ^= h >> 12;
  h *= 0x297a2d39u;
  h ^= h >> 15;
  return h;
}
static float hashf(int32_t x, int32_t z, uint32_t seed) { return (float)(hash2(x, z, seed) >> 8) * (1.0f / 16777216.0f); }

static float value_noise(float x, float z, uint32_t seed) {
  float fx = floorf(x), fz = floorf(z);
  int32_t ix = (int32_t)fx, iz = (int32_t)fz;
  float tx = x - fx, tz = z - fz;
  /* quintic fade */
  tx = tx * tx * tx * (tx * (tx * 6 - 15) + 10);
  tz = tz * tz * tz * (tz * (tz * 6 - 15) + 10);
  float a = hashf(ix, iz, seed), b = hashf(ix + 1, iz, seed), c = hashf(ix, iz + 1, seed), d = hashf(ix + 1, iz + 1, seed);
  return cw_lerp(cw_lerp(a, b, tx), cw_lerp(c, d, tx), tz);
}

static float fbm(float x, float z) {
  float s = 0, amp = 0.5f, f = 1;
  for (int o = 0; o < 5; o++) {
    s += amp * value_noise(x * f, z * f, 17u + (uint32_t)o);
    f *= 2.03f;
    amp *= 0.5f;
  }
  return s; /* about 0..1 */
}

/* the village: a flat basin around the origin, the road along x */
#define VILLAGE_R 58.0f
float sc_height(float x, float z) {
  float d = sqrtf(x * x + z * z);
  float hills = fbm(x / 70.0f + 3.1f, z / 70.0f - 1.7f) * 46.0f - 8.0f;
  /* a rim of higher ground far out: the valley's walls */
  float rim = cw_smoothstep(120, 230, d) * 34.0f;
  float ground = 1.2f + 0.9f * value_noise(x / 9.0f, z / 9.0f, 5u);
  float w = cw_smoothstep(VILLAGE_R, VILLAGE_R + 70, d);
  /* the road continues out of the village to the east and west */
  float road = 1.0f - cw_smoothstep(4, 14, fabsf(z));
  float hw = hills + rim;
  float h = cw_lerp(ground, hw > ground ? hw : ground, w * (1.0f - 0.85f * road));
  return h;
}

static t3_vec3 normal_at(float x, float z) {
  const float e = 0.5f;
  float hx = sc_height(x + e, z) - sc_height(x - e, z);
  float hz = sc_height(x, z + e) - sc_height(x, z - e);
  t3_vec3 n = { -hx, 2 * e, -hz };
  float l = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
  return (t3_vec3){ n.x / l, n.y / l, n.z / l };
}

static t3_color ground_color(float x, float z, float h, t3_vec3 n) {
  float v = value_noise(x / 6.0f, z / 6.0f, 9u);
  t3_color grass = { 0.06f + 0.04f * v, 0.13f + 0.05f * v, 0.03f };
  t3_color dry = { 0.20f, 0.17f, 0.08f };
  t3_color rock = { 0.21f + 0.05f * v, 0.20f + 0.05f * v, 0.18f + 0.04f * v };
  t3_color dirt = { 0.19f, 0.13f, 0.08f };
  float dryness = cw_smoothstep(10, 40, h) * 0.6f + 0.3f * value_noise(x / 40.0f, z / 40.0f, 11u);
  t3_color c = { cw_lerp(grass.r, dry.r, dryness), cw_lerp(grass.g, dry.g, dryness), cw_lerp(grass.b, dry.b, dryness) };
  float steep = cw_smoothstep(0.86f, 0.70f, n.y);
  c = (t3_color){ cw_lerp(c.r, rock.r, steep), cw_lerp(c.g, rock.g, steep), cw_lerp(c.b, rock.b, steep) };
  /* the road and the square */
  float d = sqrtf(x * x + z * z);
  float path = (1.0f - cw_smoothstep(2.6f, 3.6f, fabsf(z))) * (1.0f - cw_smoothstep(200, 230, fabsf(x)));
  float square = 1.0f - cw_smoothstep(11, 13, d);
  float p = path > square ? path : square;
  c = (t3_color){ cw_lerp(c.r, dirt.r, p), cw_lerp(c.g, dirt.g, p), cw_lerp(c.b, dirt.b, p) };
  return c;
}

/* ── validation: nothing malformed ever reaches GL ──────────────────── */

static bool check_geometry(const char *what, const t3_geometry *g) {
  const t3_attribute *pos = g->attributes[T3_ATTR_POSITION];
  if (!pos || pos->count <= 0 || pos->item_size != 3 || !pos->array) { cw_error("showcase: %s: no positions", what); return false; }
  for (int s = 0; s < T3_ATTR_COUNT; s++) {
    const t3_attribute *a = g->attributes[s];
    if (a && s != T3_ATTR_INSTANCE_MATRIX && s != T3_ATTR_INSTANCE_COLOR && a->count != pos->count) {
      cw_error("showcase: %s: attribute %d has %d items, positions %d", what, s, a->count, pos->count);
      return false;
    }
  }
  const float *p = (const float *)pos->array;
  for (int i = 0; i < pos->count * 3; i++)
    if (!(p[i] == p[i]) || fabsf(p[i]) > 1e5f) { cw_error("showcase: %s: position %d is not finite", what, i / 3); return false; }
  if (g->index) {
    const t3_attribute *ix = g->index;
    if (ix->count % 3 || ix->item_size != 1) { cw_error("showcase: %s: index count %d", what, ix->count); return false; }
    for (int i = 0; i < ix->count; i++) {
      uint32_t v = ix->type == T3_UINT16 ? ((const uint16_t *)ix->array)[i] : ((const uint32_t *)ix->array)[i];
      if (v >= (uint32_t)pos->count) { cw_error("showcase: %s: index %d = %u >= %d vertices", what, i, v, pos->count); return false; }
    }
  }
  return true;
}

/* ── terrain ────────────────────────────────────────────────────────── */

#define CHUNK_SIZE (SC_TERRAIN_SIZE / SC_CHUNKS)
static const int lod_quads[SC_LODS] = { 40, 16, 6 };
static const float lod_dist[SC_LODS] = { 0, 70, 160 };

/* one chunk at one LOD: an (n+1)^2 grid plus a skirt hanging 4 m below every
 * edge vertex (the next LOD's coarser edge can sit above or below: the skirt
 * fills the crack) */
static t3_geometry *chunk_geometry(float x0, float z0, int n, int *tris) {
  int grid = (n + 1) * (n + 1), skirt = 4 * n, vc = grid + skirt;
  float *pos = malloc(sizeof(float) * 3 * (size_t)vc), *nor = malloc(sizeof(float) * 3 * (size_t)vc),
        *col = malloc(sizeof(float) * 3 * (size_t)vc);
  int ic = n * n * 6 + skirt * 6;
  uint16_t *idx = malloc(sizeof(uint16_t) * (size_t)ic);
  float step = CHUNK_SIZE / (float)n;
  for (int j = 0; j <= n; j++)
    for (int i = 0; i <= n; i++) {
      int v = j * (n + 1) + i;
      float x = x0 + (float)i * step, z = z0 + (float)j * step, h = sc_height(x, z);
      t3_vec3 nn = normal_at(x, z);
      t3_color c = ground_color(x, z, h, nn);
      pos[v * 3] = x; pos[v * 3 + 1] = h; pos[v * 3 + 2] = z;
      nor[v * 3] = nn.x; nor[v * 3 + 1] = nn.y; nor[v * 3 + 2] = nn.z;
      col[v * 3] = c.r; col[v * 3 + 1] = c.g; col[v * 3 + 2] = c.b;
    }
  int k = 0;
  for (int j = 0; j < n; j++)
    for (int i = 0; i < n; i++) {
      uint16_t a = (uint16_t)(j * (n + 1) + i), b = (uint16_t)(a + 1), c = (uint16_t)(a + n + 1), d = (uint16_t)(c + 1);
      /* counter-clockwise seen from above (+y) */
      idx[k++] = a; idx[k++] = c; idx[k++] = b;
      idx[k++] = b; idx[k++] = c; idx[k++] = d;
    }
  /* the skirt: walk the border once around (4n edge vertices), each with a
   * copy 4 m lower; quads between consecutive pairs */
  int ring[4 * 40 + 4];
  int r = 0;
  for (int i = 0; i < n; i++) ring[r++] = i;                           /* z0 edge, +x */
  for (int j = 0; j < n; j++) ring[r++] = j * (n + 1) + n;             /* x1 edge, +z */
  for (int i = n; i > 0; i--) ring[r++] = n * (n + 1) + i;             /* z1 edge, -x */
  for (int j = n; j > 0; j--) ring[r++] = j * (n + 1);                 /* x0 edge, -z */
  for (int s = 0; s < skirt; s++) {
    int src = ring[s], v = grid + s;
    memcpy(&pos[v * 3], &pos[src * 3], sizeof(float) * 3);
    pos[v * 3 + 1] -= 4.0f;
    memcpy(&nor[v * 3], &nor[src * 3], sizeof(float) * 3);
    memcpy(&col[v * 3], &col[src * 3], sizeof(float) * 3);
  }
  for (int s = 0; s < skirt; s++) {
    uint16_t a = (uint16_t)ring[s], b = (uint16_t)ring[(s + 1) % skirt];
    uint16_t a2 = (uint16_t)(grid + s), b2 = (uint16_t)(grid + (s + 1) % skirt);
    /* double sided enough: the skirt is seen from outside the chunk */
    idx[k++] = a; idx[k++] = b; idx[k++] = a2;
    idx[k++] = b; idx[k++] = b2; idx[k++] = a2;
  }
  t3_geometry *g = t3_geometry_new();
  t3_attribute *pa = t3_attribute_new(T3_FLOAT32, pos, vc, 3), *na = t3_attribute_new(T3_FLOAT32, nor, vc, 3),
               *ca = t3_attribute_new(T3_FLOAT32, col, vc, 3), *ia = t3_attribute_new(T3_UINT16, idx, k, 1);
  t3_geometry_set_attribute(g, T3_ATTR_POSITION, pa);
  t3_geometry_set_attribute(g, T3_ATTR_NORMAL, na);
  t3_geometry_set_attribute(g, T3_ATTR_COLOR, ca);
  t3_geometry_set_index(g, ia);
  t3_release(pa); t3_release(na); t3_release(ca); t3_release(ia);
  free(pos); free(nor); free(col); free(idx);
  t3_geometry_compute_bounding_sphere(g);
  t3_geometry_compute_bounding_box(g);
  *tris = k / 3;
  return g;
}

static bool build_terrain(sc_world *w, t3_object *parent) {
  t3_material *m = t3_mesh_standard_material_new(0xffffff);
  m->vertex_colors = true;
  m->roughness = 0.94f;
  m->metalness = 0;
  bool ok = true;
  for (int cz = 0; cz < SC_CHUNKS && ok; cz++)
    for (int cx = 0; cx < SC_CHUNKS && ok; cx++) {
      float x0 = -SC_TERRAIN_SIZE / 2 + (float)cx * CHUNK_SIZE, z0 = -SC_TERRAIN_SIZE / 2 + (float)cz * CHUNK_SIZE;
      t3_lod *lod = t3_lod_new();
      for (int l = 0; l < SC_LODS; l++) {
        int tris;
        t3_geometry *g = chunk_geometry(x0, z0, lod_quads[l], &tris);
        if (!check_geometry("terrain chunk", g)) { ok = false; t3_release(g); break; }
        if (l == 0) w->triangles_l0 += tris;
        t3_mesh *mesh = t3_mesh_new(g, m);
        mesh->base.cast_shadow = true;
        mesh->base.receive_shadow = true;
        t3_lod_add_level(lod, mesh, lod_dist[l]);
        t3_release(mesh);
        t3_release(g);
      }
      /* the LOD picks by distance to its own position: the chunk's centre */
      t3_object_set_position(lod, 0, 0, 0);
      /* levels are placed in world space; move the LOD to the chunk's centre
       * and the levels back so distances are measured from the centre */
      float ccx = x0 + CHUNK_SIZE / 2, ccz = z0 + CHUNK_SIZE / 2, cy = sc_height(ccx, ccz);
      t3_object_set_position(lod, ccx, cy, ccz);
      for (int l = 0; l < lod->level_count; l++) t3_object_set_position(lod->levels[l].object, -ccx, -cy, -ccz);
      t3_object_add(parent, lod);
      w->chunks[cz * SC_CHUNKS + cx] = lod;
      t3_release(lod);
    }
  t3_release(m);
  return ok;
}

/* ── instanced helpers ──────────────────────────────────────────────── */

static t3_instanced_mesh *instanced(t3_object *parent, t3_geometry *g, t3_material *m, int count, bool shadows) {
  if (!check_geometry("instanced", g)) return NULL;
  t3_instanced_mesh *im = t3_instanced_mesh_new(g, m, count);
  /* r128 culls an InstancedMesh by its geometry's sphere at the object's
   * origin, not by the instances: draw it whole */
  im->mesh.base.frustum_culled = false;
  im->mesh.base.cast_shadow = shadows;
  im->mesh.base.receive_shadow = true;
  t3_object_add(parent, im);
  return im;
}

static void place(t3_instanced_mesh *im, int i, t3_vec3 p, float yaw, t3_vec3 s) {
  t3_mat4 m;
  t3_mat4_compose(&m, p, t3_quat_from_axis_angle((t3_vec3){ 0, 1, 0 }, yaw), s);
  t3_instanced_mesh_set_matrix_at(im, i, &m);
}

/* ── forest ─────────────────────────────────────────────────────────── */

static bool build_forest(sc_world *w, t3_object *parent) {
  cw_rng rng;
  cw_rng_seed(&rng, 1400);
  t3_vec3 at[SC_TREES];
  float sc[SC_TREES], yaw[SC_TREES];
  int n = 0;
  for (int tries = 0; n < SC_TREES && tries < SC_TREES * 20; tries++) {
    float x = cw_rng_range(&rng, -225, 225), z = cw_rng_range(&rng, -225, 225);
    float d = sqrtf(x * x + z * z);
    float clump = value_noise(x / 35.0f, z / 35.0f, 23u);
    float keep = cw_rng_float(&rng);
    if (d < VILLAGE_R + 6 || fabsf(z) < 9 || clump < 0.42f || keep > clump) continue;
    t3_vec3 nn = normal_at(x, z);
    if (nn.y < 0.78f) continue;
    at[n] = (t3_vec3){ x, sc_height(x, z) - 0.3f, z };
    sc[n] = cw_rng_range(&rng, 0.75f, 1.45f);
    yaw[n] = cw_rng_range(&rng, 0, CW_TAU);
    n++;
  }
  w->tree_count = n;
  t3_geometry *trunk = t3_cylinder_geometry_new(0.16f, 0.28f, 2.4f, 6, 1, false);
  t3_geometry *crown = t3_cone_geometry_new(1.7f, 5.0f, 9, 1, false);
  t3_geometry *crown2 = t3_cone_geometry_new(1.25f, 3.6f, 9, 1, false);
  t3_material *bark = t3_mesh_standard_material_new(0xffffff);
  bark->color = (t3_color){ 0.12f, 0.08f, 0.05f };
  bark->roughness = 0.9f;
  t3_material *needles = t3_mesh_standard_material_new(0xffffff);
  needles->color = (t3_color){ 1, 1, 1 };
  needles->roughness = 0.85f;
  t3_instanced_mesh *it = instanced(parent, trunk, bark, n, true);
  t3_instanced_mesh *ic = instanced(parent, crown, needles, n, true);
  t3_instanced_mesh *ic2 = instanced(parent, crown2, needles, n, true);
  bool ok = it && ic && ic2;
  for (int i = 0; ok && i < n; i++) {
    t3_vec3 s = { sc[i], sc[i], sc[i] };
    place(it, i, (t3_vec3){ at[i].x, at[i].y + 1.2f * sc[i], at[i].z }, yaw[i], s);
    place(ic, i, (t3_vec3){ at[i].x, at[i].y + 4.4f * sc[i], at[i].z }, yaw[i], s);
    place(ic2, i, (t3_vec3){ at[i].x, at[i].y + 6.6f * sc[i], at[i].z }, yaw[i] + 0.3f, s);
    float v = hashf(i, 7, 3u);
    t3_color g = { 0.035f + 0.03f * v, 0.075f + 0.05f * v, 0.03f + 0.01f * v };
    t3_instanced_mesh_set_color_at(ic, i, g);
    t3_instanced_mesh_set_color_at(ic2, i, (t3_color){ g.r * 1.15f, g.g * 1.15f, g.b * 1.1f });
  }
  if (it) t3_release(it);
  if (ic) t3_release(ic);
  if (ic2) t3_release(ic2);
  t3_release(trunk); t3_release(crown); t3_release(crown2); t3_release(bark); t3_release(needles);
  return ok;
}

/* ── village ────────────────────────────────────────────────────────── */

static cw_node mesh_node(cw_node parent, const char *name, const char *prim, t3_vec3 p, t3_vec3 size, float r, float g,
                         float b, float rough) {
  cw_node n = cw_node_new("MeshInstance3D", name);
  cw_node_set(n, "primitive", cw_val_enum(prim));
  cw_node_set(n, "size", cw_val_vec3(size.x, size.y, size.z));
  cw_node_set(n, "color", cw_val_color(r, g, b, 1));
  cw_node_set(n, "roughness", cw_val_float(rough));
  cw_node_set(n, "cast_shadow", cw_val_bool(true));
  cw_node_set(n, "receive_shadow", cw_val_bool(true));
  cw_node_set(n, "static", cw_val_bool(true)); /* joins the visibility BVH */
  cw_node_add_child(parent, n);
  cw_node3d_set_position(n, p);
  return n;
}

#define HOUSES 16
static bool build_village(sc_world *w, cw_node root, t3_object *parent) {
  cw_rng rng;
  cw_rng_seed(&rng, 77);
  cw_node village = cw_node_new("Node3D", "Village");
  cw_node_add_child(root, village);
  /* houses on both sides of the road and around the square */
  t3_geometry *roof = t3_cone_geometry_new(1, 1, 4, 1, false);
  t3_material *roof_m = t3_mesh_standard_material_new(0xffffff);
  roof_m->color = (t3_color){ 0.25f, 0.07f, 0.04f };
  roof_m->roughness = 0.7f;
  t3_instanced_mesh *roofs = instanced(parent, roof, roof_m, HOUSES, true);
  t3_geometry *win = t3_plane_geometry_new(1, 1, 1, 1);
  t3_material *win_m = t3_mesh_basic_material_new(0xffffff);
  w->windows = instanced(parent, win, win_m, SC_WINDOWS, false);
  if (!roofs || !w->windows) return false;
  int wi = 0;
  for (int h = 0; h < HOUSES; h++) {
    float side = (h & 1) ? 1.0f : -1.0f;
    float x = -50 + (float)(h / 2) * 14.0f + cw_rng_range(&rng, -1.5f, 1.5f);
    if (fabsf(x) < 16) x += x < 0 ? -10 : 10; /* keep the square open */
    float z = side * cw_rng_range(&rng, 11, 15);
    float wdt = cw_rng_range(&rng, 6, 8), dep = cw_rng_range(&rng, 5, 7), hgt = cw_rng_range(&rng, 3.6f, 5.2f);
    float y = sc_height(x, z);
    char name[16];
    snprintf(name, sizeof name, "House%d", h);
    float tint = cw_rng_range(&rng, 0, 1);
    mesh_node(village, name, "BOX", (t3_vec3){ x, y + hgt / 2 - 0.3f, z }, (t3_vec3){ wdt, hgt + 0.6f, dep },
              0.55f + 0.15f * tint, 0.48f + 0.1f * tint, 0.38f, 0.85f);
    w->chimney_top[h] = (t3_vec3){ x + wdt * 0.25f, y + hgt + 2.9f, z + dep * 0.1f };
    snprintf(name, sizeof name, "Chimney%d", h);
    mesh_node(village, name, "BOX", (t3_vec3){ x + wdt * 0.25f, y + hgt + 1.6f, z + dep * 0.1f }, (t3_vec3){ 0.7f, 2.4f, 0.7f },
              0.3f, 0.28f, 0.27f, 0.9f);
    /* a four-sided cone turned 45 degrees: a hip roof over the box */
    t3_mat4 m;
    t3_mat4_compose(&m, (t3_vec3){ x, y + hgt + 1.15f, z }, t3_quat_from_axis_angle((t3_vec3){ 0, 1, 0 }, CW_PI / 4),
                    (t3_vec3){ wdt * 0.78f, 2.3f, dep * 0.78f });
    t3_instanced_mesh_set_matrix_at(roofs, h, &m);
    /* windows on the road side: 6 a house, two rows of three */
    float face = z - side * (dep / 2 + 0.02f);
    for (int r = 0; r < 2; r++)
      for (int c = 0; c < 3 && wi < SC_WINDOWS; c++, wi++) {
        float wx = x + ((float)c - 1) * wdt * 0.28f, wy = y + 1.3f + (float)r * (hgt * 0.45f);
        t3_mat4 wm;
        t3_mat4_compose(&wm, (t3_vec3){ wx, wy, face }, t3_quat_from_axis_angle((t3_vec3){ 0, 1, 0 }, side > 0 ? CW_PI : 0),
                        (t3_vec3){ 0.9f, 1.1f, 1 });
        t3_instanced_mesh_set_matrix_at(w->windows, wi, &wm);
        w->window_stagger[wi] = cw_rng_range(&rng, -3, 5);
        float warm = cw_rng_range(&rng, 0, 1);
        w->window_color[wi] = (t3_color){ 1.0f, 0.55f + 0.2f * warm, 0.22f + 0.15f * warm };
        t3_instanced_mesh_set_color_at(w->windows, wi, (t3_color){ 0.02f, 0.025f, 0.03f });
      }
  }
  t3_release(roofs); t3_release(roof); t3_release(roof_m); t3_release(win); t3_release(win_m);

  /* lamp posts: along both road edges, around the square, out along the road */
  t3_geometry *post = t3_cylinder_geometry_new(0.07f, 0.1f, 4.0f, 6, 1, false);
  t3_material *iron = t3_mesh_standard_material_new(0xffffff);
  iron->color = (t3_color){ 0.04f, 0.04f, 0.045f };
  iron->metalness = 0.6f;
  iron->roughness = 0.5f;
  t3_geometry *bulb = t3_sphere_geometry_new(0.22f, 10, 6);
  t3_material *glow = t3_mesh_basic_material_new(0xffffff);
  t3_instanced_mesh *posts = instanced(parent, post, iron, SC_LAMPS, true);
  w->bulbs = instanced(parent, bulb, glow, SC_LAMPS, false);
  if (!posts || !w->bulbs) return false;
  cw_node lamps = cw_node_new("Node3D", "Lamps");
  cw_node_add_child(root, lamps);
  for (int i = 0; i < SC_LAMPS; i++) {
    float x, z;
    if (i < 16) { /* the square */
      float a = (float)i / 16 * CW_TAU, s, c;
      cw_sincos(a, &s, &c);
      x = c * 12.5f;
      z = s * 12.5f;
    } else { /* both road edges, 112 posts every 4 m from x = -222 to 222 */
      int k = i - 16;
      x = -222 + (float)(k / 2) * 8.0f + ((k & 1) ? 4.0f : 0);
      z = (k & 1) ? 3.9f : -3.9f;
      if (fabsf(x) < 13.5f) z *= 3.6f; /* step around the square */
    }
    float y = sc_height(x, z);
    place(posts, i, (t3_vec3){ x, y + 2.0f, z }, 0, (t3_vec3){ 1, 1, 1 });
    place(w->bulbs, i, (t3_vec3){ x, y + 4.15f, z }, 0, (t3_vec3){ 1, 1, 1 });
    t3_instanced_mesh_set_color_at(w->bulbs, i, (t3_color){ 0.05f, 0.05f, 0.05f });
    sc_lamp *l = &w->lamps[i];
    l->pos = (t3_vec3){ x, y + 4.15f, z };
    l->stagger = hashf(i, 3, 41u) * 6.0f - 1.0f; /* turns on between 5 and -1 degrees */
  }
  t3_release(posts); t3_release(post); t3_release(iron); t3_release(bulb); t3_release(glow);

  /* the campfire on the square: logs, stones, a shadowed flickering light */
  w->campfire_pos = (t3_vec3){ 0, sc_height(0, 0), 0 };
  for (int i = 0; i < 4; i++) {
    char name[16];
    snprintf(name, sizeof name, "Log%d", i);
    cw_node lg = mesh_node(village, name, "CYLINDER", (t3_vec3){ 0, w->campfire_pos.y + 0.2f, 0 }, (t3_vec3){ 0.3f, 1.8f, 0.3f },
                           0.1f, 0.06f, 0.03f, 0.9f);
    cw_node3d_set_rotation(lg, (t3_vec3){ 1.25f, (float)i * CW_PI / 4, 0 });
  }
  w->campfire = cw_node_new("OmniLight3D", "Campfire");
  cw_node_set(w->campfire, "color", cw_val_color(1.0f, 0.5f, 0.18f, 1));
  cw_node_set(w->campfire, "energy", cw_val_float(0));
  cw_node_set(w->campfire, "range", cw_val_float(22));
  cw_node_set(w->campfire, "shadow", cw_val_bool(true));
  cw_node_set(w->campfire, "shadow_size", cw_val_int(512));
  cw_node_add_child(lamps, w->campfire);
  cw_node3d_set_position(w->campfire, (t3_vec3){ 0, w->campfire_pos.y + 1.1f, 0 });
  /* the few real lights the lamps share (sc_world_lamp_lights), made after
   * the campfire: three.c orders a kind's lights by id and needs its shadow
   * casters first (a caster after a plain light has no program: every lit
   * material was skipped while the lamps came first) */
  for (int i = 0; i < SC_LAMP_LIGHTS; i++) {
    sc_lamp_light *ll = &w->lamp_lights[i];
    char name[16];
    snprintf(name, sizeof name, "LampLight%d", i);
    ll->light = cw_node_new("OmniLight3D", name);
    ll->lamp = -1;
    cw_node_set(ll->light, "color", cw_val_color(1.0f, 0.72f, 0.42f, 1));
    cw_node_set(ll->light, "energy", cw_val_float(0));
    cw_node_set(ll->light, "range", cw_val_float(10));
    cw_node_add_child(lamps, ll->light);
  }

  /* fireflies over the meadows by the road */
  t3_geometry *dot = t3_sphere_geometry_new(0.06f, 6, 4);
  t3_material *dot_m = t3_mesh_basic_material_new(0xffffff);
  w->fly_glow = instanced(parent, dot, dot_m, SC_FIREFLIES, false);
  t3_release(dot); t3_release(dot_m);
  if (!w->fly_glow) return false;
  for (int i = 0; i < SC_FIREFLIES; i++) {
    sc_firefly *f = &w->flies[i];
    f->cx = cw_rng_range(&rng, -60, 60);
    f->cz = (i & 1 ? 1 : -1) * cw_rng_range(&rng, 18, 40);
    f->r = cw_rng_range(&rng, 1.5f, 5);
    f->speed = cw_rng_range(&rng, -0.02f, 0.02f);
    f->phase = cw_rng_range(&rng, 0, CW_TAU);
    f->h = cw_rng_range(&rng, 0.6f, 2.2f);
  }
  return true;
}

bool sc_world_build(sc_world *w, cw_node root) {
  memset(w, 0, sizeof *w);
  t3_object *parent = cw_node3d_object(root);
  w->ok = build_terrain(w, parent) && build_forest(w, parent) && build_village(w, root, parent);
  if (!w->ok) cw_error("showcase: world generation failed validation; nothing more is drawn");
  return w->ok;
}

/* three.js's needsUpdate: written instance data uploads on the next draw */
static void instances_changed(t3_instanced_mesh *im) {
  im->instance_matrix->version++;
  if (im->instance_color) im->instance_color->version++;
}

/* ── particles, decals, visibility ────────────────────── */

static cw_node particles(cw_node parent, const char *name, t3_vec3 at) {
  cw_node n = cw_node_new("GPUParticles3D", name);
  cw_node_add_child(parent, n);
  cw_node3d_set_position(n, at);
  return n;
}
#define SETF(n, k, v) cw_node_set(n, k, cw_val_float(v))
#define SETI(n, k, v) cw_node_set(n, k, cw_val_int(v))
#define SETS(n, k, v) cw_node_set(n, k, cw_val_string(v))
#define SETE(n, k, v) cw_node_set(n, k, cw_val_enum(v))
#define SETV(n, k, x, y, z) cw_node_set(n, k, cw_val_vec3(x, y, z))

static void smoke(cw_node n, int amount, float size, float seed) {
  SETI(n, "amount", amount);
  SETF(n, "lifetime", 6);
  SETE(n, "emission_shape", "SPHERE");
  SETF(n, "emission_radius", 0.2f);
  SETF(n, "spread", 15);
  SETF(n, "speed_min", 0.6f);
  SETF(n, "speed_max", 1.1f);
  SETV(n, "gravity", 0.35f, 0.2f, 0.1f); /* a light breeze */
  SETF(n, "damping", 0.2f);
  SETF(n, "turbulence", 1.2f);
  SETF(n, "turbulence_scale", 0.3f);
  SETF(n, "turbulence_speed", 0.25f);
  SETE(n, "blend", "ALPHA");
  cw_node_set(n, "sort", cw_val_bool(true));
  SETF(n, "soft_distance", 0.6f);
  SETF(n, "size", size);
  SETF(n, "size_random", 0.3f);
  SETS(n, "size_curve", "0 0.3, 0.3 0.9, 1 2.2");
  SETS(n, "color_ramp", "0 0.32 0.31 0.3 0, 0.12 0.3 0.29 0.28 0.45, 1 0.25 0.25 0.26 0");
  SETF(n, "spin", 0.3f);
  SETI(n, "seed", (int)seed);
}

void sc_world_build_fx(sc_world *w, cw_node root) {
  if (!w->ok) return;
  cw_node fx = cw_node_new("Node3D", "Fx");
  cw_node_add_child(root, fx);
  t3_vec3 c = w->campfire_pos;
  /* flames: short-lived, rising, white-yellow to deep red, HDR so they bloom */
  w->fire = particles(fx, "Fire", (t3_vec3){ c.x, c.y + 0.25f, c.z });
  SETI(w->fire, "amount", 1600);
  SETF(w->fire, "lifetime", 0.9f);
  SETF(w->fire, "lifetime_random", 0.4f);
  SETE(w->fire, "emission_shape", "SPHERE");
  SETF(w->fire, "emission_radius", 0.45f);
  SETF(w->fire, "spread", 12);
  SETF(w->fire, "speed_min", 0.8f);
  SETF(w->fire, "speed_max", 1.8f);
  SETV(w->fire, "gravity", 0, 1.2f, 0);
  SETF(w->fire, "damping", 0.8f);
  SETF(w->fire, "turbulence", 2.0f);
  SETF(w->fire, "turbulence_scale", 0.6f);
  SETF(w->fire, "size", 0.32f);
  SETF(w->fire, "size_random", 0.4f);
  SETS(w->fire, "size_curve", "0 0.4, 0.2 1, 1 0.1");
  SETS(w->fire, "color_ramp", "0 1 0.85 0.5 0, 0.1 1 0.7 0.25 1, 0.5 1 0.35 0.06 0.8, 1 0.5 0.08 0.02 0");
  SETF(w->fire, "energy", 7);
  SETF(w->fire, "soft_distance", 0.3f);
  SETI(w->fire, "seed", 31);
  /* sparks: stretched streaks thrown up and drifting */
  w->sparks = particles(fx, "Sparks", (t3_vec3){ c.x, c.y + 0.5f, c.z });
  SETI(w->sparks, "amount", 300);
  SETF(w->sparks, "lifetime", 2.2f);
  SETF(w->sparks, "lifetime_random", 0.5f);
  SETE(w->sparks, "emission_shape", "SPHERE");
  SETF(w->sparks, "emission_radius", 0.3f);
  SETF(w->sparks, "spread", 25);
  SETF(w->sparks, "speed_min", 1.5f);
  SETF(w->sparks, "speed_max", 3.5f);
  SETV(w->sparks, "gravity", 0.2f, 0.4f, 0);
  SETF(w->sparks, "damping", 0.3f);
  SETF(w->sparks, "turbulence", 3.0f);
  SETF(w->sparks, "turbulence_scale", 0.8f);
  SETE(w->sparks, "draw_mode", "STRETCHED");
  SETF(w->sparks, "stretch", 0.04f);
  SETF(w->sparks, "size", 0.03f);
  SETS(w->sparks, "color_ramp", "0 1 0.8 0.4 1, 0.6 1 0.4 0.1 1, 1 0.6 0.1 0.02 0");
  SETF(w->sparks, "energy", 12);
  SETI(w->sparks, "seed", 37);
  /* the fire's smoke and the chimneys' */
  w->fire_smoke = particles(fx, "FireSmoke", (t3_vec3){ c.x, c.y + 1.6f, c.z });
  smoke(w->fire_smoke, 500, 0.9f, 41);
  w->smokes[w->smoke_count++] = w->fire_smoke;
  w->smoke_light = -1;
  for (int h = 0; h < 16; h += 3) {
    char name[16];
    snprintf(name, sizeof name, "Smoke%d", h);
    cw_node sm = particles(fx, name, w->chimney_top[h]);
    smoke(sm, 260, 0.7f, (float)(50 + h));
    w->smokes[w->smoke_count++] = sm;
  }
  w->fire_on = true;
  /* decals: the fire's burn mark on the square, wear on the road */
  cw_node d = cw_node_new("Decal3D", "Scorch");
  cw_node_add_child(fx, d);
  cw_node3d_set_position(d, (t3_vec3){ c.x, c.y + 0.5f, c.z });
  cw_node3d_set_rotation(d, (t3_vec3){ -CW_PI / 2, 0, 0 });
  SETV(d, "size", 4.2f, 4.2f, 3.0f);
  SETS(d, "texture", "decals/scorch.png");
  SETE(d, "blend", "MULTIPLY");
  SETF(d, "normal_fade", 0);
  for (int i = 0; i < 10; i++) {
    char name[16];
    snprintf(name, sizeof name, "Wear%d", i);
    float x = -95 + (float)i * 21.0f + 5.0f * hashf(i, 1, 61u), z = -1.6f + 3.2f * hashf(i, 2, 61u);
    cw_node e = cw_node_new("Decal3D", name);
    cw_node_add_child(fx, e);
    cw_node3d_set_position(e, (t3_vec3){ x, sc_height(x, z) + 0.5f, z });
    cw_node3d_set_rotation(e, (t3_vec3){ -CW_PI / 2, 0, hashf(i, 3, 61u) * CW_TAU });
    float sz = 2.2f + 1.6f * hashf(i, 4, 61u);
    SETV(e, "size", sz, sz, 3.0f);
    SETS(e, "texture", "decals/cracks.png");
    SETE(e, "blend", "MULTIPLY");
    SETF(e, "normal_fade", 0);
    SETF(e, "energy", 0.7f);
  }
}

/* ── per tick ───────────────────────────────────────────────────────── */

void sc_world_tick(sc_world *w, uint64_t tick, float elev) {
  if (!w->ok) return;
  float t = (float)tick;
  float night = 1.0f - cw_smoothstep(-4, 6, elev);
  for (int i = 0; i < SC_LAMPS; i++) {
    sc_lamp *l = &w->lamps[i];
    float target = elev < l->stagger ? 1.0f : 0.0f;
    float on = cw_move_toward(l->on, target, 1.0f / 30);
    if (on != l->on) {
      l->on = on;
      /* a sodium lamp warming up: the colour shifts with the level */
      float k = 10.0f * on * on;
      t3_instanced_mesh_set_color_at(w->bulbs, i, (t3_color){ 0.05f + k * 1.0f, 0.05f + k * 0.62f, 0.05f + k * 0.3f });
    }
  }
  for (int i = 0; i < SC_FIREFLIES; i++) {
    sc_firefly *f = &w->flies[i];
    float a = f->phase + t * f->speed, s, c;
    cw_sincos(a, &s, &c);
    float x = f->cx + c * f->r + 0.6f * cw_sin(a * 3.1f), z = f->cz + s * f->r;
    float y = sc_height(x, z) + f->h + 0.35f * cw_sin(t * 0.05f + f->phase);
    float blink = 0.5f + 0.5f * cw_sin(t * 0.11f + f->phase * 5);
    float e = night * blink;
    /* the glow dot exists only while the firefly shines */
    float sz = e > 0 ? 0.4f + 0.6f * e : 0;
    place(w->fly_glow, i, (t3_vec3){ x, y, z }, 0, (t3_vec3){ sz, sz, sz });
    t3_instanced_mesh_set_color_at(w->fly_glow, i, (t3_color){ 0.02f + 6 * e, 0.02f + 12 * e, 0.02f + 3 * e });
  }
  /* campfire: lit from late afternoon; flicker from two hashed noises */
  float fire = 1.0f - cw_smoothstep(8, 20, elev);
  float fl = 0.75f + 0.15f * value_noise(t * 0.21f, 0.5f, 99u) + 0.1f * value_noise(t * 0.67f, 3.5f, 98u);
  cw_node_set(w->campfire, "energy", cw_val_float(60.0f * fire * fl));
  /* particles are unlit: smoke takes the daylight's level (quantized so the
   * property is not rewritten every tick) */
  float sl = floorf((0.06f + 0.94f * (1.0f - night)) * 32 + 0.5f) / 32;
  if (sl != w->smoke_light) {
    w->smoke_light = sl;
    for (int i = 0; i < w->smoke_count; i++) cw_node_set(w->smokes[i], "energy", cw_val_float(sl));
  }
  bool burning = fire > 0.05f;
  if (cw_node_valid(w->fire) && burning != w->fire_on) {
    w->fire_on = burning;
    cw_node_set(w->fire, "emitting", cw_val_bool(burning));
    cw_node_set(w->sparks, "emitting", cw_val_bool(burning));
    cw_node_set(w->fire_smoke, "emitting", cw_val_bool(burning));
  }
  for (int i = 0; i < SC_WINDOWS; i++) {
    float on = cw_smoothstep(w->window_stagger[i] + 2, w->window_stagger[i] - 2, elev);
    t3_color c = w->window_color[i];
    float k = 0.02f + 3.5f * on;
    t3_instanced_mesh_set_color_at(w->windows, i, (t3_color){ c.r * k, c.g * k, c.b * k });
  }
  instances_changed(w->windows);
  instances_changed(w->fly_glow);
  instances_changed(w->bulbs);
}

void sc_world_lamp_lights(sc_world *w, t3_vec3 focus) {
  if (!w->ok) return;
  /* the lit lamps nearest the focus, nearest first */
  int want[SC_LAMP_LIGHTS], nwant = 0;
  float want_d[SC_LAMP_LIGHTS];
  for (int i = 0; i < SC_LAMPS; i++) {
    const sc_lamp *l = &w->lamps[i];
    if (l->on <= 0) continue;
    float dx = l->pos.x - focus.x, dy = l->pos.y - focus.y, dz = l->pos.z - focus.z;
    float d = dx * dx + dy * dy + dz * dz;
    int k = nwant < SC_LAMP_LIGHTS ? nwant++ : SC_LAMP_LIGHTS;
    while (k > 0 && want_d[k - 1] > d) {
      if (k < SC_LAMP_LIGHTS) want[k] = want[k - 1], want_d[k] = want_d[k - 1];
      k--;
    }
    if (k < SC_LAMP_LIGHTS) want[k] = i, want_d[k] = d;
  }
  /* a light keeps its lamp while the lamp is wanted (fading in), else fades
   * out and is free again; free lights take the wanted lamps without one */
  bool held[SC_LAMP_LIGHTS] = { false };
  for (int s = 0; s < SC_LAMP_LIGHTS; s++) {
    sc_lamp_light *ll = &w->lamp_lights[s];
    if (ll->lamp < 0) continue;
    int j = 0;
    while (j < nwant && want[j] != ll->lamp) j++;
    if (j < nwant) held[j] = true;
    ll->fade = cw_move_toward(ll->fade, j < nwant ? 1.0f : 0.0f, 1.0f / 15);
    if (ll->fade <= 0) ll->lamp = -1;
  }
  for (int j = 0; j < nwant; j++) {
    if (held[j]) continue;
    for (int s = 0; s < SC_LAMP_LIGHTS; s++)
      if (w->lamp_lights[s].lamp < 0) {
        w->lamp_lights[s].lamp = want[j];
        w->lamp_lights[s].fade = 0;
        cw_node3d_set_position(w->lamp_lights[s].light, w->lamps[want[j]].pos);
        break;
      }
  }
  for (int s = 0; s < SC_LAMP_LIGHTS; s++) {
    sc_lamp_light *ll = &w->lamp_lights[s];
    float on = ll->lamp >= 0 ? w->lamps[ll->lamp].on : 0;
    cw_node_set(ll->light, "energy", cw_val_float(28.0f * on * on * ll->fade));
  }
}

void sc_world_lod_counts(const sc_world *w, int counts[SC_LODS]) {
  for (int l = 0; l < SC_LODS; l++) counts[l] = 0;
  if (!w->ok) return;
  for (int i = 0; i < SC_CHUNKS * SC_CHUNKS; i++) {
    int l = w->chunks[i]->current_level;
    if (l >= 0 && l < SC_LODS) counts[l]++;
  }
}

int sc_world_lamps_on(const sc_world *w) {
  int n = 0;
  for (int i = 0; i < SC_LAMPS; i++) n += w->lamps[i].on > 0.5f;
  return n;
}
