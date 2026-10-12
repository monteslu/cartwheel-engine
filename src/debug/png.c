/* stb_image_write, compiled once for both builds: native screenshots
 * (CW_SCREENSHOT) and the debug channel's layer screenshots (PNG in memory,
 * cart and native alike). */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ASSERT(x) ((void)0)
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "stb_image_write.h"
