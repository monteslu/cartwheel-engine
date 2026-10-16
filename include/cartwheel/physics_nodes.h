/* Physics nodes: the scene tree's layer over the Box2D / Box3D wrappers
 * (physics.h). The 2D and 3D sets mirror each other name for name.
 *
 * ── node types ───────────────────────────────────────────────────────────
 *   CollisionObject2D/3D  (abstract) collision_layer is an INDEX 0..31 (a
 *                         body's shapes sit on ONE layer, physics.h) while
 *                         collision_mask is a BIT SET: layer = 1, mask = 5
 *                         means "on layer 1, collides with layers 0 and 2"
 *                         (Godot uses bits for both). Prefer the name forms
 *                         collision_layer_name / collision_mask_names
 *                         ("world|enemy"), resolved against the project's
 *                         layer names (below). An unknown name is logged as
 *                         an error (cw_error): the layer stays as it was, a
 *                         mask leaves that name out.
 *   PhysicsBody2D/3D      (abstract) friction, bounce, rolling_resistance,
 *                         tangent_speed (2D conveyor, px/s) / tangent_velocity
 *                         (3D, m/s, shape-local), user_material,
 *                         contact_monitor (emit body_entered/body_exited on
 *                         contact begin/end), hit_events (emit body_hit(other,
 *                         speed) above the world's hit speed).
 *   StaticBody2D/3D       never moves by itself; moving the node teleports it.
 *   KinematicBody2D/3D    follows the node's global transform: every tick
 *                         the body is given the velocity that reaches it
 *                         (pushes rigid bodies, carries characters).
 *   RigidBody2D/3D        density, mass (0 = from density), gravity_scale,
 *                         linear_damp, angular_damp, lock_rotation, bullet,
 *                         can_sleep, freeze + freeze_mode (STATIC|KINEMATIC:
 *                         the runtime body-type switch), linear_velocity,
 *                         angular_velocity (live), sleeping (read-only).
 *                         Its transform is written into the node after every
 *                         step; setting position/rotation teleports it.
 *   CharacterBody2D/3D    velocity, floor_max_angle, floor_snap_length,
 *                         step_height, max_slides, push_mass, ride_platforms;
 *                         read-only on_floor, on_wall, on_ceiling,
 *                         floor_normal, wall_normal, platform_velocity.
 *                         Moves only through cw_character*_move_and_slide
 *                         (call it from fixed_update). Its shape (the first
 *                         CollisionShape child: capsule, circle/sphere or
 *                         rect/box) sizes the mover; a kinematic proxy body
 *                         with the same shapes sits at the mover each step so
 *                         areas, rays and other characters see it.
 *   Area2D/3D             monitoring, monitorable; sensor shapes. Signals
 *                         body_entered(node), body_exited(node),
 *                         area_entered(node), area_exited(node); read-only
 *                         overlap_count. Areas move with their node.
 *                         DETECTION IS TWO-SIDED: an area sees a body only
 *                         when the area's mask has the body's layer AND the
 *                         body's mask has the area's layer (Box2D/Box3D
 *                         filter both ways; Godot checks one side). Give a
 *                         player a mask such as "world|pickups|hazards".
 *                         A shape's `disabled` (and any shape property) may
 *                         change at run time: the body is rebuilt before the
 *                         next step and overlaps begin / end then.
 *   CollisionShape2D      shape (RECT|CIRCLE|CAPSULE|POLYGON|SEGMENT|CHAIN),
 *                         size, radius, height, points ("x y, x y, ..." local
 *                         pixels; POLYGON may be concave: it is triangulated),
 *                         loop (CHAIN), disabled. Direct child of a body or
 *                         area; its own position/rotation/scale place it.
 *                         CHAIN: one-sided segments. Open: walking the points
 *                         on screen the solid side is on your LEFT (a ground
 *                         line is listed right to left), and every authored
 *                         segment collides (Box2D's ghost end vertices are
 *                         added past the ends). loop: solid inside whatever
 *                         the winding.
 *   CollisionShape3D      shape (BOX|SPHERE|CAPSULE|CONVEX|TRIMESH), size,
 *                         radius, height, points ("x y z, ..."), mesh (path
 *                         to a MeshInstance3D or a baked CSG root, whose
 *                         geometry CONVEX/TRIMESH use when points is
 *                         empty), disabled. TRIMESH: static bodies only.
 *   Joint2D/3D            joint_type (2D: PIN HINGE SLIDER DISTANCE SPRING
 *                         WELD WHEEL MOTOR; 3D adds CONE (ball and socket
 *                         with a cone limit)), node_a, node_b (paths relative
 *                         to the joint; empty node_a = the world), anchor =
 *                         the joint's global position, axis = its local x
 *                         (2D) / z (3D: hinge axis) / x (3D slider);
 *                         collide_connected, break_force, break_torque,
 *                         limit_enabled, lower, upper, motor_enabled,
 *                         motor_speed, max_motor, stiffness (Hz), damping,
 *                         length (0 = current), cone_angle. Signal broken.
 *                         Motor/limit/spring changes apply in place; other
 *                         changes rebuild the joint.
 *   RayCast2D/3D          enabled, target_position (local), collision_mask,
 *                         collide_with_bodies, collide_with_areas,
 *                         exclude_parent; read-only colliding, collider,
 *                         collision_point, collision_normal, collision_fraction.
 *   ShapeCast2D/3D        shape (2D: CIRCLE RECT CAPSULE; 3D: SPHERE BOX
 *                         CAPSULE), radius, size, height, target_position
 *                         (zero = overlap test), max_results, the ray's
 *                         filters; read-only colliding, collision_count and
 *                         the closest hit's collider/point/normal/fraction.
 *
 * Units: 2D nodes are canvas pixels with y DOWN; the Box2D world runs in
 * meters with y up (pixels_per_meter, default 32). 3D nodes are meters.
 *
 * ── worlds and the frame order ───────────────────────────────────────────
 * One 2D and one 3D world per tree (tree slots "physics2d" / "physics3d"),
 * created when the first physics node enters. Each fixed tick:
 *   1. hook (order -100): bodies' nodes snapshot their transform (render
 *      interpolation start)
 *   2. nodes' fixed_update in tree order: scripts read last step's results,
 *      set velocities, call move_and_slide (characters move NOW against the
 *      current world), move kinematic nodes
 *   3. deferred structural changes and signals of phase 2 are applied
 *   4. hook (order 100): node -> body (new/changed shapes, teleports,
 *      kinematic targets, joints), step 2D then 3D, body -> node for rigid
 *      bodies, ray/shape casts refresh, then contact / sensor / hit / break
 *      events become signals, delivered in the library's event order; node
 *      frees and adds from those handlers are deferred until all are sent
 *   5. hook (order 200): Camera2D smoothing
 * This is Godot's order (physics_process, then the physics step), so a
 * script sees signals from the step that followed its last fixed_update.
 * Consequence: a character riding a KinematicBody platform follows it with
 * one tick of lag (it moved in step 2, the platform moves in step 4); it is
 * consistent and deterministic. Snap length + step height also carry a
 * running character across small gaps (about 1 m at default settings):
 * level designers should make gaps wider or lower snap/step on the body.
 * The world does not step while the tree is paused.
 *
 * Project layer names / settings: an optional asset `project.cwproj` with
 * lines `layer_names_2d = "world, player, coins"`, `layer_names_3d = ...`,
 * `gravity_2d = vec2(0, 980)` (px/s^2, y down), `gravity_3d = vec3(0, -9.8,
 * 0)`, `pixels_per_meter = 32`, `substeps = 4`; or cw_physics_nodes_config
 * before the first physics node enters a tree. */
