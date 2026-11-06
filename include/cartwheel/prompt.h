/* cartwheel prompts: how to show a player which button, key or touch control
 * does something, drawn for the device that player is using right now.
 *
 * A game never writes a button's name or colour. It names the ACTION, and the
 * engine draws that action's binding on the player's current device:
 *
 *   cw_prompt_text(font, "{jump}: hop a gap", 0, pos, 24, white, CW_ALIGN_LEFT);
 *
 * On a pad that is a diamond of four dots with the south one solid; on a
 * keyboard a keycap reading "Space"; on a phone the on-screen button's disc
 * and label. Rebind jump to the north button and the north dot lights, the
 * same frame. One string covers every device and every rebinding, and it sits
 * in a translation catalog like any other text.
 *
 * ── The glyphs ─────────────────────────────────────────────────────────
 * Vector shapes from render2d (circles, rings, rounded rects, polygons), so
 * they are sharp at any size and identical on GL and WebGPU. Solid means
 * "this one", an outline means "not this one"; nothing depends on colour,
 * and no glyph carries a maker's letter, symbol or colour.
 *   face buttons     a diamond of four dots: the one to press solid, three rings
 *   shoulders        two flat bars side by side, the left or right one solid
 *   triggers         two tall tabs side by side, the left or right one solid
 *   sticks           a thumbstick from the front and a little above (a flat
 *                    socket, a short post, a wide cap), the other stick smaller
 *                    and dimmer beside it: the left stick has its twin on its
 *                    right. Arrows beside the cap say left and right, above it
 *                    up, under the socket down; a pair or four draw them all
 *   stick press      the stick with its cap pushed down under an arrow
 *   d-pad            a plus: the arms to press solid, the rest outlined (a
 *                    pair lights two, four directions light all four)
 *   Start            the word "Start" in a pill (every pad has that button;
 *                    a keyboard binding shows its keycap, usually "Enter")
 *   Back             two small pills side by side, the left one solid (the
 *                    makers name that button differently, so no word is used)
 *   keyboard         a keycap holding cw_binding_display_name's text
 *                    ("Space", "Enter", "W"; one character larger and heavier
 *                    than a word); the arrow keys draw an arrow
 *   mouse            a mouse outline with the bound button solid; the wheel
 *                    draws its arrow
 *   touch            the on-screen control as the touch overlay draws it: a
 *                    button's disc and label, a thumbstick's knob on its
 *                    filled base with the arrows inside it
 * Each shape has a dark edge under a light (or `color`) fill, so a glyph reads
 * on a light background and on a dark one. MINIMUM SIZE: 1.5% of the canvas
 * height (16 px at 1080 lines), read from native-scale crops on light and
 * dark backgrounds: the face-button diamond still reads at 14 px (its rings
 * stay open, the solid dot stays the only solid one), but keycap text, the
 * word "Start" and a stick's arrows need 16. A prompt sits in text: give its
 * line the body text size (CW_TEXT_FLOOR_BODY, cartwheel/render2d.h).
 * Smaller sizes still draw; the rings close up and the text blurs.
 *
 * TEXT SCALE: every size here is multiplied by the player's text scale
 * (cw_text_scale, cartwheel/render2d.h) once, at the call, with the same cap,
 * so a prompt line grows with the text around it and cw_prompt_text_size
 * answers the size it draws at. The words of cw_prompt_text count in the
 * text size check; a glyph's own legend (a keycap's letters) does not.
 *
 * ── Which device ───────────────────────────────────────────────────────
 * Each player's CURRENT device is the one they used last, read from the
 * input module's FRAME snapshot: a pad button, a stick or trigger past half
 * way, a key going down, a mouse button or the wheel, a touch landing. Before
 * a player touches anything: their pad when it is connected, else a touch
 * screen on a touch-first device (cw_platform_touch_primary), else the
 * keyboard, else their pad even when it is unplugged. A pad that unplugs
 * hands its player back to that default.
 *
 * Which binding of several: the FIRST binding, in binding order, on that
 * device. So `key:space, pad:south` draws the diamond for a pad player and
 * "Space" for a keyboard player, with no game code. The keyboard and mouse
 * count as one desk: a keyboard player is shown a mouse binding when the
 * action has no key, and the other way round.
 *
 * Nothing on the device: an action with no binding on the player's device
 * draws NOTHING (never a guess, never a placeholder) and logs a warning ONCE
 * per action and device, naming both. The debug field `prompt.missing` counts
 * those pairs.
 *
 * ── Movement ───────────────────────────────────────────────────────────
 * Two actions (left|right or up|down) or four (left|right|up|down) draw as
 * ONE glyph when their pad bindings agree: the stick when all of them are on
 * one stick, else the d-pad when all are on it. When both exist the stick is
 * shown, except on a pad whose d-pad this player has used and whose sticks
 * they never have: that is how a pad with no stick (a retro handheld, which
 * still steers through the d-pad bindings of the same actions) is told apart,
 * since no host reports which pads have sticks. Until a pad has been used it
 * shows the stick. Bindings that agree on nothing draw each action's glyph
 * side by side. On a keyboard each action's keycap is drawn, side by side.
 *
 * ── Shared prompts (player -1) ─────────────────────────────────────────
 * A title or instructions screen that several seats read: the pad glyph when
 * any present seat is on a pad, then the keycap when any is on the keyboard
 * or mouse, then the touch control when any is on touch, side by side. A
 * seat is present when its pad is connected or it holds the keyboard, mouse
 * or touch. A device the action has no binding on is left out while another
 * device shows something (a menu's keys on a phone beside a pad); only when
 * nothing at all is drawn is it logged. Per-seat text (a join slot, a
 * split-screen HUD) passes the seat.
 *
 * ── Text markup ───────────────────────────────────────────────────────
 * cw_prompt_text draws text with glyphs in it, on one line or several ('\n'):
 *   {jump}                  an action's glyph, by its name
 *   {pad:west} {key:space}  one binding (the cw_binding_parse text form), for a
 *                           design that is about position (lanes, answer slots)
 *   {move_left|move_right}  a pair as one glyph (Movement, above)
 *   {left|right|up|down}    four directions as one glyph
 *   {{ and }}               a literal brace
 * The markup lives inside translated strings (cw_tr("hint.jump") returning
 * "{jump} : sauter"), so a translator moves the glyph where the language
 * wants it. Right-to-left text lays its runs out right to left, glyphs
 * included. A name that is no action (a typo) draws nothing, is logged by
 * cw_error ONCE naming it and the text, and counts in the debug field
 * `prompt.errors`, which tests/gate.mjs fails a run on.
 *
 * ── Determinism ───────────────────────────────────────────────────────
 * Prompts are presentation. The current device comes from the frame
 * snapshot and never from the tick snapshot; nothing here enters the
 * simulation, the recorded action stream or `input.hash`. Calling any of
 * these from fixed_update is a bug (the answer would change the game as the
 * player picks up another device): debug builds assert, release builds log
 * once and count it in `prompt.errors`.
 *
 * ── Tests ─────────────────────────────────────────────────────────────
 * CW_TEST_PROMPT_DEVICE=pad|keyboard|mouse|touch (native runs) forces every
 * player's device, whatever they press. The debug request `prompt device
 * <player|all> <pad|keyboard|mouse|touch|auto>` does the same in a cart (and
 * natively): a gate passes it with "startCmds". Without either, an input
 * script switches the device with its first event on another device, as a
 * player would. `prompt` alone answers every player's device. Debug fields:
 * `prompt.devices` (cw_prompt_device per player), `prompt.errors`,
 * `prompt.missing`, `prompt.count` (glyphs drawn last frame) and
 * `prompt.glyphs` (the first 16 of them, each kind << 16 | what was lit:
 * see cw_prompt_glyph_code).
 *
 * Costs: a glyph is 4 to 20 render2d shapes; text markup is one layout pass
 * per call (cw_font_measure per run). Main thread only, inside an open
 * render2d pass (cw_r2d_begin), outside fixed_update. */
