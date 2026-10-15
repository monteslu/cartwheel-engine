/*
 * orca.c: ORCA (optimal reciprocal collision avoidance) for cartwheel's crowd,
 * ported to C from the RVO2 Library's Agent.cc.
 *
 * SPDX-FileCopyrightText: 2008 University of North Carolina at Chapel Hill
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * <https://gamma.cs.unc.edu/RVO2/>
 *
 * Changed from the original (see ORIGIN.md): C instead of C++, agent lines
 * built four neighbours at a time with SIMD (both branches computed, the
 * answer selected per lane), a per-pair responsibility weight in place of the
 * fixed 0.5, a defined direction for coincident agents, and no static
 * obstacle lines (the navmesh bounds movement instead).
 *
 * Determinism: only + - * / and sqrt, compares and selects, so every host
 * and every SIMD path gives the same bits (simd4.h).
 */
#include <math.h>

#include "orca.h"
#include "../../simd4.h"

#define RVO_EPSILON 0.00001f

void cw_orca_lines(const cw_orca_input *in, int n, float vx, float vy, float inv_time_horizon, float inv_dt,
                   cw_orca_line *out) {
  const v4 zero = v4_splat(0), one = v4_splat(1);
  const v4 ith = v4_splat(inv_time_horizon), idt = v4_splat(inv_dt);
  const v4 svx = v4_splat(vx), svy = v4_splat(vy);
  for (int i = 0; i < n; i += 4) {
#define LD(a) v4_set(in->a[i], in->a[i + 1], in->a[i + 2], in->a[i + 3])
    v4 rpx = LD(rpx), rpy = LD(rpy), rvx = LD(rvx), rvy = LD(rvy), cr = LD(cr), w = LD(weight), side = LD(side);
#undef LD
    v4 dist_sq = v4_add(v4_mul(rpx, rpx), v4_mul(rpy, rpy));
    v4 cr_sq = v4_mul(cr, cr);

    /* No collision: w is the vector from the cutoff center to the relative velocity. */
    v4 wx = v4_sub(rvx, v4_mul(ith, rpx)), wy = v4_sub(rvy, v4_mul(ith, rpy));
    v4 w_len_sq = v4_add(v4_mul(wx, wx), v4_mul(wy, wy));
    v4 dot1 = v4_add(v4_mul(wx, rpx), v4_mul(wy, rpy));
    v4m cut = v4_and(v4_lt(dot1, zero), v4_gt(v4_mul(dot1, dot1), v4_mul(cr_sq, w_len_sq)));
    /* project on the cut-off circle */
    v4 w_len = v4_sqrt(w_len_sq);
    v4 w_safe = v4_sel(v4_gt(w_len, zero), w_len, one);
    v4 uwx = v4_div(wx, w_safe), uwy = v4_div(wy, w_safe);
    v4 cut_dx = uwy, cut_dy = v4_sub(zero, uwx);
    v4 cut_s = v4_sub(v4_mul(cr, ith), w_len);
    v4 cut_ux = v4_mul(cut_s, uwx), cut_uy = v4_mul(cut_s, uwy);
    /* project on the legs */
    v4 leg_sq = v4_sub(dist_sq, cr_sq);
    v4 leg = v4_sqrt(v4_sel(v4_gt(leg_sq, zero), leg_sq, zero));
    v4 d_safe = v4_sel(v4_gt(dist_sq, zero), dist_sq, one);
    v4m left = v4_gt(v4_sub(v4_mul(rpx, wy), v4_mul(rpy, wx)), zero);
    v4 l_dx = v4_div(v4_sub(v4_mul(rpx, leg), v4_mul(rpy, cr)), d_safe);
    v4 l_dy = v4_div(v4_add(v4_mul(rpx, cr), v4_mul(rpy, leg)), d_safe);
    v4 r_dx = v4_sub(zero, v4_div(v4_add(v4_mul(rpx, leg), v4_mul(rpy, cr)), d_safe));
    v4 r_dy = v4_sub(zero, v4_div(v4_add(v4_sub(zero, v4_mul(rpx, cr)), v4_mul(rpy, leg)), d_safe));
    v4 leg_dx = v4_sel(left, l_dx, r_dx), leg_dy = v4_sel(left, l_dy, r_dy);
    v4 dot2 = v4_add(v4_mul(rvx, leg_dx), v4_mul(rvy, leg_dy));
    v4 leg_ux = v4_sub(v4_mul(dot2, leg_dx), rvx), leg_uy = v4_sub(v4_mul(dot2, leg_dy), rvy);

    /* Collision: project on the cut-off circle of one time step. Agents
     * exactly on top of each other part along x, each to its own side. */
    v4 bx = v4_sub(rvx, v4_mul(idt, rpx)), by = v4_sub(rvy, v4_mul(idt, rpy));
    v4 b_len = v4_sqrt(v4_add(v4_mul(bx, bx), v4_mul(by, by)));
    v4m b_zero = v4_not(v4_gt(b_len, zero));
    v4 b_safe = v4_sel(b_zero, one, b_len);
    v4 ubx = v4_sel(b_zero, side, v4_div(bx, b_safe)), uby = v4_sel(b_zero, zero, v4_div(by, b_safe));
    v4 col_dx = uby, col_dy = v4_sub(zero, ubx);
    v4 col_s = v4_sub(v4_mul(cr, idt), b_len);
    v4 col_ux = v4_mul(col_s, ubx), col_uy = v4_mul(col_s, uby);

    v4m apart = v4_gt(dist_sq, cr_sq);
    v4 dx = v4_sel(apart, v4_sel(cut, cut_dx, leg_dx), col_dx);
    v4 dy = v4_sel(apart, v4_sel(cut, cut_dy, leg_dy), col_dy);
    v4 ux = v4_sel(apart, v4_sel(cut, cut_ux, leg_ux), col_ux);
    v4 uy = v4_sel(apart, v4_sel(cut, cut_uy, leg_uy), col_uy);
    v4 px = v4_add(svx, v4_mul(w, ux)), py = v4_add(svy, v4_mul(w, uy));

    float fpx[4], fpy[4], fdx[4], fdy[4];
    v4_store(fpx, px);
    v4_store(fpy, py);
    v4_store(fdx, dx);
    v4_store(fdy, dy);
    for (int k = 0; k < 4 && i + k < n; k++) out[i + k] = (cw_orca_line){ fpx[k], fpy[k], fdx[k], fdy[k] };
  }
}

