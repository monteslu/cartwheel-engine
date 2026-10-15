# Coming from Godot

{{name}}'s node model is shaped like Godot's on purpose, and most node names match. The big difference is who writes the game: here it is C against the public headers, with no scripting VM. This page maps Godot concepts to their equivalents and says plainly where there is none yet.

## The model

| Godot | {{name}} | Notes |
|---|---|---|
| the Godot editor | none for now | the editor is paused; scenes, projects, input maps and themes are text files edited directly |
| `Node`, `SceneTree` | `cw_node`, `cw_tree` | a node is a generational handle; a freed node's handle reads as null. Processing is tree order, depth first. See [Scenes and nodes](guide:scenes). |
| `get_node("A/B")`, `$A/B`, `%Unique` | `cw_node_get(from, "A/B")` | also `"../Sibling"`, `"/root/Level/Player"` and `"%Unique"` |
| `add_child`, `remove_child`, `reparent` | `cw_node_add_child`, `cw_node_remove_child`, `cw_node_reparent` | |
| `queue_free()` | `cw_node_free` | frees the subtree; deferred while the tree is processing |
| `PackedScene`, `.tscn` | `cw_packed_scene`, `.cwscene` | text, one property per line, only non-default values; instancing with overrides. `cw_scene_load`, `cw_scene_instance_path`, `cw_scene_instance_linked` |
| `change_scene_to_file` | `cw_scene_app_change_scene` | or the `change_scene` debug command |
| `Resource`, `load()`, `preload()` | `cw_res`, `cw_res_load` | reference counted, addressed by path or stable UID; sub-resources as `file.glb#mesh/0/1`. See [Resources](guide:resources). |
| `ResourceLoader.load_threaded_request` | `cw_res_load_async` | decodes on the job pool; finishes in request order on every host |
| signals, `connect`, `emit_signal` | `cw_connect`, `cw_emit` | flags: `CW_CONNECT_DEFERRED` (like `CONNECT_DEFERRED`), `CW_CONNECT_ONESHOT`. Scene files save named connections. |
| `call_deferred` | a deferred connection, or a [Trigger](node:Trigger) | deferred work runs at the end of the current phase, in order |
| groups, `get_tree().get_nodes_in_group` | `cw_node_add_group`, `cw_tree_group` | tree order |
| `@export var speed := 4.0` | a `cw_prop` in the type's property table (`CW_PROP_FIELD`, `CW_PROP_FIELD_RANGE`, `CW_PROP_ACCESSOR`) | reflected properties are saved in scenes, animatable, settable over the debug channel and listed in [the node reference](node:Node) |
| a custom class (`class_name`) | a registered `cw_node_type` | own data block per type, a parent type, callbacks; see [Architecture](guide:architecture) |
| methods callable by name (`call`) | `cw_node_type_add_method`, `cw_node_call` | typed signatures, checked by the debug channel |

## Scripts and the loop

| Godot | {{name}} | Notes |
|---|---|---|
| GDScript, C# | C | no scripting VM for now. Per-entity and per-frame logic is C; "when X then Y" rules a human tweaks are [triggers](guide:triggers). |
| `_ready` | the type's `ready` callback | once, after its children are ready |
| `_enter_tree`, `_exit_tree` | `enter_tree`, `exit_tree` | |
| `_physics_process(delta)` | `fixed_update(dt)` | the fixed step (default 60 Hz, `render.fixed_hz`); put simulation here |
| `_process(delta)` | `update(dt)` | once per rendered frame |
| `_input`, `_unhandled_input` | input actions polled in `fixed_update`; UI events through Control nodes | there is no per-node input callback |
| `get_tree().paused`, `process_mode` | `cw_tree_set_paused`, `cw_node_set_pause_mode` | modes `INHERIT`, `PAUSABLE`, `WHEN_PAUSED`, `ALWAYS`, `DISABLED` |
| autoloads (singletons) | none | keep global state in the app (`cw_app_desc.user`, `base.init`) or in nodes; modules keep per-tree state in tree slots (`cw_tree_slot`) |
| `RandomNumberGenerator` | `cw_rng`, `cw_tree_rng` | identical on every host; seed explicitly |
| `Engine.get_physics_frames()` | `cw_tick_index` | |
| addons, GDExtension | runtime plugins (`cartwheel/plugin.h`) | a project's `plugins/<name>/` with a `plugin.cwplugin` manifest and C sources, compiled into the game natively and as a cart |