#ifndef CW_PHYSICS_NODES_H
#define CW_PHYSICS_NODES_H

#include "physics.h"
#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cw_physics_nodes_config {
  cw_vec2 gravity_2d;          /* px/s^2, y down: (0, 980) */
  t3_vec3 gravity_3d;          /* (0, -9.8, 0) */
  float pixels_per_meter;      /* 32 */
  int substeps;                /* 4 */
  const char *layers_2d[CW_PHYS_LAYERS], *layers_3d[CW_PHYS_LAYERS]; /* interned names */
} cw_physics_nodes_config;
/* The current project settings (loaded from project.cwproj on first use). */
const cw_physics_nodes_config *cw_physics_nodes_get_config(void);
/* Replace them; worlds created afterwards use them. */
void cw_physics_nodes_config_set(const cw_physics_nodes_config *c);

/* The tree's worlds (created on demand). */
cw_world2d cw_physics2d_world(cw_tree *t);
/* The tree's 3D world, created (with the project config) on first call; null for a NULL tree. */
cw_world3d cw_physics3d_world(cw_tree *t);
/* The tree's pixels per meter: its 2D world's, or the config value when the
 * tree has no 2D world yet (none is created) or t is NULL. */
float cw_physics2d_ppm(cw_tree *t);

/* Characters: move by the node's `velocity` for dt (the fixed dt), slide,
 * update velocity and the floor/wall/ceiling flags. Returns true when it
 * collided with anything. */
bool cw_character2d_move_and_slide(cw_node n, float dt);
/* The 3D twin, in meters; returns on_floor || on_wall || on_ceiling. False for other node types. */
bool cw_character3d_move_and_slide(cw_node n, float dt);

/* The body behind a CollisionObject node (null before it entered a tree). */
cw_body2d cw_collision_object2d_body(cw_node n);
/* The 3D twin of cw_collision_object2d_body. */
cw_body3d cw_collision_object3d_body(cw_node n);
/* The node that owns a body (null for bodies not made by nodes). */
cw_node cw_physics2d_body_node(cw_body2d b);
/* The 3D twin. Both search every tree's world, so they work outside a tree update. */
cw_node cw_physics3d_body_node(cw_body3d b);

/* Rigid bodies (no effect on other types): forces in node units. */
void cw_rigid_body2d_apply_impulse(cw_node n, cw_vec2 impulse_px);
/* Impulse (N*s) at the center of mass of a RigidBody3D node; ignored for other nodes. */
void cw_rigid_body3d_apply_impulse(cw_node n, t3_vec3 impulse);

/* Re-run a cast now (normally refreshed after each step). */
void cw_raycast2d_force_update(cw_node n);
/* The 3D twin, for RayCast3D and ShapeCast3D nodes; no effect before the world exists. */
void cw_raycast3d_force_update(cw_node n);

#ifdef __cplusplus
}
#endif
#endif
