/* Recast and Detour's math and allocator, compiled in through the hooks of
 * patches/recastnavigation/0002 and 0003 (RC_MATH_HEADER, DT_MATH_HEADER,
 * RC_ALLOC_HEADER, DT_ALLOC_HEADER all name this file; each part is used by
 * the file that asks for it).
 *
 *   - math: cartwheel's deterministic transcendentals, the same bits on every
 *     C library;
 *   - memory: cw_alloc under CW_MEM_NAV (thread-safe, so tiles build on
 *     several threads), zeroed, so no uninitialized byte can reach tile data
 *     that is hashed or saved. */
#ifndef CW_RECAST_HOOKS_H
#define CW_RECAST_HOOKS_H

#include "cartwheel/core.h"
#include "cartwheel/math.h"

static inline float rcMathCosf(float x) { return cw_cos(x); }
inline float dtMathCosf(float x) { return cw_cos(x); }
inline float dtMathSinf(float x) { return cw_sin(x); }
inline float dtMathAtan2f(float y, float x) { return cw_atan2(y, x); }

#endif

/* the allocator parts: included by RecastAlloc.cpp / DetourAlloc.cpp, where
 * rcAllocHint / dtAllocHint are declared */
#if defined(RECASTALLOC_H) && !defined(CW_RECAST_ALLOC_DONE)
#define CW_RECAST_ALLOC_DONE
__attribute__((unused)) static void *rcAllocDefault(size_t size, rcAllocHint) { return cw_calloc(1, size ? size : 1, CW_MEM_NAV); }
__attribute__((unused)) static void rcFreeDefault(void *ptr) { cw_free(ptr); }
#endif
#if defined(DETOURALLOCATOR_H) && !defined(CW_DETOUR_ALLOC_DONE)
#define CW_DETOUR_ALLOC_DONE
__attribute__((unused)) static void *dtAllocDefault(size_t size, dtAllocHint) { return cw_calloc(1, size ? size : 1, CW_MEM_NAV); }
__attribute__((unused)) static void dtFreeDefault(void *ptr) { cw_free(ptr); }
#endif
