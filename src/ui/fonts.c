/* The UI's embedded fonts (SIL Open Font License 1.1, texts beside them in
 * src/ui/fonts/): Fira Sans Regular for UI text, Fira Mono Regular for code.
 * Loaded on first use as SDF fonts (one bake serves every size). */
#include "ui_internal.h"

#if defined(__clang__)
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif

/* CW_EMBED_INC: a compiler without #embed (the Android NDK's clang 18) gets
 * the same bytes as generated initializer lists (CMakeLists.txt) */
static const unsigned char fira_sans[] = {
#if defined(CW_EMBED_INC)
#include "FiraSans-Regular.ttf.inc"
#else
#embed "fonts/FiraSans-Regular.ttf"
#endif
};
static const unsigned char fira_mono[] = {
#if defined(CW_EMBED_INC)
#include "FiraMono-Regular.ttf.inc"
#else
#embed "fonts/FiraMono-Regular.ttf"
#endif
};

static cw_font sans, mono;

cw_font cw_ui_font_sans(void) {
  if (CW_HANDLE_IS_NULL(sans))
    sans = cw_font_from_memory(fira_sans, sizeof fira_sans, &(cw_font_desc){ .mode = CW_FONT_SDF, .bake_size = 48 });
  return sans;
}

cw_font cw_ui_font_mono(void) {
  if (CW_HANDLE_IS_NULL(mono))
    mono = cw_font_from_memory(fira_mono, sizeof fira_mono, &(cw_font_desc){ .mode = CW_FONT_SDF, .bake_size = 48 });
  return mono;
}
