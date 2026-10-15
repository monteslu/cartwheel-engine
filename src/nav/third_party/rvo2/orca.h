/*
 * orca.h: ORCA (optimal reciprocal collision avoidance) for cartwheel's crowd,
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
 * built four neighbours at a time with SIMD, a per-pair responsibility weight
 * in place of the fixed 0.5, a defined direction for coincident agents, and
 * no static obstacle lines (the navmesh bounds movement instead).
 */
#ifndef CW_NAV_ORCA_H
#define CW_NAV_ORCA_H

#ifdef __cplusplus
extern "C" {
#endif

/* A half-plane constraint on velocity: the permitted side is left of the
 * directed line through (px, py) along (dx, dy). */
typedef struct cw_orca_line {
  float px, py, dx, dy;
} cw_orca_line;

/* The neighbours of one agent as seen from it, lanes padded to a multiple of 4. */
typedef struct cw_orca_input {
  float rpx[16], rpy[16];  /* relative positions (other - self) */
  float rvx[16], rvy[16];  /* relative velocities (self - other) */
  float cr[16];            /* combined radii */
  float weight[16];        /* this agent's share of the avoidance (0.5 between equals) */
  float side[16];          /* +1 / -1: which way to part when exactly on top of each other */
} cw_orca_input;

/* The ORCA lines for n neighbours (n <= 16) of an agent moving at (vx, vy). */
void cw_orca_lines(const cw_orca_input *in, int n, float vx, float vy, float inv_time_horizon, float inv_dt,
                   cw_orca_line *out);
/* The velocity closest to pref (within max_speed) that satisfies the lines,
 * or the one that violates them least (n <= 16). */
void cw_orca_solve(const cw_orca_line *lines, int n, float max_speed, const float pref[2], float out[2]);

#ifdef __cplusplus
}
#endif
#endif
