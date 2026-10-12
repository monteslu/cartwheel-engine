/* kb_text_shape (deps/kb, zlib license, (C) Jimmy Lefevre), compiled once:
 * OpenType shaping, bidi run segmentation and font fallback for the 2D text
 * renderer's complex-script path (src/render2d/text.c). Scalar rounding on
 * every target so native and cart shape to the same positions. */
#define KB_TEXT_SHAPE_IMPLEMENTATION
#define KB_TEXT_SHAPE_NO_CRT
#define KBTS__ROUND_COMPATIBILITY
#include <string.h>
#define KBTS_MEMSET memset
#define KBTS_MEMCPY memcpy
#include "kb_text_shape.h"
