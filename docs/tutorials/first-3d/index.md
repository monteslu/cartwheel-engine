# Your first 3D game

This tutorial builds Crate Push: walk a little figure across a floor, shove a crate onto a glowing pad, and win. It covers what most 3D games in {{name}} start with:

- a project made from the `empty_3d` template,
- lights, and the render pipeline's settings through a `PostProcess` node,
- a `CharacterBody3D` player moved by `cw_character3d_move_and_slide`, with a camera that follows it,
- a `RigidBody3D` crate that the player pushes (`push_mass`),
- an `Area3D` goal that reports the crate through a signal, and a win state on the HUD,
- named debug fields, so a script can play the game and check the result on every host.

Every listing is included from the finished project in `docs/tutorials/first-3d/project/`, and `node tests/tutorials.mjs first-3d` builds and plays that project on every host on every run. If you have not done [Your first 2D game](tutorial:first-2d), skim its steps 1, 8 and 10 first: making a project, debug fields and input scripts work the same way in 3D.

## 1. Start from the template

The `empty_3d` template is a camera, a sun, a floor and a spinning cube. Copy it and remove `template.cfg`, which only describes the template:

```sh
cp -r editor/templates/empty_3d ../crate_push
rm ../crate_push/template.cfg
```

Name the game in `../crate_push/project.cwproj` (`app` names the executable and the cart):

@include docs/tutorials/first-3d/project/project.cwproj#name

Build it once, both targets, and run the native build:

```sh
node tools/cwproject.mjs build ../crate_push all
../crate_push/.cwbuild/native/project/crate_push
```

## 2. Input and physics settings

Four movement actions, each on keys, the d-pad and the left stick:

@include docs/tutorials/first-3d/project/project.cwproj#input

The 3D physics layers get names, gravity is a little stronger than Earth's (it reads better in a small scene), and `substeps = 4` splits every physics step in four, which keeps stacked and pushed bodies steady:

@include docs/tutorials/first-3d/project/project.cwproj#physics

## 3. Lights and the PostProcess node

Open `main.cwscene`. Keep the root `Main` node and replace the rest; the finished scene is built up in the steps below.

Every scene renders as three.js r186 does: linear HDR with physical light units, finished by a tone curve (AgX by default) and sRGB output; a `PostProcess` node holds those settings. Its units are why the numbers look the way they do: a `DirectionalLight3D` energy is illuminance (a sun of 3 to 5 is a bright day at exposure 1), and ambient and hemisphere energies are small (0.05 to 0.3). `msaa` smooths the edges, and a little bloom makes the goal pad glow:

@include docs/tutorials/first-3d/project/main.cwscene#Env..Sun

Colours are linear values, so pick them darker than you would on a colour picker; the tone curve also desaturates bright colours. That is why the background is a deep `color(0.03, 0.08, 0.22, 1)`: it comes out as a mid sky blue on screen. The node reference for [PostProcess](node:PostProcess) lists every setting.

## 4. The camera, the floor and the crate

The camera starts above, behind and a little to the right of the player, turned toward it (`rotation` is x pitch, y yaw, in radians). The code moves it from step 6 on:

@include docs/tutorials/first-3d/project/main.cwscene#Camera

The floor is a `StaticBody3D` with a box shape and a box mesh of the same size. A shape is never drawn; a mesh never collides. Both are children of the body and share its position:

@include docs/tutorials/first-3d/project/main.cwscene#Floor

The crate is a `RigidBody3D`: the physics engine moves it, and its node follows. Its mass comes from `density` times the volume of its shapes (40 kg for a 1 m cube at density 40). It collides with the world and the player, and its mask also has `goal`, because area detection is two-sided (see step 5):

@include docs/tutorials/first-3d/project/main.cwscene#Crate

## 5. The goal

The goal is an `Area3D` with a box shape, a flat glowing cylinder to show where it is, and an `OmniLight3D` above it. An omni light's energy is luminous intensity: it falls off with the square of the distance, and `range` cuts it to zero at 6 m:

@include docs/tutorials/first-3d/project/main.cwscene#Goal

