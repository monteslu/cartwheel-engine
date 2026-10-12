# Templates

A template is a complete, runnable project skeleton in `editor/templates/<id>/`. Copy one, rename it, build it with the engine checkout, and it runs natively and as a cart from the first build.

@templates

## Start a project from a template

```sh
cp -r editor/templates/empty_2d ~/games/mygame
node tools/cwproject.mjs build ~/games/mygame all
```

Then set `name` (the display name) and `app` (a C identifier: the executable and cart name) in `~/games/mygame/project.cwproj` and build again. `template.cfg` is only the template's listing (title, description, order); a copied project ignores it and you may delete it.

`tools/cwproject.mjs build <dir> [native|cart|all]` configures the engine tree with `-DCW_PROJECT_DIR=<dir> -DCW_ENGINE_ONLY=ON` into `<dir>/.cwbuild/<target>/`. The native game lands in `.cwbuild/native/project/<app>` (with its `.cwpak`), the cart in `.cwbuild/cart/carts/<app>.wasc`, and both paths are written back into `project.cwproj` as `game_exe` and `game_cart`. The command ends with one `@result {json}` line per target and exits 0 only when every target built. `node tools/cwproject.mjs info <dir>` prints the project's keys. See [Getting started](guide:getting-started) for running the result on each host.

## What every template has

| File | Role |
|---|---|
| `project.cwproj` | `key = value` settings read by `cartwheel/project.h`: `name`, `app`, `main_scene`, `sources`, `scenes`, the input map (`input.<action> = key:space, pad:south`), physics layer names and gravity, `render.width` / `render.height` / `render.msaa` / `render.vsync`, and two export presets (`native` for linux-x64, `cart` for wasc) |
| `CMakeLists.txt` | reads `app` from `project.cwproj` and declares the game: `cw_add_app(<app> SOURCES src/*.c PACK roots.txt)`. Nothing else; the engine tree does the rest. |
| `roots.txt` | what ships: the asset packer follows scene references from these files, so only reachable assets are packed |
| `main.cwscene` | the main scene, built only from engine node types |
| `src/main.c` | the game code: `cw_main` loads the input map, applies the `render.*` settings and runs the main scene with `cw_scene_app`; `init` registers the debug fields; `fixed_update` is the game logic |
| `.gitignore` | ignores `.cwbuild/`, `.cwcache/` and `exports/` |

The smallest one, `empty_2d`, in full:

@include editor/templates/empty_2d/src/main.c

Every template exposes its state as named debug fields (`game.ticks`, `tree.nodes` and what its game counts), so the same checks work on every host and over the debug channel.

## The templates

| Template | Scenes and nodes | Game code | Debug fields |
|---|---|---|---|
| `empty_2d` | `main.cwscene`: `CanvasSettings` (320 x 180 virtual canvas, integer scaling), `Camera2D`, a `Polygon2D` marker, a `CanvasLayer` with a `Label` | spins the marker; counts presses of `ui_accept` | `game.ticks`, `game.presses`, `marker.rotation`, `tree.nodes` |
| `empty_3d` | `main.cwscene`: `WorldEnvironment`, `Camera3D`, `DirectionalLight3D`, a floor and a cube (`MeshInstance3D`) | spins the cube; counts presses of `ui_accept` | `game.ticks`, `game.presses`, `cube.rotation`, `tree.nodes` |
| `platformer_2d` | `main.cwscene`: a `StaticBody2D` ground with two shapes, three instances of `coin.cwscene` (an `Area2D` in group `coins`), a `CharacterBody2D` player with a `Camera2D`, a HUD `Label` | run and jump (`move_left`, `move_right`, `jump`), collect coins on overlap, respawn after a fall | `game.ticks`, `player.pos`, `player.on_floor`, `player.jumps`, `coins`, `player.respawns`, `tree.nodes` |
| `platformer_3d` | `main.cwscene`: a floor and a step (`StaticBody3D`), three instances of `gem.cwscene` (an `Area3D` in group `gems`), a `CharacterBody3D` player, a follow camera | run on the XZ plane and jump (`move_left`, `move_right`, `move_forward`, `move_back`, `jump`), collect gems, respawn after a fall | `game.ticks`, `player.pos`, `player.on_floor`, `player.jumps`, `gems`, `player.respawns`, `tree.nodes` |

The two platformers are finished small games to read and change; the empty templates are where tutorials start:

| Tutorial | Starts from |
|---|---|
| [Your first 2D game](tutorial:first-2d) | `empty_2d` |
| [Your first 3D game](tutorial:first-3d) | `empty_3d` |
| [Multiplayer with rollback](tutorial:multiplayer-rollback) | `empty_2d` |

The tutorial gate checks that a tutorial's `CMakeLists.txt` and `.gitignore` are still its template's, so a template change cannot silently leave a tutorial behind.

## How templates are tested

`tests/project-templates.mjs` (the `project-templates` gate) turns every template into a fresh project under `out/project-templates/`, builds it native and cart with `tools/cwproject.mjs`, and runs the same pad script for 200 frames (300 for `platformer_3d`) on every host:

- native, romdev and chromium must report the same named state exactly, and their last frames must agree within the gate's tolerance;
- wcnative (no input there) must match the frame of an input-free native run;
- each template's script must cause something: a press counted (`empty_*`), a coin collected with one jump and the player past x = 200 (`platformer_2d`), a gem collected with one jump and a respawn after running off the edge (`platformer_3d`);
- the control is the same run without the script, which must fail that check.

```sh
node tests/project-templates.mjs empty_2d --hosts native,romdev
```

## Adding a template

A new template is a directory `editor/templates/<id>/` with the files above and a `template.cfg` (`title`, `description`, `order`). Add its pad script, fields and check to the `SPEC` table in `tests/project-templates.mjs`, and a tutorial that starts from it or uses it (the docs gate fails on a template no tutorial covers).
