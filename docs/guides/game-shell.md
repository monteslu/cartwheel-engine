# The game shell

Every game needs the same plumbing around its play: a loading screen, a
title screen, players joining, a pause menu, settings that are saved, save
data, menu sounds and music, split screen for local multiplayer.
(header:shell.h) is all of it, built from the engine's own modules, so a game
writes only its gameplay and art:

- screens: loading, title (Play, Settings, Quit), join (one to four players:
  a pad joins with South and leaves with East, the keyboard joins with
  Enter), play, pause (Resume, Settings, Quit to title) and settings;
- settings: graphics ((header:quality.h): left and right step the preset,
  accept opens the graphics page of (header:quality_menu.h) with every
  setting, the render resolution with its pixel size, ambient occlusion,
  shadows, anti-aliasing, effects detail, Auto, and the game's own settings
  from `cw_quality_lever_add`), music and effects volume, and the controls
  (each action and its bindings, from (header:input.h)); saved with the
  game's own save data in one versioned, checksummed block;
- input: menus answer pads, the keyboard and pointer taps (mouse or touch);
  joined players are assigned their pad and devices in join order, so
  gameplay asks `cw_action_*` for players 0 to n-1;
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

A game that wants its own picture behind the title, join, settings and pause screens sets `draw_background` in `cw_shell_desc`: the shell calls it inside its own 2D pass with the canvas size, then dims the result lightly and draws the menu over it (without the hook the screens sit on a dark panel).

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
  /* HUD */
  cw_shell_draw();
}
```

In a scene app (`cw_scene_app`) the tree is paused outside play, so its
nodes hold still under the menus, and the menus tick from a hook of the
tree's own. Menus run on the fixed tick from the input snapshot, so a
replayed input script drives them like gameplay; an autopilot build or a
test starts a match directly with `cw_shell_start`.

[cartwheel-starter](https://github.com/monteslu/cartwheel-starter) is a
complete small game on the shell.
