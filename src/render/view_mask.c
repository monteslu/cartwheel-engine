/* Per-view visibility for split screen (cw_object_set_views, cartwheel/pipeline.h): an object, and everything below
 * it, drawn only in some of the views.
 *
 * Each masked object is held (retained) in a small table sorted by address. A frame drawn view by view applies the
 * masks as visibility: before view k draws, a masked object outside view k is hidden, and after the views every one
 * gets back the visibility the game gave it. A frame drawn in one pass (src/render/pipeline.c, array_run) cannot
 * hide an object for one view of one render, so there the masks become layers: every object of a masked subtree is
 * put on the view layers (src/render/view_layers.h) of the views that draw it, which only those views' cameras see,
 * and its cast_shadow follows view 0 (the shadow maps are drawn once a frame, as view 0 left the scene in a frame
 * drawn view by view). Both are put back after the run. Render only. */
#include <stdlib.h>
#include <string.h>

#include "cartwheel/core.h"
#include "cartwheel/pipeline.h"
#include "cartwheel/scene.h"
#include "three.h"
#include "view_layers.h"
#include "view_mask.h"

typedef struct vm_entry {
  t3_object *o; /* retained */
  uint32_t views;
  bool vis; /* the game's visible, while a frame drawn view by view hides it */
} vm_entry;

typedef struct vm_saved {
  t3_object *o;
  uint32_t layers;
  bool cast;
} vm_saved;

static struct {
  vm_entry *e;
  int n, cap;
  bool vis_applied;
  vm_saved *s;
  int sn, scap;
  int views_n; /* the one-pass run's views and their cameras' layers (cw__view_masks_apply_layers) */
  uint32_t cam_layers[CW_MAX_VIEWS];
} V;

static int find(const t3_object *o, bool *found) {
  int lo = 0, hi = V.n;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if ((uintptr_t)V.e[mid].o < (uintptr_t)o) lo = mid + 1;
    else hi = mid;
  }
  *found = lo < V.n && V.e[lo].o == o;
  return lo;
}

void cw_object_set_views(t3_object *o, uint32_t views) {
  if (!o) return;
  bool found;
  int i = find(o, &found);
  if (views == CW_VIEWS_ALL) {
    if (!found) return;
    if (V.vis_applied) o->visible = V.e[i].vis;
    t3_release(o);
    memmove(&V.e[i], &V.e[i + 1], sizeof *V.e * (size_t)(V.n - i - 1));
    V.n--;
    return;
  }
  if (found) {
    V.e[i].views = views;
    return;
  }
  if (V.n == V.cap) {
    V.cap = V.cap ? V.cap * 2 : 32;
    V.e = cw_realloc(V.e, sizeof *V.e * (size_t)V.cap, CW_MEM_RENDER);
  }
  memmove(&V.e[i + 1], &V.e[i], sizeof *V.e * (size_t)(V.n - i));
  V.e[i] = (vm_entry){ t3_retain(o), views, o->visible };
  V.n++;
}

uint32_t cw_object_views(const t3_object *o) {
  bool found;
  int i = o ? find(o, &found) : 0;
  return o && found ? V.e[i].views : CW_VIEWS_ALL;
}

void cw_node3d_set_views(cw_node n, uint32_t views) { cw_object_set_views(cw_node3d_object(n), views); }

int cw__view_masks_count(void) { return V.n; }

/* the objects only this table still holds are gone from every scene: let them go (between frames) */
void cw__view_masks_frame(void) {
  int k = 0;
  for (int i = 0; i < V.n; i++) {
    if (V.e[i].o->_refs <= 1 && !V.e[i].o->parent) {
      t3_release(V.e[i].o);
      continue;
    }
    V.e[k++] = V.e[i];
  }
  V.n = k;
}

void cw__view_masks_apply_visible(int view) {
  if (!V.n) return;
  if (!V.vis_applied)
    for (int i = 0; i < V.n; i++) V.e[i].vis = V.e[i].o->visible;
  V.vis_applied = true;
  uint32_t bit = view >= 0 && view < 32 ? 1u << view : 0;
  for (int i = 0; i < V.n; i++) V.e[i].o->visible = V.e[i].vis && (V.e[i].views & bit) != 0;
}