## Input

| Godot | {{name}} |
|---|---|
| Input Map (project settings) | `input.<action> = key:space, pad:south` lines in `project.cwproj`, loaded with `cw_project_load_input` |
| `Input.is_action_pressed`, `is_action_just_pressed`, `get_action_strength` | `cw_action_pressed`, `cw_action_just_pressed`, `cw_action_strength` (player index first) |
| `InputMap.action_add_event` | `cw_action_bind`, `cw_action_bind_text`; `cw_input_save_bindings` for rebinding menus |

See [Input](guide:input).

## Physics

| Godot | {{name}} | Notes |
|---|---|---|
| `CharacterBody2D` / `3D`, `move_and_slide()` | [CharacterBody2D](node:CharacterBody2D) / [CharacterBody3D](node:CharacterBody3D), `cw_character2d_move_and_slide` / `cw_character3d_move_and_slide` | `velocity`, `on_floor`, `on_wall`, `floor_normal`, `step_height`, `ride_platforms` |
| `RigidBody2D` / `3D`, `apply_impulse` | `RigidBody2D` / `RigidBody3D`, `cw_rigid_body2d_apply_impulse` / `cw_rigid_body3d_apply_impulse` | Box2D v3 / Box3D underneath |
| `StaticBody2D` / `3D`, `AnimatableBody2D` / `3D` | `StaticBody2D` / `3D`, `KinematicBody2D` / `3D` | |
| `Area2D` / `3D` | [Area2D](node:Area2D) / `Area3D` | signals `body_entered`, `body_exited`, `area_entered`, `area_exited`; `overlap_count`, but no overlap listing yet |
| `CollisionShape2D` / `3D`, `RayCast2D` / `3D`, `ShapeCast2D` / `3D` | same names | |
| joints | `Joint2D`, `Joint3D` (`joint_type` selects the kind) | |
| collision layers and masks | `collision_layer`, `collision_mask` on every collision object; names in `layer_names_2d` / `layer_names_3d` | |
| `PhysicsDirectSpaceState.intersect_ray` | `cw_world2d_raycast`, `cw_world3d_raycast` and the batch and overlap queries | |
| `NavigationRegion3D`, `NavigationAgent3D`, `NavigationObstacle3D` | same names | `target_position`, signals `path_changed`, `target_reached`, `velocity_computed`; avoidance is ORCA as in Godot; obstacles carve the navmesh of a `dynamic` region; no 2D navigation nodes yet |
| `NavigationServer3D` map and path queries | `cw_nav_find_path`, `cw_nav_raycast`, `cw_nav_nearest`, crowds in C ([cartwheel/nav.h](header:nav.h)) | |

See [Physics](guide:physics) and [Navigation](guide:navigation).

## 2D, 3D and UI

