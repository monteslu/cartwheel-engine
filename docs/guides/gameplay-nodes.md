# Gameplay nodes

Node types for things a game would otherwise write by hand: a countdown, a path to ride, a camera boom that does not go through walls, "is this on screen?". Sound players are in [Audio](guide:audio), the list of what stands inside an area is in [Physics](guide:physics), and save data that survives a game update is in [Saves](guide:saves).

Every one of them is deterministic: it counts fixed ticks and reads the tree, never a clock, so a replay, a rollback and every host agree.

Headers: [cartwheel/gameplay.h](header:gameplay.h), [cartwheel/curve.h](header:curve.h). Sample: `gameplay_nodes`, whose scene file uses all of them together. Gates: `gameplay-nodes` (the sample on every host, its control stops the cart, the dot and the timer and must fail) and the unit suites `unit-gameplay` and `unit-curve`.

## Timer

A [Timer](node:Timer) counts down and emits `timeout`. From the sample, a timer that rings a chime every half second through a [Trigger](node:Trigger), with no code:

@include samples/gameplay_nodes/assets/main.cwscene#Blink..Chime

| Property | Meaning |
|---|---|
| `wait_time` | seconds between timeouts (default 1) |
| `one_shot` | stop after the first timeout; the default repeats |
| `autostart` | start when the node becomes ready |
| `paused` | hold the count without stopping |
| `time_left` | read-only: seconds until the next timeout, 0 when stopped |

Connect to `timeout` in C, or let a trigger act on it. From code: `cw_timer_start` (an optional new wait time), `cw_timer_stop`, `cw_timer_is_stopped`, `cw_timer_time_left`, `cw_timer_ticks_left`; the methods `start` and `stop` do the same over the debug channel.

A timer counts **fixed ticks**. `wait_time` becomes a whole number of ticks (`wait_time / dt`, rounded to the nearest, never under one), so 0.5 s at 60 ticks a second is exactly 30 ticks on every host. It runs in the node's `fixed_update`, in tree order: started on tick T by a node that runs before it, it counts tick T; started by a later node, its first counted tick is T + 1. It follows the node's pause mode, so a paused tree holds it.

The live count is two properties, `running` and `elapsed_ticks`, which scenes do not save and tree snapshots do: a rollback restores a timer in the middle of its count and it fires on the same tick again.

## Paths

A [Path3D](node:Path3D) or [Path2D](node:Path2D) holds a curve; a [PathFollow3D](node:PathFollow3D) or [PathFollow2D](node:PathFollow2D) child sits on it at a distance from its start. Rails, patrols, moving platforms, a cutscene camera, a race line. The sample's track is a circle of four Bezier quarters with a cart that drives itself round it:

@include samples/gameplay_nodes/assets/main.cwscene#Track..Track/Cart

`points` is the curve as text: points separated by commas, each `x y z`, optionally followed by its `in` and `out` handles (offsets from the point; none makes straight segments) and a tilt in radians. A 2D point is `x y` and its handles. `closed` joins the last point to the first.

A follower's properties:

| Property | Meaning |
|---|---|
| `progress` | distance along the curve, in meters (3D) or pixels (2D) |
| `progress_ratio` | the same as 0..1 of the length |
| `loop` | wrap round the length (default), or stop at the ends |
| `speed` | units a second added to `progress` every fixed tick; 0 leaves moving it to the game |
| `h_offset`, `v_offset` | 3D: sideways and up from the curve; 2D: along it and across it |
| `rotation_mode` (3D) | `NONE`, `Y` (face along the curve, level), `XY` (also pitch), `XYZ` (also roll by the curve's tilt), `ORIENTED` (the curve's own frame, right through loops and corkscrews) |
| `rotates` (2D) | turn the node along the curve |

Setting any of them moves the node at once. With a `speed` the node also draws smoothly between ticks, and a follower with `loop` off emits `end_reached` when it arrives at an end. The sample's 2D dot patrols by turning round there:

@include samples/gameplay_nodes/main.c#signals

### The curve in C

The curve is its own type, [cw_curve](api:cw_curve), usable with no node: `cw_curve_add_point`, then ask by distance.

```c
cw_curve *c = cw_curve_new();
cw_curve_add_point(c, (t3_vec3){ 0, 0, 0 }, zero, (t3_vec3){ 0, 0, -3 }, -1);
cw_curve_add_point(c, (t3_vec3){ 8, 2, -8 }, (t3_vec3){ -3, 0, 0 }, zero, -1);
float length = cw_curve_length(c);
t3_vec3 halfway = cw_curve_sample_baked(c, length * 0.5f);
cw_curve_frame f = cw_curve_sample_frame(c, 3.0f, true);   /* position, forward, up, right */
float nearest = cw_curve_closest_offset(c, player_pos);     /* how far along is the player? */
```

Walking by distance uses the curve **baked**: cut into short straight pieces (`bake_interval` long: 0.2 m in 3D, 5 px in 2D) with the distance to each, on the first query after a change. A straight segment is one piece, so its length is exact. Each baked point carries a frame (forward and up) carried along the curve without twisting about it, so a loop or a corkscrew has a sensible up everywhere and a closed curve's frames meet at the seam; a point's tilt rolls it. `cw_curve_baked` hands the polyline over for drawing a path or building a mesh along it. A node's curve is `cw_path_curve(path)`; after changing it call `cw_path_curve_changed`.

Baking and sampling are plain float arithmetic and the engine's own trigonometry in a fixed order: the same bits natively and in a cart, so simulation may depend on a path. `unit-curve` folds every baked curve into the checksum each host must reproduce.

## On-screen notifiers

A [VisibleOnScreenNotifier3D](node:VisibleOnScreenNotifier3D) emits `screen_entered` when its box comes into view and `screen_exited` when it leaves: wake an enemy when the player can see it, free a projectile that flew off screen. [VisibleOnScreenNotifier2D](node:VisibleOnScreenNotifier2D) is the same with a rectangle.

@include samples/gameplay_nodes/assets/main.cwscene#Beacon/Seen

`center` and `size` place the box in the node's space; `on_screen` reads the answer (`cw_notifier_is_on_screen`). The test runs once per fixed tick. In 3D it is the box against the view volume of the tree's current [Camera3D](node:Camera3D) (its place, `fov`, `near`, `far` and the picture's aspect); walls in between do not count. In 2D it is the rectangle against the view the last frame drew. A hidden notifier and one that leaves the tree are off screen. `cw_view3d_sees_box` and `cw_view2d_sees_rect` are the same tests without a node.

## SpringArm3D

A [SpringArm3D](node:SpringArm3D) is a camera boom. The arm runs from the node along its +Z; every fixed tick a small sphere is swept along it through the 3D physics world, and the node's children (the camera) are put at the end of the arm or just short of the first thing in the way. So a follow camera slides in toward its character instead of showing the inside of a wall. The sample's camera rides behind the cart:

@include samples/gameplay_nodes/assets/main.cwscene#Track/Cart/Hull/Pivot..Track/Cart/Hull/Pivot/Arm/Camera

| Property | Meaning |
|---|---|
| `spring_length` | the arm's full length |
| `radius` | the swept sphere's radius (0 sweeps a ray); keep it near the camera's near plane |
| `margin` | how far short of what it hit the arm stops |
| `collision_mask` | the layers that shorten it |
| `exclude_parent` | ignore the nearest body above the arm in the tree: the character it follows |
| `extend_speed` | the arm shortens at once; it grows back at this many meters a second (0: at once) |
| `hit_length` | read-only: the length now |

Aim the arm by rotating its parent (a pivot the player's stick turns). The camera draws smoothly between ticks. There is no shake of any kind: an impact is felt through the pad (`cw_input_rumble`).
