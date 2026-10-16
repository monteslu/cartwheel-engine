/* Audio node types: ReverbZone3D, a reverb zone (cartwheel/audio.h) placed
 * in a scene. The zone follows the node's world position (its rotation and
 * scale are not applied: boxes stay axis-aligned) and lives while the node
 * is in a tree and the audio module is running; a scene loaded before
 * cw_audio_init gets its zones on the first fixed tick after it. */
#include <stddef.h>
#include <string.h>

#include "cartwheel/audio.h"
#include "cartwheel/scene.h"

typedef struct {
  int shape;       /* CW_ZONE_BOX | CW_ZONE_SPHERE */
  t3_vec3 size;    /* box: full size */
  float radius;    /* sphere */
  float blend;
  int priority;
  float decay, damping_hz, predelay, room_size, level;
  int bus;
  /* runtime */
  int zone;        /* -1 = none yet */
  bool in_tree;
  t3_vec3 placed;  /* the centre the zone was last given */
  float made_size, made_predelay; /* fixed when the zone is made */
} zone3d;

static const char *const shape_names[] = { "BOX", "SPHERE", NULL };

static cw_reverb_zone_desc desc_of(cw_node n, const zone3d *z) {
  return (cw_reverb_zone_desc){
    .shape = z->shape == CW_ZONE_SPHERE ? CW_ZONE_SPHERE : CW_ZONE_BOX,
    .center = cw_node3d_global_position(n),
    .half_extents = { z->size.x * 0.5f, z->size.y * 0.5f, z->size.z * 0.5f },
    .radius = z->radius, .blend = z->blend, .priority = z->priority, .level = z->level, .bus = z->bus,
    .decay_s = z->decay, .hf_damp_hz = z->damping_hz, .predelay_s = z->predelay, .size = z->room_size,
  };
}

/* Make the zone when audio runs, or refresh it when the node moved. */
static void sync(cw_node n, zone3d *z, bool force) {
  if (!z->in_tree || cw_audio_sample_rate() <= 0) return;
  cw_reverb_zone_desc d = desc_of(n, z);
  if (z->zone >= 0 && (z->made_size != z->room_size || z->made_predelay != z->predelay)) {
    cw_audio_remove_reverb_zone(z->zone); /* size and pre-delay need a new reverb */
    z->zone = -1;
  }
  if (z->zone < 0) {
    z->zone = cw_audio_add_reverb_zone(&d);
    z->made_size = z->room_size;
    z->made_predelay = z->predelay;
    z->placed = d.center;
    return;
  }
  if (force || memcmp(&d.center, &z->placed, sizeof d.center)) {
    cw_audio_set_reverb_zone(z->zone, &d);
    z->placed = d.center;
  }
}

static void zone_init(cw_node n, void *data) {
  (void)n;
  zone3d *z = data;
  *z = (zone3d){ .shape = CW_ZONE_BOX, .size = { 10, 5, 10 }, .radius = 5, .blend = 2, .decay = 1.5f,
                 .damping_hz = 6000, .predelay = 0.02f, .room_size = 0.5f, .level = 1, .zone = -1 };
}

static void zone_enter(cw_node n, void *data) {
  zone3d *z = data;
  z->in_tree = true;
  sync(n, z, false);
}

static void zone_exit(cw_node n, void *data) {
  (void)n;
  zone3d *z = data;
  if (z->zone >= 0) cw_audio_remove_reverb_zone(z->zone);
  z->zone = -1;
  z->in_tree = false;
}

static void zone_fixed(cw_node n, void *data, double dt) {
  (void)dt;
  sync(n, data, false);
}

static void zone_changed(cw_node n, void *data, const cw_prop *p) {
  (void)p;
  sync(n, data, true);
}

#define ZF(field, lo, hi, docs) CW_PROP_FIELD_RANGE(zone3d, field, CW_T_FLOAT, lo, hi, docs)
static const cw_prop zone_props[] = {
  { .name = "shape", .type = CW_T_ENUM, .flags = CW_PROP_DEFAULT, .offset = (int)offsetof(zone3d, shape),
    .size = (int)sizeof(int), .enum_names = shape_names, .doc = "BOX (axis-aligned, `size`) or SPHERE (`radius`)." },
  CW_PROP_FIELD(zone3d, size, CW_T_VEC3, CW_PROP_DEFAULT, "Box: full size in world units (the node's rotation and scale are not applied)."),
  ZF(radius, 0, 1e6, "Sphere: radius in world units."),
  ZF(blend, 0, 1e6, "Units outside the shape over which the zone fades out (0 = a hard edge)."),
  CW_PROP_FIELD(zone3d, priority, CW_T_INT, CW_PROP_DEFAULT, "Where zones overlap, higher takes its share first (a cave inside a valley)."),
  ZF(decay, 0.05, 60, "Seconds for the reverb to fall 60 dB (RT60)."),
  ZF(damping_hz, 0, 24000, "High frequencies above this die faster (0 = no damping)."),
  ZF(predelay, 0, 0.5, "Seconds before the reverb starts (a new reverb is made when it changes)."),
  ZF(room_size, 0, 1, "Delay line lengths, 0 small to 1 large (a new reverb is made when it changes)."),
  ZF(level, 0, 10, "How much of the spatial voices' sends the zone takes in."),
  CW_PROP_FIELD(zone3d, bus, CW_T_INT, CW_PROP_DEFAULT, "Audio bus the reverb plays into (0 = master)."),
  CW_PROP_END,
};

static cw_node_type zone_type = {
  .name = "ReverbZone3D", .parent = "Node3D", .data_size = sizeof(zone3d), .props = zone_props,
  .doc = "A place with its own reverb: spatial voices' reverb sends feed it while the listener is inside or near "
         "(cartwheel/audio.h, reverb zones).",
  .init = zone_init, .enter_tree = zone_enter, .exit_tree = zone_exit, .fixed_update = zone_fixed,
  .prop_changed = zone_changed,
};

void cw__register_audio_types(void) { cw_node_type_register(&zone_type); }
