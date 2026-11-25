# The game shell

Every game needs the same plumbing around its play: a loading screen, a
title screen, players joining, a pause menu, settings that are saved, save
data, menu sounds and music, split screen for local multiplayer.
(header:shell.h) is all of it, built from the engine's own modules, so a game
writes only its gameplay and art:

- screens: loading, title (Play, Settings, Quit; South or Start on the
  highlighted item), join (one to four players: a pad joins with South and
  leaves with East, the keyboard joins with Enter; Start on any pad plays, see
  below), play, pause (Resume, up to two items of the game's own, Settings,
  Quit to title; see below) and settings;
- settings: graphics ((header:quality.h): left and right step the preset
  through Very Low, Low, Medium, High, Ultra and Auto, accept opens the
  graphics page of (header:quality_menu.h) with every setting, the render
  resolution with its pixel size (the player's own: a preset never changes
  it), ambient occlusion, shadows, anti-aliasing, effects detail, and the
  game's own settings from `cw_quality_lever_add`), music and effects volume, Text size (100%, 125%, 150%, 200%: every piece of 2D text, the
  game's HUD included, see [localization and accessibility](guide:l10n-a11y)), Text
  background (Off, Outline, Panel: contrast for text over the scene) and the controls (each action and its bindings, from
  (header:input.h)), then up to eight rows of the game's own (see below); saved with the game's own save data in one versioned,
  checksummed block;
- no screen shake: no camera shakes, at any setting, so there is no Screen
  shake row. A big landing, a boss hit or an explosion is felt in the pads
  (`cw_input_rumble`, the explosion kit's `cw_explosion_shake` as its
  strength) and seen in the effects. `cw_shell_screen_shake()` is deprecated
  and returns 0; `cw_shell_shake_level` and `cw_shell_set_shake_level` only
  keep the level an older save stored, so a save keeps its layout;
- input: menus answer pads, the keyboard and pointer taps: a mouse click, or a
  finger on a touch screen (any of wasmcart's pointer slots 1 to 9, whether or
  not the app sets `cw_app_desc.touch_pointer`). A tap on an item of the title,
  pause or settings page (and on the graphics page) chooses it; a tap on the
  join screen seats the keyboard's player, who holds the mouse and touch, and
  plays (in a match, goes back to it), and with no pad seated the screen says
  "tap to play". Gates `shell-touch`, `shell-touch-pause` and `shell-mouse`
  hold it natively and through romdev on GL and WebGPU, each with taps in an
  empty corner as the control;
  joined players are assigned their pad and devices in join order, so
  gameplay asks `cw_action_*` for players 0 to n-1;
- prompts: every hint the shell draws is a button prompt ((header:prompt.h))
  for the devices the players hold: the join screen's empty slots show
  `{ui_accept} join` for every seat and its footer `{ui_accept} join
  {ui_back} leave {ui_start} play`, and the title, pause and settings pages
  end with a line of their controls. The shell's actions are `ui_up`,
  `ui_down`, `ui_left`, `ui_right`, `ui_accept`, `ui_back`, `pause` and
  `ui_start` (Start on a pad, Enter on the keyboard), so a game's own screens
  say the same thing the same way: `cw_prompt_text(font, "{ui_back} back",
  -1, pos, size, color, CW_ALIGN_CENTER)`. On a touch screen the lines are left
  out: the items are tapped;
- audio: the shell starts the mixer and makes two buses, music and effects,
  which follow the volume settings; its hooks play the menu sounds and each
  screen's music;
- split screen: `cw_shell_split` gives each player a view with the pipeline's
  layout (two side by side, three or four in quadrants, three leaving a
  quadrant for a shared HUD), and `cw_shell_player_rect` and
  `cw_shell_shared_rect` tell the HUD where to draw;
- shared screen: a scrolling brawler or a co-op platformer has several players in
  one view. Set `shared_screen` in `cw_shell_desc`: `cw_shell_split` then keeps the
  single camera, `cw_shell_player_rect` gives every player the whole canvas and
  `cw_shell_shared_rect` is empty. The game draws one HUD with a card per player.
  The `unit-shell` gate checks both layouts.

Start plays from any pad, so one player can press Start, Start from the title and be playing. A pad with no seat takes the first free seat as it presses Start; with every seat taken the seated players play. Enter does the same for a keyboard that holds any seat, or when every seat is taken by others. South still only joins and East still only leaves. The press is answered on its own frame: the select sound, and "Starting..." over the screen. The `start` hook then runs on the next tick after that picture was drawn, so a game that loads for a long time in `start` never looks dead. Presses on this screen are counted per tick: a frame that runs no tick (a display faster than the tick rate) keeps them for the next tick, and a frame that runs several sees each once. A test or autopilot calling `cw_shell_start` gets the hook at once, as before.

The shell's own text meets the body text floor at 100%, and every gap in its screens comes from the size its text draws at, so at 200% nothing overlaps: a menu that no longer fits scrolls with its selection (a mark above or below says there is more), a hint line too wide for the canvas breaks into lines, and the controls list moves into the menu's place when there is no room beside it. A game with its own menus offers the same settings with `cw_text_scale_cycle` / `cw_text_scale_label` and `cw_text_backdrop_cycle` / `cw_text_backdrop_label`, saving `cw_text_scale_saved()` (one byte, both settings) and calling `cw_text_scale_restore` at init.

A game that wants its own picture behind the title, join, settings and pause screens sets `draw_background` in `cw_shell_desc`: the shell calls it inside its own 2D pass with the canvas size, then dims the result lightly and draws the menu over it (without the hook the screens sit on a dark panel).

The selected row of every shell list (the title, the pause menu, Settings, the Graphics page, and the join screen's taken seats) shows in the style `cw_shell_desc.selection` names, in the desc's `accent`: `CW_SHELL_SELECT_COLOUR` (the default) draws its text in the accent; `CW_SHELL_SELECT_MARKER` adds a caret before it; `CW_SHELL_SELECT_UNDERLINE` a bar under it; `CW_SHELL_SELECT_INVERSE` draws the row on a plate of the accent with its text in `selection_text` (by default the menus' dark background colour). A game whose picture has two colours only (a one-bit look, [Looks](rendering-3d.md#looks-restyling-the-picture)) shows the selection by shape: an accent of its light colour with MARKER, UNDERLINE or INVERSE. `cw_shell_set_selection(style)` changes it from the next frame (when the game switches looks), `cw_shell_selection_style()` reads it, and the debug field `shell.selection` publishes it. The `shell_settings` and `shell_join` samples take `SHELL_SELECTION=0..3`; the gates are `shell-selection` and `shell-select-inverse`.

A game whose whole picture is two colours (a one-bit dither, [Looks](rendering-3d.md#looks-restyling-the-picture)) gets menus in the same two with `cw_shell_desc.two_colour` and its `ink` and `paper`: every pixel the shell draws on the loading, title, pause, Settings, Graphics and join screens is one of them, opaque. Text, marks, frames and the hint row's button prompts are ink; text has no blended pixel at its edges (each pixel is ink when the glyph covers half of it or more, which keeps every stroke at every text size) and a thin paper outline instead of the dark one; no screen is dimmed with a see-through layer. Without `draw_background` the screens are a sheet of paper; over a picture (the game's `draw_background`, or the pause menu over play) the picture stays as it is and each line of text sits on a paper plate. The selection shows by shape in ink: `CW_SHELL_SELECT_COLOUR` cannot show and draws as INVERSE (an ink plate, its text in paper), MARKER and UNDERLINE draw their mark in ink; the join screen's seats are paper in an ink frame, a taken seat's frame heavier (all ink with INVERSE). `accent` and `selection_text` are not used in this mode. When the player changes the look in the game's own Settings (another palette, or the dither off), `cw_shell_set_colours(accent, selection_text, ink, paper)` and `cw_shell_set_two_colour(on)` change the menus from the next frame they draw; a colour with alpha 0 takes its default (ink white, paper the menus' dark 0.02 0.02 0.05). The debug fields `shell.two_colour`, `shell.ink` and `shell.paper` publish them. A game's own Graphics page drawn with `cw_quality_menu_draw` takes the same three fields in `cw_quality_menu_style`. The `shell_settings` and `shell_join` samples take `SHELL_TWO_COLOUR=1` (with `SHELL_INK`, `SHELL_PAPER`, `SHELL_TWO_BG=1` and `SHELL_SWITCH_AT`, `samples/common/shell_two_colour.h`); the gate is `shell-two-colour`. The game's own HUD gets the same edges with [cw_r2d_set_one_bit](api:cw_r2d_set_one_bit), [cw_r2d_set_one_bit_paper](api:cw_r2d_set_one_bit_paper) and [cw_prompt_set_two_colour](api:cw_prompt_set_two_colour) ([2D rendering](guide:rendering-2d), "One-bit edges").

A pad that appears in the middle of a match, or a player who wants out, does not need a restart: `cw_shell_join_open()` shows the join screen over the running match (play pauses as it does for the pause menu) with the current players seated. A pad presses A to take a free seat, a seated pad presses B to leave, Start or Enter confirms and a pad with no seat that presses Start takes a free seat as it does (one player always stays) and play resumes after the `players_changed` hook of `cw_shell_desc` told the game its new roster; B on a pad without a seat cancels. Seats keep their numbers (a player who stays is still player 2 after player 1 left, and the shell reports it as an empty seat 1: `cw_shell_seat_taken(1)` is false, its actions read nothing); the player count is the highest seat held plus one. `cw_shell_join_requested()` is true on the tick a pad that has no seat presses A or Start: call it between rounds and open the screen when it is true. For the quickest join, `cw_shell_join_press()` seats a pad that has no seat the moment it presses A (no screen, one press; it takes the first free seat and `players_changed` fires), and returns true when it did; the keyboard is left out because it already drives a player. The `shell_join` sample shows all three.

The join screen draws one box per seat (`max_players` of them), each a quarter of the canvas wide, in a row centred under the "Join" heading: four seats fill the width, two or three sit in the middle. `cw_shell_join_seat_rect(seat)` returns a seat's box in 2D pass units, so a game that draws its own art behind the menus (`draw_background`) can put it where the seats are instead of guessing. Run the `shell_join` sample with `SHELL_JOIN_SEATS=2` (or 1 to 4) to see another seat count.

**CPU seats.** A game with computer players sets `cpu_seats = true` in `cw_shell_desc` and the join screen offers each seat to a CPU at Easy, Normal or Hard. Left and right (`ui_left`, `ui_right`) move a frame from seat to seat, up and down (`ui_up`, `ui_down`) step the framed seat through empty, CPU Easy, Normal, Hard and round; a seat a person holds stays theirs. On a touch screen or with a mouse a tap on a seat no person holds steps it the same way, and a tap anywhere else plays as before. A person who joins takes an empty seat, else the first CPU's, so a player arriving late replaces a CPU. The CPUs set are saved with the settings and are back on their seats the next time the join screen opens.

In the match a CPU seat is a player like the others: it counts in `cw_shell_players()` and `cw_shell_seat_taken()`, seats keep their order (people and CPUs both close gaps the same way), and it has no device. [cw_shell_seat_cpu](api:cw_shell_seat_cpu) says whether a seat is a CPU and at what level ([cw_shell_cpu_level_label](api:cw_shell_cpu_level_label) names it); read it in `start` and in `players_changed`, since a person may take a CPU's seat during play (`cw_shell_join_press` does when every seat is taken). The game drives each CPU seat with [cw_input_set_actions](api:cw_input_set_actions) in `fixed_update` ([Input](guide:input)), which puts its moves in the recording and the input hash. What a level means is the game's own: seeded mistakes and slower reactions for Easy are the usual reading.

@include samples/hex_board/main.c#shell

@include samples/hex_board/main.c#start

[cw_shell_start_seats](api:cw_shell_start_seats) starts a match with seats set (people on pads in seat order, `CW_SHELL_SEAT_HOT` hot seats, CPUs at their levels), for an autopilot build or a test; `hex_board` takes `HEX_BOARD_SEATS=-1,0,1,2`, read in `menu_update` (the pause rule, below). Debug fields: `shell.seat_cpu` (eight, -1 or the level), `shell.join_kind` (the join screen's: 0 none, 1 a hot seat, 2 + level a CPU) and `shell.join_cursor`. The gates are `shell-cpu-seats` (the pad alone sets two CPUs, plays, quits and finds them saved on the join screen; its control presses no up) and `shell-cpu-seats-touch` (taps alone; its control taps no seat).

**Hot seats.** A turn-based game for more people than pads sets `hot_seats = true`: up to `CW_SHELL_MAX_SEATS` (eight) seats, each a person on a pad of their own, a hot seat or (with `cpu_seats`) a CPU. On the join screen up and down step the framed seat through empty, a hot seat ("pass the pad"), then the CPU levels; `seat_names` and `seat_colors` in `cw_shell_desc` name and colour each seat (Red, Blue, ...), and more than four seats draw in two rows. Start plays without seating the pad that pressed it when hot seats are set, since the free pads drive them. In the match the game says whose turn it is with [cw_shell_set_turn](api:cw_shell_set_turn) (in `start` and whenever the turn passes): the pads no seat holds, and the keyboard, mouse and touch unless a seat holds them, drive that hot seat ([cw_input_assign_pads](api:cw_input_assign_pads)) and no other hot seat reads anything, so one pad passed round plays all eight and the rules still read each seat with `cw_action_*`. A button still held when the turn passes is not the next seat's press. On a CPU's turn or a pad seat's turn the hot seats read nothing; Start on a free pad opens the pause menu on any turn, and under the menus the free pads drive them. [cw_shell_seat_hot](api:cw_shell_seat_hot) says which seats are hot. A match of more than four seats is one shared view. The wasmcart pad interface is unchanged: these are seats over the same four pads. Debug fields `shell.seat_hot` and `shell.turn`. `hex_board` plays eight hot seats with `HEX_BOARD_SEATS=-2,-2,-2,-2,-2,-2,-2,-2`; the gates are `hex-board-hotseat` (sixteen presses on one pad place two stones for every seat in turn, no seat hears a press out of turn, and the match replays exactly; control: `CW_TEST_TURN_WRONG_SEAT` hands the pad to the wrong seat) and `shell-hot-seats` (eight seats set up from one pad).


The calls a game makes:

```c
cw_app_desc cw_main(int argc, char **argv) {
  static cw_app_desc app = { .init = init, .fixed_update = fixed_update, .render = render };
  app.save = cw_shell_save_region(); /* the settings and the game's save data */
  app.save_size = cw_shell_save_region_size();
  return cw_scene_app("main.cwscene", &app);
}
static void init(void *user) {
  cw_shell_init(&(cw_shell_desc){ .title = "My Game", .max_players = 4,
                                  .game_save_size = sizeof(my_save), .start = start_match });
}
static void fixed_update(void *user, double dt) {
  if (!cw_shell_update()) return; /* a menu is up */
  /* gameplay */
}
static void render(void *user, double alpha) {
  /* the frame */
  if (cw_shell_hud_shown()) draw_hud(); /* the HUD, only in play */
  cw_shell_draw();
}
```

The game's own save data rides in the same block as the settings. A struct that grows at its end keeps old saves; a changed layout takes `game_save_version` and `game_save_migrate`; `game_save_slots` gives named slots instead. See [Saves](guide:saves).

A game can put up to two items of its own in the pause menu, such as restarting the level, picking another song or changing car. Name them in `pause_items` of `cw_shell_desc` (a NULL or empty label leaves its slot out) and set the `pause_item` hook: the items show after Resume and before Settings, drawn, highlighted, tapped and clicked like the shell's own, under the same hint line. Choosing one does what Resume does (the Back sound, the shell is in play again, and the press stays the menu's: the game's first tick after it sees no new press), then calls `pause_item(index, user)` with the item's index in `pause_items` (0 or 1). The game switches its own screen from there. Back and Start still resume. A game that names no items gets the shell's three, the same pixels as before.

```c
static void pause_item(int index, void *user) {
  if (index == 0) restart_level();
  else open_song_list();
}
cw_shell_init(&(cw_shell_desc){ .title = "My Game", .start = start,
                                .pause_items = { "Restart level", "Change song" }, .pause_item = pause_item });
```

The shell reads the labels when the pause menu opens. `cw_shell_set_pause_items` changes them (or adds or removes one) from the next pause, say a "Restart level" offered only inside a level; a label can also point at a buffer of the game's whose text it rewrites between pauses. The strings must stay valid while the menu is open. The `shell_pause` sample has two items, and renames one each time it is chosen; gates `shell-pause-items`, `shell-pause-items-wired`, `shell-pause-leak`, `shell-pause-none` and `shell-pause-touch` check the order, the index, the return to play, that the choosing press never reaches the game, that a game without items keeps the three-item menu, and touch.

The title takes items of the game's own the same way: up to two in `title_items` (Credits, How to play), between Play and Settings, drawn, highlighted, tapped and clicked like the shell's. Choosing one opens a page of the game's: the screen is `CW_SHELL_PAGE` (`cw_shell_page()` says which item, the debug field `shell.page` too), the shell calls `title_item(index, user)`, then each frame draws `draw_background` (or its dark panel), the game's `draw_page(index, canvas, user)` and a hint to go back. Back, accept or a tap anywhere goes back to the title with the item still selected. A page that wants more than one view reads `ui_left` and `ui_right`, which the shell leaves alone, in `menu_update` (below). With no `draw_page` the page shows the item's label. `cw_shell_set_title_items` changes the items from the next frame the title draws. A game that names none keeps Play, Settings, Quit, the same pixels as before.

```c
static void draw_page(int index, cw_vec2 canvas, void *user) {
  cw_r2d_text(font, index == 0 ? "Credits" : "How to play", cw_v2(canvas.x * 0.5f, canvas.y * 0.2f),
              &(cw_text_style){ .size = canvas.y * 0.08f, .color = CW_WHITE, .align = CW_ALIGN_CENTER });
}
cw_shell_init(&(cw_shell_desc){ .title = "My Game", .start = start,
                                .title_items = { "Credits", "How to play" }, .draw_page = draw_page });
```

The `board_kit` sample has both items; gate `shell-title-items` opens Credits from the pad and comes back (its control sets no items, so the same presses open Settings).

A game can add rows of its own to the Settings screen, such as a visual style, hit sounds or a difficulty: up to `CW_SHELL_SETTINGS_MAX` (eight) `cw_shell_setting` rows in `settings_items` of `cw_shell_desc`, with their count in `settings_item_count`. Each row has a `label`, the `choices` it shows (`count` of them), `value` (a pointer to the game's own int, the chosen index; what it holds at `cw_shell_init` is the default) and a `key` it is saved under. The rows show after the shell's own (after Controls, before Back), on the page opened from the title and from the pause menu, drawn, highlighted, scrolled and sized with the text like the shell's rows, under the same hint line. They change like the shell's Music, Text size and Text background rows: left and right step the choice and stop at the ends, accept (South, Enter, a tap or a click on the row) steps it forward and goes round. Each change sets `*value` and then calls `settings_changed(index, value, user)` with the row's index in `settings_items`; a step at an end that changes nothing calls nothing.

```c
static int style = 0; /* the default: All */
static const char *const STYLES[] = { "All", "Fixed: Neon", "Fixed: Paper" };
static const cw_shell_setting ROWS[] = {
  { .label = "Visual style", .key = "style", .value = &style, .choices = STYLES, .count = 3 },
};
static void settings_changed(int index, int value, void *user) {
  if (index == 0) apply_style(value);
}
cw_shell_init(&(cw_shell_desc){ .title = "My Game", .start = start, .settings_items = ROWS, .settings_item_count = 1,
                                .settings_changed = settings_changed });
/* style holds the player's saved choice from here on */
```

The rows are saved with the shell's own settings: `cw_shell_save_commit` (which every change on the Settings screen runs) writes each row with a key as a slot named `shell:<key>` of a save store ((header:save.h)), and `cw_shell_init` reads them back into the ints before the game's first frame. A game with `game_save_slots` has them in its own store (`cw_shell_save_store`, where it sees them among its slots); a game with a save struct has them in a store of the shell's in the last `CW_SHELL_SETTINGS_SAVE_BYTES` (1024) of the save area, so with saved rows the struct can be up to `CW_SHELL_GAME_SAVE_MAX - CW_SHELL_SETTINGS_SAVE_BYTES` (5120) bytes; a larger one is told so and its rows are not saved. A struct that changes size or version keeps them. A slot holds the chosen index and the choice's label, and loading takes the choice with that label, else the index, else the default:

- a new row starts at its default;
- a removed row is no longer read; its slot stays, so it comes back if the row does;
- choices added, removed or moved keep the player's pick by its label; renamed choices (another language) keep it by its index;
- a saved index past the choices, with no label that matches, gives the default (logged);
- a value the game sets itself is saved at the next `cw_shell_save_commit`.

The shell copies the rows at init, so the strings and the ints must outlive it. `cw_shell_set_settings_items` replaces them (rename a row, translate its choices, show a row only in some modes): the page shows the new rows from its next frame, a row whose key is new loads its saved choice, and the others keep their ints. A game with no rows gets the shell's Settings page, the same pixels as before. The `shell_settings` sample has three rows; gates `shell-settings-rows`, `shell-settings-change`, `shell-settings-pause`, `shell-settings-none`, `shell-settings-touch` and `shell-settings-save` check the order, the index and value given to the hook, the path from the pause menu, the page without rows, a tap, and a restart through the save (with an out-of-range save falling back), and `unit-save` the migrations.

The game's HUD goes away under the shell's menus: `cw_shell_hud_shown()` is true only in play, and false under the pause menu, the settings opened from it, the join screen opened during play and on the title and loading screens. The menus are drawn over the game's frame and dim it only lightly, so a HUD left on screen sits under the menu's lines, and at a large text size (150% and 200%) they overlap. Keep drawing the world and the views; leave out the HUD, the hint lines and the banners.

The pause rule: in a scene app (`cw_scene_app`) the tree is paused on every
screen but play (loading, title, join, a title page, pause, settings and the
"Starting..." answer), so its nodes hold still under the menus. A paused
tree runs no process or physics of its nodes (but those whose pause mode
runs while paused) and does not call the app's `fixed_update`: in a scene
app, code in `fixed_update` runs only in play. The menus tick from a hook
of the tree's own instead. Anything that must run while a menu shows goes in
`cw_shell_desc.menu_update`, which the shell calls on every tick the menus
own, in a scene app and a plain app alike, with the screen showing. Above
all that is a test's or an autopilot's start: `cw_shell_start` or
`cw_shell_start_seats` from a `cw_test_env` switch, which in a scene
app's `fixed_update` would wait at the title forever. The match then
starts on that tick and gameplay runs from it on. A plain app (no scene
tree) keeps calling its `fixed_update` on every screen, where
`cw_shell_update` returns false while a menu owns the tick.

@include samples/board_kit/main.c#menu_update

Menus run on the fixed tick from the input snapshot, so a replayed input
script drives them like gameplay. Gate `board-kit-test-start` starts
`board_kit` (a scene app) from `menu_update`; its control makes the same
start from `fixed_update`, and the match never starts.

[cartwheel-starter](https://github.com/monteslu/cartwheel-starter) is a
complete small game on the shell.
