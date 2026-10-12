# Getting started

Build the engine, run a sample on every host, and start a project of your own. Every command here was run on a Linux workstation against this repository.

## Requirements

- cmake, ninja and a C17 compiler.
- SDL2 and EGL / GLESv2 development packages (native builds).
- node (packing carts, the test hosts, the project and export tools).
- For the `romdev` host: the romdev MCP server listening on 127.0.0.1:7331. For `chromium`: Playwright's Chromium. For `wcnative`: a wasmcart-native build beside the checkout (or `CW_WCNATIVE=<path to wasmcart-run>`) and a display.

The wasi-sdk that builds carts is fetched with the other dependencies; set `WASI_SDK=<dir>` to reuse one you have.

## Fetch the dependencies and build

```sh
scripts/fetch-deps.sh
scripts/build.sh
```

`scripts/fetch-deps.sh` reads `scripts/deps.lock` and fetches every pinned dependency into `deps/`: git dependencies at a pinned commit (three.c, Box2D, Box3D and the wasmcart pieces, from their GitHub repositories), archives checked by sha256, then the patches in `patches/<dep>/` applied on top. It only fetches what is missing; `FORCE=1` refetches everything. The exporters' toolchains are large and fetched only with `--export` (see [Exporting](guide:export)).

`scripts/build.sh` configures and builds two trees with ninja:

| Tree | What it holds |
|---|---|
| `build/native` | samples in `build/native/samples/<app>`, unit suites in `build/native/tests/unit/`, tools (`cwhot`, `cwbake`) |
| `build/cart` | every app as a cart in `build/cart/carts/<app>.wasc` |

`BUILD_TYPE=Debug` and `TARGETS="native"` (or `"cart"`) narrow it. New `src/<module>/*.c` files are picked up by a glob, so re-run the script (it re-runs cmake) after adding one.

## Run a sample on every host

`tools/run-cart.mjs` runs one app for an exact number of frames on the fixed 1/60 s clock and writes the last frame and the named debug state:

```sh
node tools/run-cart.mjs --host native   --app hello --frames 60 --shot out/hello.png --state out/hello.json
node tools/run-cart.mjs --host romdev   --app hello --frames 60 --state out/hello-romdev.json
node tools/run-cart.mjs --host chromium --app hello --frames 60 --state out/hello-chromium.json
node tools/run-cart.mjs --host wcnative --app hello --frames 60 --shot out/hello-wc.png
```

Each prints one line naming what it wrote:

```text
{"host":"native","frames":60,"shot":"out/hello.png","state":"out/hello.json"}
```

and the state is the same on every host that reports it:

```text
{"cw.frame":60,"cw.tick":60,"cw.pad0.buttons":0,...,"x":0,"angle":1.4999991655349731,"ticks":60}
```

Frame N means "after N engine frames". `--input script.json` feeds an input script (the format is in the header of `tools/run-cart.mjs` and in [Testing](guide:testing)); `--exe` and `--cart` run a binary that is not under `build/`. `wcnative` takes no input and reports no state, only frames; it needs a display it is allowed to use (on a Wayland desktop, set `XAUTHORITY` to the session's Xwayland authority file if it hangs).

To run a sample by hand, start the binary: `build/native/samples/platformer2d`. The environment variables `run-cart.mjs` uses work by hand too:

| Variable | Effect (native) |
|---|---|
| `CW_FRAMES=N` | run exactly N frames on the fixed clock, then exit |
| `CW_SCREENSHOT=file.png` | save the last frame |
| `CW_DEBUG_DUMP=file.json` | write every debug field as JSON at exit |
| `CW_INPUT=file` | play an input script |
| `CW_DEBUG_CMDS="cmd1;cmd2"` | run debug-channel requests at the end of a `CW_FRAMES` run and print the replies |
| `CW_START_PAUSED=1` | pause after the first frame (step it over the debug channel) |
| `CW_SEED=n` | the seed a host would deliver (`cw_host_seed`) |
| `CW_WORKERS=n` | job threads besides the main thread |
| `CW_DEBUG=1` | print debug-level log lines |

## The cross-host gate

```sh
node tests/gate.mjs hello-still hello-input --hosts native,romdev
```

```text
ok   hello-input (native, romdev)
ok   hello-still (native, romdev)
```

Each gate runs on every host it can, compares frames and named state against the native run, and runs a control that must fail. [Testing](guide:testing) covers the fields; `scripts/ci.sh` runs everything.

## Start a project

A project is a directory with `project.cwproj`, scenes, C sources in `src/` and a `CMakeLists.txt` that only declares the app. The templates in `editor/templates/` are ready-made projects: copy one and build it with the engine checkout.

```sh
cp -r editor/templates/empty_3d ~/games/mygame
node tools/cwproject.mjs build ~/games/mygame all
```

Rename it by editing `name` and `app` in `project.cwproj` (`app` is a C identifier: it names the executable and the cart). The build configures the engine with `-DCW_PROJECT_DIR=<project>` into `<project>/.cwbuild/<target>` and ends with one line per target:

```text
@result {"target":"native","ok":true,"path":".cwbuild/native/project/my_game"}
@result {"target":"cart","ok":true,"path":".cwbuild/cart/carts/my_game.wasc"}
```

The paths are also written into `project.cwproj` as `game_exe` and `game_cart`. Run the native build directly, or through `run-cart.mjs` with `--exe` / `--cart`:

```sh
~/games/mygame/.cwbuild/native/project/my_game
CW_FRAMES=30 CW_DEBUG_CMDS='tree /root 2' ~/games/mygame/.cwbuild/native/project/my_game
node tools/run-cart.mjs --host romdev --app my_game --cart ~/games/mygame/.cwbuild/cart/carts/my_game.wasc --frames 60 --state out/mygame.json
```

`node tools/cwproject.mjs info <dir>` prints the project's keys as JSON. [Templates](guide:templates) lists what each template contains; [Your first 3D game](tutorial:first-3d) and [Your first 2D game](tutorial:first-2d) start from the empty ones.

## Next

- [The agent workflow](guide:agent-workflow): driving a running game through romdev and the debug channel.
- [Architecture](guide:architecture): the loop, the node tree, resources and the render pipeline.
- [Exporting](guide:export): shippable builds per platform.
