/* check-prompts control: the forms that must pass (articles, markup, Start, keyboard keys, debug text). */
static const char *const OK[] = {
  "{jump}: hop a gap",
  "press {jump} to join",
  "{move_left|move_right} steer    {pad:west} {pad:south} {pad:east}",
  "PRESS START",
  "Start or {ui_accept}: play",
  "A scrapyard bowl: the crusher's mound in the middle.",
  "A button that opens a list of choices.",
  "A frozen lake. A broken station in orbit.",
  "Press a key...",
  "Press a button...",
  "W / S: gas",
  "Bounce light: a grid of L2 spherical-harmonic probes",
  "T%llu C%llu RB%u D%u",
  "Snapping (Y key): grid step",
  "X", "Y", "Z",
  "Score: %d   Lap %d / %d",
  "{{literal braces}} and {n} points",
  "Square one",
  "a red car, a blue lane",
  "f(x) = x * 2, sin(a) + cos(b)",
  "item(s) left: %d",
  "pos[x] (y/n) [y/n] (a, b)",
  "(ab) and (by) and (axe)",
  "grid (x)%d (y)%d",
  "[x] done, [ ] to do, [b]bold[/b]",
  "r = bitcast<f32>(a) >> 1",
};
static const char *const MEANT[] = {
  "A: the key that is literally labelled A on this keyboard", /* prompt-ok: a keyboard key, in a key-only menu */
  /* prompt-ok: the line below explains what makers call the buttons */
  "Cross, Circle, Square: what one maker calls them",
};