static inline float det2(float ax, float ay, float bx, float by) { return ax * by - ay * bx; }

/* linearProgram1: the best point on line no within the speed circle and the earlier lines. */
static int lp1(const cw_orca_line *lines, int no, float radius, const float opt[2], int dir_opt, float result[2]) {
  const cw_orca_line *L = &lines[no];
  const float dot = L->px * L->dx + L->py * L->dy;
  const float disc = dot * dot + radius * radius - (L->px * L->px + L->py * L->py);
  if (disc < 0) return 0; /* the speed circle fully invalidates line no */
  const float sq = sqrtf(disc);
  float t_left = -dot - sq, t_right = -dot + sq;
  for (int i = 0; i < no; i++) {
    const cw_orca_line *M = &lines[i];
    const float denom = det2(L->dx, L->dy, M->dx, M->dy);
    const float numer = det2(M->dx, M->dy, L->px - M->px, L->py - M->py);
    if ((denom < 0 ? -denom : denom) <= RVO_EPSILON) { /* (almost) parallel */
      if (numer < 0) return 0;
      continue;
    }
    const float t = numer / denom;
    if (denom >= 0) t_right = t < t_right ? t : t_right; /* line i bounds line no on the right */
    else t_left = t > t_left ? t : t_left;               /* on the left */
    if (t_left > t_right) return 0;
  }
  float t;
  if (dir_opt) {
    t = opt[0] * L->dx + opt[1] * L->dy > 0 ? t_right : t_left;
  } else {
    t = L->dx * (opt[0] - L->px) + L->dy * (opt[1] - L->py);
    t = t < t_left ? t_left : t > t_right ? t_right : t;
  }
  result[0] = L->px + t * L->dx;
  result[1] = L->py + t * L->dy;
  return 1;
}

/* linearProgram2: returns the count of lines satisfied (n when all are). */
static int lp2(const cw_orca_line *lines, int n, float radius, const float opt[2], int dir_opt, float result[2]) {
  if (dir_opt) {
    result[0] = opt[0] * radius;
    result[1] = opt[1] * radius;
  } else if (opt[0] * opt[0] + opt[1] * opt[1] > radius * radius) {
    float len = sqrtf(opt[0] * opt[0] + opt[1] * opt[1]);
    result[0] = opt[0] / len * radius;
    result[1] = opt[1] / len * radius;
  } else {
    result[0] = opt[0];
    result[1] = opt[1];
  }
  for (int i = 0; i < n; i++) {
    if (det2(lines[i].dx, lines[i].dy, lines[i].px - result[0], lines[i].py - result[1]) > 0) {
      float keep[2] = { result[0], result[1] };
      if (!lp1(lines, i, radius, opt, dir_opt, result)) {
        result[0] = keep[0];
        result[1] = keep[1];
        return i;
      }
    }
  }
  return n;
}

/* linearProgram3: when the lines leave no room, the velocity that violates them least. */
static void lp3(const cw_orca_line *lines, int n, int begin, float radius, float result[2]) {
  float distance = 0;
  cw_orca_line proj[16];
  for (int i = begin; i < n; i++) {
    const cw_orca_line *L = &lines[i];
    if (det2(L->dx, L->dy, L->px - result[0], L->py - result[1]) <= distance) continue;
    int np = 0;
    for (int j = 0; j < i; j++) {
      const cw_orca_line *M = &lines[j];
      cw_orca_line l;
      const float d = det2(L->dx, L->dy, M->dx, M->dy);
      if ((d < 0 ? -d : d) <= RVO_EPSILON) { /* parallel */
        if (L->dx * M->dx + L->dy * M->dy > 0) continue; /* same direction */
        l.px = 0.5f * (L->px + M->px);
        l.py = 0.5f * (L->py + M->py);
      } else {
        const float s = det2(M->dx, M->dy, L->px - M->px, L->py - M->py) / d;
        l.px = L->px + s * L->dx;
        l.py = L->py + s * L->dy;
      }
      float ddx = M->dx - L->dx, ddy = M->dy - L->dy;
      float len = sqrtf(ddx * ddx + ddy * ddy);
      l.dx = len > 0 ? ddx / len : 0;
      l.dy = len > 0 ? ddy / len : 0;
      proj[np++] = l;
    }
    float keep[2] = { result[0], result[1] };
    const float opt[2] = { -L->dy, L->dx };
    if (lp2(proj, np, radius, opt, 1, result) < np) {
      /* in principle impossible (the result is already feasible); floating
       * point error: keep the current result */
      result[0] = keep[0];
      result[1] = keep[1];
    }
    distance = det2(L->dx, L->dy, L->px - result[0], L->py - result[1]);
  }
}

void cw_orca_solve(const cw_orca_line *lines, int n, float max_speed, const float pref[2], float out[2]) {
  int fail = lp2(lines, n, max_speed, pref, 0, out);
  if (fail < n) lp3(lines, n, fail, max_speed, out);
}
