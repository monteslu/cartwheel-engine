# Your first 2D game

This tutorial builds Gem Garden, a small top-down game: walk a yellow ball around a walled garden, pick up four gems, stay away from the red thorns, and win. Along the way it uses the pieces almost every 2D game in {{name}} needs:

- a project made from the `empty_2d` template,
- an input map in `project.cwproj`,
- a `CharacterBody2D` player that moves with `cw_character2d_move_and_slide`,
- `StaticBody2D` walls,
- gems and thorns as small scenes of their own, an `Area2D` each, wired up through signals,
- a HUD made of `Label` nodes, and a win state,
- named debug fields, so a script can play the game and check the result on every host.

Every listing on this page is included from the finished project in `docs/tutorials/first-2d/project/`, and `node tests/tutorials.mjs first-2d` builds that project and plays it on every host on every run, so what you read here is what runs.

You need a built engine checkout first ([Getting started](guide:getting-started)). Commands run from the root of the checkout.

## 1. Start from the template

A project is a directory. The `empty_2d` template is the smallest runnable 2D one: a scene with a camera, a spinning square and a label. Copy it, and delete `template.cfg` (it only describes the template in the template list):

```sh
cp -r editor/templates/empty_2d ../gem_garden
rm ../gem_garden/template.cfg
```

Then open `../gem_garden/project.cwproj` and give the game its name. `name` is what the window title shows; `app` is a C identifier that names the executable and the cart:

@include docs/tutorials/first-2d/project/project.cwproj#name

Leave `CMakeLists.txt` alone. It reads `app` from `project.cwproj`, compiles every `src/*.c`, and packs what `roots.txt` reaches. Build and run it once to see that everything works:

```sh
node tools/cwproject.mjs build ../gem_garden all
../gem_garden/.cwbuild/native/project/gem_garden
```

`all` builds both targets into the project's own `.cwbuild/` directory: the native executable (`.cwbuild/native/project/gem_garden`, with its `gem_garden.cwpak` beside it) and the cart (`.cwbuild/cart/carts/gem_garden.wasc`). The build prints one `@result` line per target:

```text
@result {"target":"native","ok":true,"path":".cwbuild/native/project/gem_garden"}
@result {"target":"cart","ok":true,"path":".cwbuild/cart/carts/gem_garden.wasc"}
```

The first build compiles the engine for the project and takes a minute or two; later builds only compile what changed.

## 2. The input map and the physics layers

Actions are declared in `project.cwproj` and bound to keys, pad buttons and stick axes in one line each. The game asks for actions, never for keys, so the same code works with a keyboard, a pad, and the input scripts of the test hosts:

@include docs/tutorials/first-2d/project/project.cwproj#input

`axis:lx-` is the left stick pushed left, `axis:ly-` pushed up. Read the rest of the binding syntax in [Input](guide:input).

The same file names the 2D physics layers. Each collision object sits on one layer and has a mask of the layers it collides with; names keep the scenes readable:

@include docs/tutorials/first-2d/project/project.cwproj#layers

Add `gem.cwscene` and `thorn.cwscene` (made in step 4) to the `scenes` line too, so the project lists every scene it has:

@include docs/tutorials/first-2d/project/project.cwproj#scenes

## 3. The level

`main.cwscene` is the level. Keep the template's `Main`, `Settings` and `Camera` nodes: the `CanvasSettings` node gives the game a 320 x 180 virtual screen scaled up by whole pixels, and the camera looks at its centre. Change the `clear_color` to the wall colour, `color(0.4, 0.3, 0.22, 1)`, and delete the template's `Marker` and `HUD` nodes.

The walls are one `StaticBody2D` with a `CollisionShape2D` per wall and per obstacle. A static body never moves on its own; the player slides along it. Collision shapes are invisible, so two `Polygon2D` nodes draw the grass inside the walls and the block in the middle (the clear colour shows through as the walls around the grass):

@include docs/tutorials/first-2d/project/main.cwscene#Walls

A `RECT` shape (the default) is centred on its `position` and `size` is its full size, in virtual pixels.

## 4. Gems and thorns are scenes

Anything that appears more than once is its own scene, so you change it in one place. A gem is an `Area2D`: a sensor region that reports bodies entering it, with no physical response. It is in the group `gems` so the code can find every gem without knowing how many there are:

@include docs/tutorials/first-2d/project/gem.cwscene

A thorn is the same idea on the `hazards` layer, in the group `thorns`:

@include docs/tutorials/first-2d/project/thorn.cwscene

Area detection is two-sided in {{name}}: the area's mask must have the body's layer AND the body's mask must have the area's layer. The gem watches `player`, and the player (next step) watches `pickups` and `hazards`. Get one side wrong and nothing is ever detected, without an error.

The level instances both scenes. Declare them at the top of `main.cwscene`:

@include docs/tutorials/first-2d/project/main.cwscene#ext_scene

and place four gems and two thorns. An instance can override properties of its root, here the position:

@include docs/tutorials/first-2d/project/main.cwscene#Gem1..Gem4

@include docs/tutorials/first-2d/project/main.cwscene#Thorn1..Thorn2

## 5. The player

The player is a `CharacterBody2D`: a body that only moves when the game calls `cw_character2d_move_and_slide`, which moves it by its `velocity`, slides it along whatever it hits and clips the velocity. Its first `CollisionShape2D` sizes it:

@include docs/tutorials/first-2d/project/main.cwscene#Player

The code lives in `src/main.c`. Replace the template's state with the game's: counters for the debug fields, the four actions, and the start position a thorn sends the player back to:

@include docs/tutorials/first-2d/project/src/main.c#state