#ifndef CW_PROMPT_H
#define CW_PROMPT_H

#include "base.h"
#include "input.h"
#include "math.h"
#include "render2d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  CW_PROMPT_NONE,     /* the player has no device (a seat with nothing assigned) */
  CW_PROMPT_PAD,
  CW_PROMPT_KEYBOARD,
  CW_PROMPT_MOUSE,
  CW_PROMPT_TOUCH,
  CW_PROMPT_DEVICE_COUNT
} cw_prompt_device;

/* The device this player used last (see "Which device"), from the FRAME
 * snapshot: for UI only, never for simulation. player outside
 * 0..CW_INPUT_MAX_PLAYERS-1: CW_PROMPT_NONE. */
cw_prompt_device cw_prompt_device_of(int player);
/* "none", "pad", "keyboard", "mouse", "touch". */
const char *cw_prompt_device_name(cw_prompt_device d);
/* Force a player's device (player -1: every player), as a test or a settings
 * page ("show keyboard prompts") wants; CW_PROMPT_NONE ends the forcing and
 * the player's last-used device answers again. */
void cw_prompt_force_device(int player, cw_prompt_device d);

/* One action's glyph for this player (player -1: every present seat, see
 * "Shared prompts"), its box's top left at pos, `size` canvas units tall
 * (wider glyphs are wider than size). color: the light fill ({0,0,0,0} =
 * white); its alpha fades the whole glyph. Returns the size drawn: {0, 0}
 * when the action has no binding on the device (logged once). */