| Godot | {{name}} | Notes |
|---|---|---|
| `Node2D`, `Sprite2D`, `AnimatedSprite2D`, `Line2D`, `Polygon2D`, `Light2D`, `LightOccluder2D`, `Camera2D`, `CanvasLayer` | same names | `Camera2D` has `zoom`, `smoothing`, `limit_*` |
| `ParallaxBackground` + `ParallaxLayer` | `ParallaxLayer` | |
| `TileMap` / `TileMapLayer` | `TileMap2D` | layers and animated tiles; no tile physics layers yet |
| `Label` in 2D space | `Label2D` | |
| project stretch settings | `CanvasSettings` | virtual size, scale mode, letterbox, clear colour |
| `Node3D`, `MeshInstance3D`, `Camera3D`, `DirectionalLight3D`, `OmniLight3D`, `SpotLight3D`, `WorldEnvironment`, `LightmapGI` | same names | three.c (three.js r186) renders: linear HDR, physical light units |
| `Environment` effects (glow, SSAO, tonemap) | `PostProcess` | one per tree |
| sky, fog | `ProceduralSky`, `HeightFog` | |
| `GPUParticles2D` / `3D`, `Decal` | `GPUParticles2D` / `GPUParticles3D`, `Decal3D`, plus `Trail3D` | |
| occlusion culling, `VisibleOnScreenNotifier` | `VisibilityManager`, `Room3D`, `Portal3D` | no per-node on-screen notifier |
| `Control` and containers, `Button`, `Label`, `RichTextLabel`, `LineEdit`, `TextEdit`, `Tree`, `ItemList`, `PopupMenu`, `TabContainer`, ... | same names | see [the node reference](node:Control) and [UI](guide:ui) |
| `Theme` resources | theme text files, `cw_theme_load`, `cw_theme_parse` | built-in dark, light, high-contrast and colour-blind themes |

## Animation, audio, networking, localization

| Godot | {{name}} | Notes |
|---|---|---|
| `AnimationPlayer` | [AnimationPlayer](node:AnimationPlayer) | value, bone, method and event tracks; signals `animation_started`, `animation_finished`, ... |
| `AnimationTree`, state machines, blend spaces | `AnimationTree` with `cw_animtree_*` graphs | built in C; no scene-file form yet |
| `Tween` | `cw_tween_new`, `cw_tween_property`, ... | a handle, not a node, like Godot 4's |
| `AudioStreamPlayer`, `2D`, `3D` | none as nodes | play sounds with `cw_audio_play` |
| `AudioServer` buses and effects | `cw_bus_create`, `cw_bus_set_volume`, `cw_bus_add_fx` | |
| `MultiplayerAPI`, `@rpc` | `cw_net_session`, `cw_net_rpc` | UDP, a relay and wasmcart peer connections; rollback with `cw_rollback_*` |
| `MultiplayerSynchronizer`, `MultiplayerSpawner` | [NetSynchronizer](node:NetSynchronizer), [NetSpawner](node:NetSpawner) | |
| `TranslationServer`, `tr()` | `cw_tr`, `cw_trn`, `cw_l10n_set_locale` | catalogs and PO files, plurals, pseudo-locales, RTL and CJK shaping |

See [Animation](guide:animation), [Audio](guide:audio), [Networking](guide:networking) and [Localization and accessibility](guide:l10n-a11y).

## Projects and shipping

| Godot | {{name}} |
|---|---|
| `project.godot` | `project.cwproj` (`key = value` lines, read with `cw_project_get`) |
| export presets, `export_presets.cfg` | `export.<preset>.<key>` lines in `project.cwproj`, run by `tools/cwexport.mjs` ([Exporting](guide:export)) |
| the remote scene tree and debugger | the debug channel: `tree`, `props`, `get`, `set`, `call`, `pause`, `step` ([The debug channel](guide:debug-channel)) |
| `user://` saves, `FileAccess` | no save-game API yet; `cartwheel/vfs.h` is the project file layer, and `cw_state_save` snapshots a tree for networking |

## Not there yet

No equivalent today: `Timer` nodes (count ticks in `fixed_update` or use a tween interval), 2D navigation nodes, `Skeleton3D` as a node (glTF bones are animated through bone tracks), `MultiMeshInstance3D`, `SubViewport` and render-to-texture nodes, `Path2D` / `Path3D` and `PathFollow`, audio player nodes, a save-game store, and any scripting language.