void cw__view_masks_restore_visible(void) {
  if (!V.vis_applied) return;
  for (int i = 0; i < V.n; i++) V.e[i].o->visible = V.e[i].vis;
  V.vis_applied = false;
}

static bool has_masked_ancestor(const t3_object *o) {
  for (const t3_object *p = o->parent; p; p = p->parent) {
    bool found;
    find(p, &found);
    if (found) return true;
  }
  return false;
}

static void save(t3_object *o) {
  if (V.sn == V.scap) {
    V.scap = V.scap ? V.scap * 2 : 256;
    V.s = cw_realloc(V.s, sizeof *V.s * (size_t)V.scap, CW_MEM_RENDER);
  }
  V.s[V.sn++] = (vm_saved){ o, o->layers, o->cast_shadow };
}

static bool is_light(const t3_object *o) { return o->type >= T3_AMBIENT_LIGHT && o->type <= T3_SPOT_LIGHT; }

/* the subtree of o, drawn in views (already ANDed with every masked ancestor's) */
static const char *walk(t3_object *o, uint32_t views, int n, const uint32_t *cam_layers, bool narrow) {
  bool found;
  int i = find(o, &found);
  if (found) views &= V.e[i].views;
  uint32_t all = (1u << n) - 1;
  if (is_light(o) && (views & all) != all) return "a light drawn in only some views";
  uint32_t base = o->layers & ~CW__VIEW_LAYERS;
  if (base) { /* (engine geometry already on the view layers alone is the engine's) */
    save(o);
    uint32_t L = 0;
    for (int k = 0; k < n; k++)
      if ((views >> k & 1) && (base & cam_layers[k])) L |= cw__view_layer(k);
    o->layers = L;
    o->cast_shadow = o->cast_shadow && (views & 1) && (base & cam_layers[0]);
  } else if (narrow && (o->layers & CW__VIEW_LAYERS)) { /* already on view layers (a mask): fewer of them */
    save(o);
    uint32_t L = 0;
    for (int k = 0; k < n; k++)
      if (views >> k & 1) L |= cw__view_layer(k);
    o->layers &= L;
    o->cast_shadow = o->cast_shadow && (views & 1);
  }
  for (int c = 0; c < o->child_count; c++) {
    const char *why = walk(o->children[c], views, n, cam_layers, narrow);
    if (why) return why;
  }
  return NULL;
}

const char *cw__view_masks_apply_layers(int n, const uint32_t *cam_layers, const bool *cam_view_layer) {
  V.sn = 0;
  V.views_n = n;
  memcpy(V.cam_layers, cam_layers, sizeof *cam_layers * (size_t)n);
  if (!V.n) return NULL;
  for (int k = 0; k < n; k++)
    if (!cam_view_layer[k]) return "a view's camera without layer 0 and objects drawn in only some views";
  for (int i = 0; i < V.n; i++) {
    t3_object *o = V.e[i].o;
    if (has_masked_ancestor(o)) continue; /* drawn by its ancestor's walk */
    const char *why = walk(o, CW_VIEWS_ALL, n, cam_layers, false);
    if (why) {
      cw__view_masks_restore_layers();
      return why;
    }
  }
  return NULL;
}

bool cw__view_masks_draw_in(t3_object *o, uint32_t views) {
  if (!o || V.views_n < 2) return false;
  for (int k = 0; k < V.views_n; k++)
    if (!(V.cam_layers[k] & 1u)) return false;
  return walk(o, views, V.views_n, V.cam_layers, true) == NULL;
}

void cw__view_masks_restore_layers(void) {
  for (int i = V.sn - 1; i >= 0; i--) V.s[i].o->layers = V.s[i].layers, V.s[i].o->cast_shadow = V.s[i].cast;
  V.sn = 0;
  V.views_n = 0;
}
