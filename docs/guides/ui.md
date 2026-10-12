# UI

{{name}}'s UI is a retained widget system made of scene nodes: every widget is a node type extending `Control`, so menus, HUDs and whole editor layouts load from `.cwscene` files, have reflected properties and emit signals. The {{name}} editor is built with it.

## Controls are nodes

Because a widget is a node, everything the scene system does applies to it. A control lives in a tree, is saved and instanced with [scenes](guide:scenes), is inspected and edited through its reflected properties (`cw_node_set`), emits signals you connect with `cw_connect`, and is freed with its subtree. There is no separate UI document or widget handle type.

A scene run by `cw_scene_app` needs no UI code at all. `samples/ui_scene` is the whole gallery scene run by the scene runner:

@include samples/ui_scene/main.c

The first time a tree holds a control, the UI registers itself as an overlay pass of the tree's 2D draw (see [2D rendering](guide:rendering-2d)), so it composites over 3D and 2D in its own render2d pass on the topmost canvas layer. Outside the scene runner, call `cw_ui_render` from your render callback (as `samples/ui_editor` does) or `cw_ui_draw` inside your own 2D pass.

## Rects, anchors and containers

Sizes are in UI units: canvas pixels divided by the UI scale (`cw_ui_set_scale`, 1 by default, 2 for a chunky pixel-art UI, and the display density is applied on top for HiDPI). A control's rect is relative to its parent control, or to the canvas for a top-level control.

- **Under a plain `Control` or the canvas**, a child's rect comes from anchors (0..1 of the parent's size, per side) plus offsets (UI units, per side). If that rect is smaller than the child's minimum size it grows in the direction `grow_horizontal` / `grow_vertical` say. `cw_control_set_anchors_preset` covers the usual cases (corners, centre, full rect, wide edges).
- **Under a container**, the container places the child and ignores its anchors. Size flags say what the child does with the space: `FILL` takes the cross size, `EXPAND` shares extra main-axis space by stretch ratio, `SHRINK_CENTER` and `SHRINK_END` sit at minimum size.

The containers are `HBoxContainer`, `VBoxContainer`, `GridContainer`, `MarginContainer`, `CenterContainer`, `Panel`, `PanelContainer`, `ScrollContainer`, `HSplitContainer` / `VSplitContainer`, `TabContainer`, `HFlowContainer` / `VFlowContainer` and `AspectRatioContainer`. Their spacing (separation, margins) comes from theme constants, not properties, so one theme change respaces every screen.

### Layout cost

Layout has two passes. **Measure** computes each control's minimum size as the larger of its `custom_minimum_size` and its content (text plus the theme's style margins for a button, children plus separations for a box). **Arrange** goes top-down and each container fits its children. Both are cached: a property that affects size marks the control and its ancestors dirty, and only dirty subtrees are measured and re-arranged. Container positions and sizes round to whole UI units, so layouts are exact on every host.

Layout runs once per frame before input routing, and again before drawing if something changed. A steady screen does no layout work at all: the gallery scene reports `ui.measured` and `ui.arranged` of 0 after its first frame. `samples/ui_perf` holds about 5,000 controls; changing one button's text re-arranges about a hundred of them instead of all of them, and the sample times full and incremental layout as debug fields.

Drawing is O(visible controls). Styles (rounded rects, borders, shadows, all anti-aliased) are vertex-coloured triangles and text is SDF, so they batch together. The 5,000-control screen draws in 3 draw calls; the gallery in about 14. Each `clip_contents` control (scroll areas, text fields) costs one batch break.

## Widgets

The widget set covers what games and tools need: `Label` (wrap, ellipsis), `RichTextLabel` (a BBCode subset with links), `Button`, `CheckBox`, `CheckButton`, `OptionButton`, `HSlider` / `VSlider`, `SpinBox`, `ProgressBar`, `LineEdit` (selection, clipboard, undo, secret mode), `TextEdit` (multi-line, line numbers, a syntax highlighter hook with a ready C highlighter), `ItemList`, `Tree` (columns, multi-select, drag), `PopupMenu`, `PopupPanel`, `MenuBar`, `AcceptDialog` / `ConfirmationDialog`, `ColorPicker`, `TextureRect`, `NinePatchRect`, and two accessibility widgets, `CaptionPanel` and `RemapButton`. The [node reference](node:Control) lists each one's properties and signals.

In a scene file, list-like widgets take their content as text (`items = "New|1\nOpen...|2\nRecent>Recent"` for a menu with a submenu). Buttons from the gallery:

@include samples/ui_assets/scenes/gallery.cwscene#Margin/Rows/Cols/Left/Buttons

`samples/ui_gallery` loads every widget from that scene and publishes layout rects as debug fields, so its gate compares hosts on layout as well as pixels.

Drag and drop is built in. A control starts a drag with `cw_ui_drag_begin` (a kind string, a number and a text payload, and a label drawn beside the pointer), and any control becomes a drop target through `cw_ui_set_drop_target` without subclassing. `TabContainer` (with `drag_to_rearrange`) and `Tree` (with `drag_items`) start drags themselves.

Custom controls are node types with a `cw_ui_class`: callbacks for minimum size, layout, drawing (with the `cw_ui_draw_*` helpers), input, hit testing and focus. Methods left NULL inherit from the nearest base type.

## Themes

A theme holds items by theme type and name: styles (`Button.hover`), colours (`Label.font_color`), constants (`BoxContainer.separation`; font sizes are constants named `*font_size`) and fonts. Themes are text files, one item per line, which makes them easy to write and diff. This is the `ui_editor` sample's theme, the built-in dark theme with a teal accent:

@include samples/ui_assets/themes/editor.theme