`init` loads the input map from `project.cwproj` and looks the actions up once:

@include docs/tutorials/first-2d/project/src/main.c#init_input

`fixed_update` runs at a fixed 60 ticks per second whatever the frame rate, so the game behaves the same on every machine. `cw_input_vector` turns the four actions into one direction, clamped to length 1 so diagonals are not faster. A top-down game has no gravity, so the velocity is just the direction times the speed:

@include docs/tutorials/first-2d/project/src/main.c#move

Build and run again: the ball walks with WASD, the arrow keys, a pad's d-pad or its left stick, and stops at the walls.

## 6. Signals: collecting gems and hitting thorns

An `Area2D` emits `body_entered` with the body that entered. `init` finds every node of the `gems` and `thorns` groups with `cw_tree_group` and connects each one's signal to a C function. The number of gems found becomes the total:

@include docs/tutorials/first-2d/project/src/main.c#init_signals

The handlers check that the body is the player (`args[0].node`). A gem counts itself, frees itself, and sets the win flag when it was the last one. `cw_node_free` is safe here: physics signals are delivered as a batch after the step, and freeing is deferred. A thorn puts the player back at the start:

@include docs/tutorials/first-2d/project/src/main.c#signals

One thorn walks up and down the bottom corridor. Its position comes from the tick counter, not from a clock, so every host computes the same thing (an `Area2D` moves with its node):

@include docs/tutorials/first-2d/project/src/main.c#patrol

## 7. The HUD and the win state

A `CanvasLayer` draws its children on top of the world, in window pixels (640 x 360 here, not the 320 x 180 virtual screen). `Gems` sits in the top-left corner. `Message` is anchored to the full width and the vertical middle, with its text centred, and starts empty:

@include docs/tutorials/first-2d/project/main.cwscene#HUD

`update_hud` writes both texts. `init` calls it once and the gem handler calls it after every gem; once `G.won` is set, `move` above stops the player:

@include docs/tutorials/first-2d/project/src/main.c#hud

## 8. Debug fields

A test should not have to read pixels to know whether the game works. `cw_debug_*` registers a variable by name; every host can read the named values while the game runs, and romdev reads them with `wasm({op:'debugState'})`. The app desc already sets `.debug = true`:

@include docs/tutorials/first-2d/project/src/main.c#init_debug

The end of `fixed_update` keeps them current:

@include docs/tutorials/first-2d/project/src/main.c#debug

`cw_main` is still the template's: it reads `render.*` from `project.cwproj` and runs the main scene:

@include docs/tutorials/first-2d/project/src/main.c#main

## 9. Run it natively and as a cart

The native build is a normal executable; run it as in step 1. The cart is the same game compiled to WebAssembly in one `.wasc` file. `tools/run-cart.mjs` runs either one for an exact number of frames on the fixed 1/60 s clock, and writes the last frame and the debug state:

```sh
node tools/run-cart.mjs --host native --app gem_garden --exe ../gem_garden/.cwbuild/native/project/gem_garden --frames 120 --shot out/gem.png --state out/gem.json
node tools/run-cart.mjs --host romdev --app gem_garden --cart ../gem_garden/.cwbuild/cart/carts/gem_garden.wasc --frames 120 --shot out/gem-romdev.png --state out/gem-romdev.json
```

The `romdev` host runs the cart in the romdev MCP server (it must be listening on 127.0.0.1:7331). `--host chromium` runs the cart in headless Chromium the same way.

## 10. Test it with an input script

An input script is a list of pad states, each held from its frame on. This one walks the garden: up, along the top past the first thorn (dipping under it), down the right side, then left along the bottom, hugging the block so the moving thorn passes below, and down into the last gem:

@include docs/tutorials/first-2d/input.json

Run it on two hosts and compare:

```sh
node tools/run-cart.mjs --host native --app gem_garden --exe ../gem_garden/.cwbuild/native/project/gem_garden --frames 520 --input docs/tutorials/first-2d/input.json --state out/gem.json --shot out/gem.png
node tools/run-cart.mjs --host romdev --app gem_garden --cart ../gem_garden/.cwbuild/cart/carts/gem_garden.wasc --frames 520 --input docs/tutorials/first-2d/input.json --state out/gem-romdev.json --shot out/gem-romdev.png
```

Both states end with the same values, bit for bit:

```text
"game.ticks":520,"player.pos":[58.81964111328125,136.5],"gems":4,"hits":0,"won":1
```

and the screenshot shows `GEMS 4/4` and `ALL GEMS! YOU WIN`. Without the script the same run ends with `"gems":0,"won":0`: a test is only worth something when it can fail.

This is exactly what the tutorial's own gate does (see [Testing](guide:testing) for the format and the other gates). `tutorial.json` says which fields must agree across hosts, what the script must cause, and that the run without input must not cause it:

@include docs/tutorials/first-2d/tutorial.json

To check a thorn, write a script that walks straight into one (`{"frame":10,"axes":{"ly":-1}}`, then `{"frame":47,"axes":{"lx":1}}`) and watch `hits` become 1 while `player.pos` restarts from the start.

## Where next

- [Your first 3D game](tutorial:first-3d) does the same in 3D, with a rigid body to push.
- [Physics](guide:physics) covers bodies, layers and signals in depth; [Scenes](guide:scenes) the scene format and instancing; [UI](guide:ui) anchors and containers.
- For a side-view game, start from the `platformer_2d` template instead: it is a finished platformer (a CharacterBody2D that runs and jumps, coins, a follow camera) built from the same pieces as this tutorial, and `tests/project-templates.mjs` builds and plays it on every host. See [Project templates](guide:templates).