The area is on the `goal` layer and watches `crates`. An area only sees a body when the area's mask has the body's layer AND the body's mask has the area's layer, so the crate's mask lists `goal` as well. Leave either side out and the signal never fires, without an error.

## 6. The player and the follow camera

The player is a `CharacterBody3D` with a capsule shape and two spheres to look at. `push_mass = 60` makes it shove rigid bodies as if it weighed 60 kg; without it the player would stop dead at the crate:

@include docs/tutorials/first-3d/project/main.cwscene#Player

In `src/main.c`, the state holds the debug values, the actions, and two positions the code keeps between ticks:

@include docs/tutorials/first-3d/project/src/main.c#state

`fixed_update` steers the velocity toward the input direction at a fixed acceleration, adds gravity, and lets `cw_character3d_move_and_slide` do the moving, sliding and pushing. `move_forward` is toward -z, away from the camera:

@include docs/tutorials/first-3d/project/src/main.c#move

The camera eases toward a point offset from the player every tick. Easing a fraction of the remaining distance per tick gives a smooth follow with no state beyond the camera's position:

@include docs/tutorials/first-3d/project/src/main.c#camera

## 7. Winning, and a crate off the edge

`init` connects the goal's `body_entered` signal and remembers where the crate and the camera start:

@include docs/tutorials/first-3d/project/src/main.c#init

The handler checks that the body is the crate, sets the win flag, and changes three nodes by property name: the HUD text, the lamp's colour and the pad's glow, from yellow to green:

@include docs/tutorials/first-3d/project/src/main.c#goal

The floor has no walls, so the crate can be pushed off. Setting a rigid body's position teleports it; clear its velocities too, or it keeps falling from its new place:

@include docs/tutorials/first-3d/project/src/main.c#reset

The HUD is one `Label` on a `CanvasLayer`, in window pixels, over the 3D view:

@include docs/tutorials/first-3d/project/main.cwscene#HUD

## 8. Debug fields

The same pattern as in 2D: register each value by name once,

@include docs/tutorials/first-3d/project/src/main.c#init_debug

and keep them current at the end of every tick:

@include docs/tutorials/first-3d/project/src/main.c#debug

`crate.pos` is a rigid body's position after hundreds of physics steps. It is in the list of fields that must agree exactly across hosts, and it does: {{name}}'s physics is deterministic, so the native build and the cart produce the same bits.

`cw_main` is unchanged from the template:

@include docs/tutorials/first-3d/project/src/main.c#main

## 9. Test it with an input script

The script walks right until the player is behind the crate, then holds forward until the crate is on the pad:

@include docs/tutorials/first-3d/input.json

Run it natively and in romdev:

```sh
node tools/run-cart.mjs --host native --app crate_push --exe ../crate_push/.cwbuild/native/project/crate_push --frames 300 --input docs/tutorials/first-3d/input.json --state out/crate.json --shot out/crate.png
node tools/run-cart.mjs --host romdev --app crate_push --cart ../crate_push/.cwbuild/cart/carts/crate_push.wasc --frames 300 --input docs/tutorials/first-3d/input.json --state out/crate-romdev.json --shot out/crate-romdev.png
```

Both end with the crate on the pad and the same numbers:

```text
"game.ticks":300,"crate.resets":0,"won":1
```

and the screenshot shows `CRATE HOME! YOU WIN` with the pad lit green. Without the script `won` stays 0, so the check can fail. The tutorial's `tutorial.json` is that test, run on native, romdev and Chromium:

@include docs/tutorials/first-3d/tutorial.json

## Where next

- Add a second crate and a second pad, and win only when both pads hold a crate (an `Area3D` has a read-only `overlap_count`).
- [Rendering 3D](guide:rendering-3d) covers the pipeline, shadows and post effects; [Physics](guide:physics) bodies, joints and layers.
- The `platformer_3d` template is a finished 3D platformer (a capsule CharacterBody3D that runs and jumps, gems, a follow camera) to copy when you want jumping rather than pushing; `tests/project-templates.mjs` builds and plays it on every host. See [Project templates](guide:templates).
