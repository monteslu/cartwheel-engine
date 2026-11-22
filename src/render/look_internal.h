/* Looks (cartwheel/look.h) internals shared by looks.c and look_font.c. */
#ifndef CW_LOOK_INTERNAL_H
#define CW_LOOK_INTERNAL_H

#include <stddef.h>

typedef struct cw__look_font cw__look_font;
/* a font over ttf (kept, not copied); owned (may be NULL) is freed with the font. NULL when stb_truetype cannot read it */
cw__look_font *cw__look_font_new(const unsigned char *ttf, size_t len, void *owned);
void cw__look_font_free(cw__look_font *f);
/* the advance of 'M' over the line height */
float cw__look_font_aspect(const cw__look_font *f);
/* codepoint cp into a cw x ch cell, row 0 the top, coverage 0..255 */
void cw__look_font_raster(const cw__look_font *f, int cp, int cw, int ch, unsigned char *out);
/* the built-in Fira Mono (src/ui/fonts.c) */
const unsigned char *cw__ui_mono_ttf(size_t *len);

#endif
