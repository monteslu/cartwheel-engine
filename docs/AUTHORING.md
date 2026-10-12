# Writing the docs

The docs site is generated from this repository by `node tools/gen-docs.mjs`
into `build/docs/` (never committed). Open `build/docs/index.html` straight
from disk or serve the directory; it works offline (search included).

```sh
node tools/gen-api.mjs              # refresh docs/api.json and docs/nodes.json
node tools/gen-docs.mjs             # build/docs/
node tools/gen-docs.mjs --check     # every include, region and link resolves; tutorials paste no code
node tests/tutorials.mjs            # build and run every tutorial's final project (native + romdev)
```

The product name lives in one place, `docs/site.json` (`"name"`): pages say
`{{name}}` where they mean it, so a rename is one edit.

## Where pages come from

| Source | Page |
|---|---|
| `docs/guides/<slug>.md` | a guide (`guides/<slug>.html`); order and sections come from `docs/site.json` |
| `docs/tutorials/<slug>/index.md` + `tutorial.json` | a tutorial (`tutorials/<slug>.html`) |
| `docs/api.json` (headers) | the C API reference, one page per header |
| `docs/nodes.json` (reflection) | the node reference, one page per node type |
| `editor/templates/*/template.cfg` | the template list (`@templates`) |

Never write the API or node reference by hand: it is generated from the
headers and the engine's reflection data. Guides explain and link to it.

## Markdown

Headings (`#` is the page title, one per page), paragraphs, `-` and `1.`
lists (nest by two spaces), fenced code, tables, `> ` notes, `**bold**`,
`*em*`, `` `code` ``, and links. Inline code that is exactly a public function
(`cw_node_get`), a node type (`CharacterBody2D`) or a header
(`cartwheel/physics2d.h`) links to its reference page automatically.

Link forms besides plain URLs and relative paths:

| Write | Links to |
|---|---|
| `[text](guide:physics)` | a guide |
| `[text](tutorial:first-2d)` | a tutorial |
| `[text](api:cw_body2d_create)` | a function, type or macro in the C API reference |
| `[text](header:physics2d.h)` | a header's reference page |
| `[text](node:CharacterBody2D)` | a node type |
| `[text](repo:samples/hello/main.c)` | a file in the repo (shown as its path; the site does not copy sources) |

`--check` fails on a link that does not resolve.

## Including real code

A code listing is never pasted. It is included from a real file by a line of
its own:

```text
@include samples/hello/main.c
@include docs/tutorials/first-2d/project/src/main.c#player
```

The path is relative to the repo root. `#name` takes one region of the file,
marked in the source by lines containing `#region name` and `#endregion`
(any comment syntax: `/* #region player */`, `// #region player`,
`; #region player`, `# #region player`). Marker lines of every region are
dropped from listings, so regions can nest and overlap.

Scene files (`.cwscene`) take no region markers: the editor saves a scene
by rewriting it from the tree (`cw_scene_save`), and comments do not
survive that. Select their sections by node instead: `#Player` takes the
node `Player` and its whole subtree (a path from the scene root such as
`#Goal/Lamp`, or a name that is unique in the file; named sections such as
`[animation name="spin"]` match by name too), `#Gem1..Gem4` takes
everything from `Gem1` through the end of `Gem4`'s subtree, and
`#ext_scene` the `[ext_scene]` lines. `--check` fails on a region marker
in a `.cwscene`.

`--check` fails when a
file or region is missing, and in tutorials it fails on any fenced block that
is not `sh` or `text` (commands and output may be written inline; code may
not).

Other directives: `@templates` (the editor templates as a table),
`@headers` (every public header with its summary), `@gates` (the gate list from
`tests/gates.json`).

## Tutorials

`docs/tutorials/<slug>/`:

- `index.md`: the tutorial. Code comes only from `@include` of files in
  `project/` (or elsewhere in the repo).
- `tutorial.json`: how the site lists it and how `tests/tutorials.mjs`
  proves it still works:

```json
{
  "title": "Your first 2D game",
  "order": 10,
  "summary": "One sentence for the index.",
  "template": "empty_2d",
  "project": "project",
  "frames": 300,
  "input": "input.json",
  "fields": ["game.ticks", "coins"],
  "expect": { "coins": 3 },
  "atLeast": { "player.jumps": 1 },
  "control": { "input": null },
  "hosts": ["native", "romdev"],
  "gate": null
}
```

- `project/`: the finished project, exactly what a reader has at the end
  (`project.cwproj`, `CMakeLists.txt`, `src/*.c`, scenes, assets). It is a
  normal project: `node tools/cwproject.mjs build docs/tutorials/<slug>/project all`.
- `template`: the editor template the tutorial starts from. The gate checks
  that the project's `CMakeLists.txt` and `.gitignore` are still the
  template's, so a template change cannot silently leave a tutorial behind.
- The gate copies `project/` to `out/tutorials/<slug>/`, builds native and
  cart, runs the input script (`tools/run-cart.mjs` format) on every host in
  `hosts`, requires `fields` to agree exactly across hosts, `expect` to match
  and `atLeast` to be reached on native, and runs the control (no input, or
  `"control": {"env": {...}}`), which must FAIL the expectations.
- `gate`: a script (`gate.mjs` in the tutorial directory) for tutorials the
  generic runner cannot drive (several processes, the editor). It gets the
  copied project directory as `argv[2]`, prints `ok ...` / `FAIL ...` lines,
  runs its own must-fail control, and exits 0 only on success.

## Style

Short sentences, present tense, no marketing. Say what a thing costs and who
owns what. No em-dashes. No commercial game names. Every claim about
behaviour should be one a reader can check in a header, a sample or a gate.