cw_vec2 cw_prompt_action(cw_action a, int player, cw_vec2 pos, float size, cw_color color);
/* The size cw_prompt_action would draw, drawing nothing. */
cw_vec2 cw_prompt_action_size(cw_action a, int player, float size);

/* Two or four actions as one movement glyph (see "Movement"): n 2 is a pair
 * (left, right or up, down), n 4 is left, right, up, down. n 1 is
 * cw_prompt_action. Returns the size drawn. */
cw_vec2 cw_prompt_actions(const cw_action *acts, int n, int player, cw_vec2 pos, float size, cw_color color);
cw_vec2 cw_prompt_actions_size(const cw_action *acts, int n, int player, float size); /* the size it would draw */

/* One physical input, whatever device the player is on: for a design that is
 * about position (lanes laid out west, south, east; answers in a diamond).
 * Returns the size drawn ({0, 0} for CW_BIND_NONE). */
cw_vec2 cw_prompt_binding(cw_binding b, cw_vec2 pos, float size, cw_color color);
cw_vec2 cw_prompt_binding_size(cw_binding b, float size); /* the size it would draw */

/* Text with glyphs in it (see "Text markup"), in font f at size (the em, as
 * cw_text_style.size), each glyph as tall as the text's capitals plus a bit
 * and centred on the line. pos: the box's top, and its left edge
 * (CW_ALIGN_LEFT), centre (CW_ALIGN_CENTER) or right edge (CW_ALIGN_RIGHT),
 * each line aligned on its own. The text has a dark outline, as the glyphs
 * do. Returns the size of the box drawn. */
cw_vec2 cw_prompt_text(cw_font f, const char *text, int player, cw_vec2 pos, float size, cw_color color, cw_text_align align);
/* The box cw_prompt_text would draw, drawing nothing (a typo still counts). */
cw_vec2 cw_prompt_text_size(cw_font f, const char *text, int player, float size);

/* The font for keycaps and "Start" (and for cw_prompt_text with a null font).
 * Default: the UI's sans (cw_ui_font_sans). */
void cw_prompt_set_font(cw_font f);

/* What a glyph is, as the debug field prompt.glyphs records each one drawn:
 * kind << 16 | lit. Kinds: 1 face buttons (lit: bit 0 south, 1 east, 2 west,
 * 3 north), 2 shoulders and 3 triggers (bit 0 left, 1 right), 4 stick (bit 0
 * the right stick, bits 4..7 directions left, right, up, down, bit 8 press),
 * 5 d-pad (bits 0..3 left, right, up, down), 6 Start, 7 Back, 8 Guide, 9
 * keycap (lit: the scancode), 10 mouse (bit 0 left, 1 right, 2 middle, 4..7
 * wheel up, down, left, right), 11 touch button (lit: control id), 12 touch
 * stick (control id | directions << 8). 0 when the binding draws nothing. */
uint32_t cw_prompt_glyph_code(cw_binding b);
/* The codes of the glyphs cw_prompt_actions would draw for these actions and
 * this player, drawing nothing (tests, and a game checking its own prompts):
 * returns how many there are, writing up to cap of them. */
int cw_prompt_action_codes(const cw_action *acts, int n, int player, uint32_t *out, int cap);

#ifdef __cplusplus
}
#endif
#endif