A lookup tries, in order: the control's own `theme_overrides`, the nearest ancestor-or-self control's `theme`, the project theme (`cw_ui_set_theme`), then the built-in default. Inside each theme it tries the control's type, then its base types (`CheckBox` falls back to `Button`), then the theme's `extends`. The built-ins are `dark`, `light`, `high_contrast` and `colorblind`. Since every size derives from content and theme, there are no hard-coded pixel constants to chase when the font size changes.

A control can carry small overrides inline, as a string in the same format: `theme_overrides = "font_size = 24"`. `cw_theme_save` writes a theme back as text, and `cw_theme_parse` reports the line number of a bad line.

## Input routing and focus

The platform calls the UI once per frame right after the input module and before any fixed tick. The UI reads the raw input snapshot and routes it:

- **Pointer** (mouse; a touch acts as the mouse): open popups first, then the topmost control under the pointer whose `mouse_filter` is not `IGNORE`. Unhandled events bubble to the parent while the filter is `PASS`; `STOP` ends there. A press captures the pointer until every button is up, so drags keep working outside the control. In a `ScrollContainer`, a touch that moves a few units becomes a scroll and the pressed control gets a canceled release.
- **Keys and text** go to the focus owner. Unhandled Tab and Shift+Tab move focus in tree order; arrows, Enter, Space and Escape act as navigation. A focused text field starts platform text input.
- **Gamepad** navigation uses the actions `ui_up`, `ui_down`, `ui_left`, `ui_right`, `ui_accept` and `ui_cancel` (see [input](guide:input)). The UI creates them with d-pad, left stick, south and east bindings unless the game already defined them. Do not bind keys to these actions: the keyboard reaches navigation through the key path. Navigation goes to an explicit focus neighbour, or else to the nearest focusable control in that direction.

Whatever the UI does not take belongs to the game. `cw_ui_wants_pointer` and `cw_ui_wants_keyboard` say whether to ignore the pointer or keys this frame, and an unhandled-input callback sees each event that fell through. `samples/ui_hud` counts clicks on empty HUD space as game clicks:

@include samples/ui_hud/main.c#unhandled

Closing a popup returns focus to the control that had it, so keyboard and gamepad users keep their place. The `ui-mouse`, `ui-keys` and `ui-pad` gates drive `samples/ui_interact` with input scripts (clicks, drags, typing, wheel and touch scrolling, menus, dialogs, keyboard and pad focus moves) and compare every signal count across hosts.

## A game HUD

`samples/ui_hud` builds a HUD in code for a pixel-art game. The root control fills the canvas but ignores the mouse, so clicks on empty space reach the game; one theme, written in the theme text format, styles the whole subtree; a label gets an inline override.

@include samples/ui_hud/main.c#build

The game draws into a 640x360 virtual canvas with `INTEGER` scaling, and the UI draws inside that same pass with `cw_ui_draw` at UI scale 2. The HUD is 320x180 UI units, every UI pixel is a 2x2 canvas block, and the whole frame is upscaled together, so UI and game pixels match. Pointer input is mapped back through the virtual canvas, so the scripted clicks in window coordinates land on the buttons.

@include samples/ui_hud/main.c#draw

Without `cw_ui_draw`, the UI draws in its own pass at window resolution over the scaled game, which is what you want for crisp text over pixel art.

## Editor-scale layouts

The same system carries a full tool layout. `samples/ui_editor` loads a menu bar with submenus, a toolbar, nested split panes, a two-column scene `Tree`, a file list, a `TextEdit` with line numbers and C highlighting, and an inspector, about 150 controls from one `.cwscene` and the theme above. The inspector is built by reflection: for the selected node it walks `cw_node_props` and makes a header per owning node type and a row per editable property, then rebuilds when the selection changes.

@include samples/ui_editor/main.c#inspector

The real [editor](guide:editor) works the same way and is a {{name}} app, so anything it can show, a game can too.

## Localization and accessibility

Text a control shows goes through `cw_tr_auto` while `auto_translate` is on (the default): text that is a catalog key shows its translation, other text shows as written, and changing the locale re-measures every control. With a right-to-left locale, containers and anchors mirror, LEFT and RIGHT alignments swap and text shapes as an RTL paragraph; scroll, split, tab, tree, slider and text-field internals stay left to right. `layout_direction` overrides this per subtree.

For accessibility, `cw_ui_set_text_scale` multiplies every theme font size (layouts grow with the text), `cw_ui_set_reduced_motion` turns off caret blink and UI motion and is readable by the game for its own effects, and the `high_contrast` and `colorblind` themes are built in. Controls carry `accessibility_name` and `accessibility_description` for screen readers. The accessibility tree, captions and remapping are covered in [localization and accessibility](guide:l10n-a11y).

## Debugging

`cw_ui_get_stats` reports controls alive, drawn, layout passes, measured and arranged counts, draw calls, commands and open popups; `cw_ui_debug_register("ui")` publishes them as debug fields. Over the [debug channel](guide:debug-channel), `screenshot ui` captures the UI layer alone, and controls answer `props`, `get` and `set` like any node.

## Samples

| Sample | Shows |
|---|---|
| `ui_scene` | a UI scene run with no UI code |
| `ui_gallery` | every widget, dark and light themes, layout rects as debug state |
| `ui_interact` | input-script-driven tests of mouse, keys, pad, touch, menus, dialogs |
| `ui_hud` | a pixel-art HUD inside a virtual canvas, unhandled input to the game |
| `ui_editor` | an editor-shaped layout with a reflection-built inspector |
| `ui_perf` | 5,000 controls, full vs incremental layout cost |

The reference: [ui.h](header:ui.h) and the node pages from `Control` down.
