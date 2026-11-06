/* The engine's own text calls (the UI, the prompts, the touch overlay):
 * text whose size the caller already scaled (a UI theme font is
 * cw_text_scale times its themed size; a prompt scales its size once at
 * the public call), so render2d must not scale it again. Counted by the
 * size check unless CW__TEXT_UNCOUNTED (a keycap's legend is part of a
 * glyph, not text a player reads as text). */
#ifndef CW_TEXT_RAW_H
#define CW_TEXT_RAW_H

#include "cartwheel/render2d.h"

enum { CW__TEXT_UNCOUNTED = 1 };
cw_text_metrics cw__r2d_text_raw(cw_font f, const char *utf8, cw_vec2 pos, const cw_text_style *style, int flags);
cw_text_metrics cw__font_measure_raw(cw_font f, const char *utf8, const cw_text_style *style);
float cw__font_advance_raw(cw_font f, uint32_t codepoint, float size);
float cw__font_caret_x_raw(cw_font f, const char *utf8, int byte, float size);
/* the factor the text scale applies to text of this size now (1 outside a window pass's reach, for fixed_size) */
float cw__text_scale_for(cw_font f, float size);

#endif
