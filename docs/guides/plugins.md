# Plugins

A runtime plugin is a directory of C sources a project adds to the engine without editing the {{name}} tree: node types, methods, scene-file sections, resource types, debug commands and render passes, registered through the engine's ordinary registries. The project build compiles enabled plugins into the game, natively and as a cart.

Header: [cartwheel/plugin.h](header:plugin.h). Sample: `samples/plugin_demo`, a project whose main scene uses a `Turntable` node type that only its plugin defines. Gates: `plugin-demo` (the turntable turns and reverses on every host), `unit-plugin` (version rules, refusals, withdrawal, error hints; fixtures in [tests/unit/plugin_fixtures](repo:tests/unit/plugin_fixtures)) and `plugins` ([tests/plugins.mjs](repo:tests/plugins.mjs): the plugin used from a fresh project directory, a third party's tree). This page covers the runtime side only.

## Layout and manifest

A plugin lives in `<project>/plugins/<name>/`: a manifest, `plugin.cwplugin`, plus its sources. The manifest is `key = value` lines like `project.cwproj`:

@include samples/plugin_demo/plugins/turntable/plugin.cwplugin

| Key | Meaning |
|---|---|
| `name` | a C identifier, the same as the directory |
| `version` | the plugin's own version (free text) |
| `engine_api` | the engine API it was written for, `major.minor` |
| `kind` | `runtime`, `editor` or `both` |
| `runtime_sources` | files or directories of `.c` (recursive), relative to the plugin; default `runtime/` |
| `provides` | the node types it registers, named in errors when a scene uses them and the plugin is off |
| `depends` | plugins it needs; they load first |
| `enabled` | `false` leaves it out of the build |
| `title`, `description` | display text |

[cw_plugin_manifest_parse](api:cw_plugin_manifest_parse) reads one for tools.

## The code: CW_PLUGIN

The runtime part defines one descriptor with `CW_PLUGIN(name, ...)`, whose `register_fn` registers whatever the plugin adds. `turntable` registers a node type, a method and a debug command:

@include samples/plugin_demo/plugins/turntable/runtime/turntable.c#register

The node type itself is an ordinary one ([Scenes](guide:scenes)): a data struct, reflected properties and callbacks.

@include samples/plugin_demo/plugins/turntable/runtime/turntable.c#type

Nothing in `register_fn` is plugin-specific. What a plugin can register, and where:

- node types and methods: `cw_node_type_register`, `cw_node_type_add_method` ([cartwheel/scene.h](header:scene.h));
- scene-file sections: `cw_scene_register_section` ([cartwheel/scene_file.h](header:scene_file.h));
- resource types and importers: `cw_res_register_type` ([Resources](guide:resources));
- debug-channel commands: `cw_debug_command` ([Debug channel](guide:debug-channel));
- render pipeline passes: added per tree from a node type's `enter_tree` ([cartwheel/pipeline.h](header:pipeline.h)).

An optional `unregister_fn` undoes what the engine cannot withdraw by itself; node types it can.

## How a plugin gets into the game

`cw_add_app` finds `<app source dir>/plugins/` (or the directory given with `PLUGINS <dir>`; `NO_PLUGINS` opts out). For each plugin it reads the manifest, checks the engine API and dependencies, compiles the enabled runtime sources into the app and generates the app's plugin table. A project's `CMakeLists.txt` needs nothing extra; `plugin_demo`'s is the template one:

@include samples/plugin_demo/CMakeLists.txt

At startup, before the app's `init`, the platform calls [cw_plugins_init](api:cw_plugins_init), which registers every compiled-in plugin, so scenes loaded in `init` already see their node types. A plugin that is disabled, refused or missing a dependency is **not compiled**; the table still records it and why.

## The engine API version

The plugin API is every public header in `include/cartwheel/`, versioned as `CW_ENGINE_API_MAJOR.MINOR` (currently 1.0). The minor goes up when something is added (old plugins still build and run); the major goes up when something a plugin may use changes or goes away. A plugin is accepted when its major equals the engine's and its minor is not newer. Internal headers (`src/`) are never part of it.

The check runs twice:

- at build time against the manifest's `engine_api`: a refused plugin is not compiled, and the game reports it;
- at load time against the `CW_ENGINE_API` the plugin's code was compiled with (`CW_PLUGIN` records it), which catches a stale prebuilt library.

[cw_plugin_api_compatible](api:cw_plugin_api_compatible) applies the rule and explains a refusal.

## Inspecting plugins

The `plugins` debug command lists every plugin the build knows, with its state (`loaded`, `disabled`, `refused`, `failed`, `unloaded`), node types and error, and every app publishes `plugins.loaded`, `plugins.refused` and `plugins.disabled` as debug fields. In `plugin_demo`, with the plugin's own `turntables` command and its `reverse` method:

```sh
SDL_VIDEODRIVER=offscreen CW_FRAMES=60 CW_DEBUG_CMDS='plugins;turntables;call Main/Table reverse' build/native/samples/plugin_demo/plugin_demo
```

```text
{"engine_api":"1.0","plugins":[{"name":"turntable","version":"1.0.0","state":"loaded","types":1,"provides":"Turntable","error":""}]}
{"turntables":[{"path":"/root/Main/Table","uid":7,"speed":1.5,"radius":1.4,"angle":1.5,"turns":0,"tip_speed":2.1}],"count":1}
{"called":"reverse","result":"-1.5","result_type":"float","tick":60}
```

The `unit_plugin` suite builds with fixtures in every state; its `plugins` reply shows what refusals say:

```text
{"name":"future","version":"0.1.0","state":"refused","types":0,"provides":"FutureThing","error":"plugin 'future' refused: it needs engine API 2.0 (engine_api in plugin_fixtures/future/plugin.cwplugin) and this engine provides 1.0; a different major version is incompatible (port the plugin to API 1.x)"}
{"name":"needs_future","version":"1.0.0","state":"refused","types":0,"provides":"NeedsThing","error":"plugin 'needs_future' refused: it depends on plugin 'future', which is refused"}
{"name":"off","version":"1.0.0","state":"disabled","types":0,"provides":"OffThing","error":"enabled = false in plugin_fixtures/off/plugin.cwplugin"}
```

[cw_plugin_count](api:cw_plugin_count), [cw_plugin_at](api:cw_plugin_at) and [cw_plugin_find](api:cw_plugin_find) give the same information to code.

## When a scene needs a plugin that is not there

A scene that uses a type from a disabled or refused plugin does not fail silently. Creating a node of an unknown type asks [cw_plugin_type_hint](api:cw_plugin_type_hint) which known plugin provides it, so the error names the plugin and the fix, and the node and its children are skipped while the scene file stays untouched:

```text
[error] cw_node_new: unknown node type 'OffThing': plugin 'off' provides it, but it is disabled (set enabled = true in plugins/off/plugin.cwplugin, or `ed plugin.enable off`, and rebuild)
[error] fixture.cwscene: node 'Off' (type OffThing) and its children were not created: the scene file itself is unchanged
```

This is why `provides` belongs in every manifest: it is how the engine knows which plugin to name.

## Withdrawing a plugin

[cw_plugin_unload](api:cw_plugin_unload) runs the plugin's `unregister_fn` and withdraws the node types its `register_fn` added (`cw_node_type_withdraw`). It is refused while nodes of those types still exist: free them first. Loading the same plugin again ([cw_plugin_load](api:cw_plugin_load)) re-runs `register_fn`; re-registering a node type with the same data layout swaps its code, which is also how a native hot reload of a plugin library keeps existing nodes alive. A compiled-in game normally never unloads; this is for hosts that load plugin libraries at run time.
