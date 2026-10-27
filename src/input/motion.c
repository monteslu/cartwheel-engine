#include "cartwheel/motion.h"

#include <string.h>

static inline const cw_cmd_frame *at(const cw_cmd_buf *b, int age) {
  if (age < 0 || age >= CW_CMD_HISTORY || (uint32_t)age >= b->count) return NULL;
  uint32_t idx = b->count - 1 - (uint32_t)age;
  if (idx < b->consumed) return NULL;
  return &b->f[idx & (CW_CMD_HISTORY - 1)];
}

void cw_cmd_clear(cw_cmd_buf *b) { memset(b, 0, sizeof *b); }

int cw_cmd_dir(bool left, bool right, bool up, bool down, int facing) {
  int x = (right ? 1 : 0) - (left ? 1 : 0);
  int y = up ? 1 : (down ? -1 : 0);
  if (facing < 0) x = -x;
  return 5 + x + 3 * y;
}

void cw_cmd_push(cw_cmd_buf *b, bool left, bool right, bool up, bool down, int facing, uint16_t buttons) {
  cw_cmd_frame *f = &b->f[b->count & (CW_CMD_HISTORY - 1)];
  f->dir = (uint8_t)cw_cmd_dir(left, right, up, down, facing);
  f->pad_ = 0;
  f->buttons = buttons;
  b->count++;
}

int cw_cmd_dir_now(const cw_cmd_buf *b) {
  const cw_cmd_frame *f = at(b, 0);
  return f ? f->dir : 5;
}
uint16_t cw_cmd_held_now(const cw_cmd_buf *b) {
  const cw_cmd_frame *f = at(b, 0);
  return f ? f->buttons : 0;
}

/* buttons held on the frame before `age` (0 before the first frame) */
static uint16_t held_before(const cw_cmd_buf *b, int age) {
  if ((uint32_t)age + 1 >= b->count || age + 1 >= CW_CMD_HISTORY) return 0;
  uint32_t idx = b->count - 2 - (uint32_t)age;
  return b->f[idx & (CW_CMD_HISTORY - 1)].buttons;
}

int cw_cmd_pressed_ago(const cw_cmd_buf *b, uint16_t mask, int within) {
  if (within >= CW_CMD_HISTORY) within = CW_CMD_HISTORY - 1;
  for (int a = 0; a <= within; a++) {
    const cw_cmd_frame *f = at(b, a);
    if (!f) return -1;
    if ((f->buttons & mask) & ~held_before(b, a)) return a;
  }
  return -1;
}
int cw_cmd_released_ago(const cw_cmd_buf *b, uint16_t mask, int within) {
  if (within >= CW_CMD_HISTORY) within = CW_CMD_HISTORY - 1;
  for (int a = 0; a <= within; a++) {
    const cw_cmd_frame *f = at(b, a);
    if (!f) return -1;
    if ((held_before(b, a) & mask) & ~f->buttons) return a;
  }
  return -1;
}

void cw_cmd_consume(cw_cmd_buf *b) { b->consumed = b->count; }

/* ── motions ─────────────────────────────────────────────────────────── */

bool cw_motion_parse(const char *text, cw_motion *out) {
  cw_motion m;
  memset(&m, 0, sizeof m);
  m.charge_frames = 30;
  if (!text) return false;
  bool in_br = false;
  for (const char *p = text; *p; p++) {
    if (*p == ' ') continue;
    if (*p == '[') {
      if (in_br || m.count != 0) return false;
      in_br = true;
      continue;
    }
    if (*p == ']') {
      if (!in_br || m.count != 1) return false;
      in_br = false;
      m.charge = 1;
      continue;
    }
    if (*p < '1' || *p > '9') return false;
    if (m.count >= CW_MOTION_MAX_STEPS) return false;
    m.steps[m.count++] = (uint8_t)(*p - '0');
  }
  if (in_br || m.count == 0 || (m.charge && m.count < 2)) return false;
  *out = m;
  return true;
}

/* does direction state s contain step d */
static inline bool has(int s, int d) {
  int sx = s % 3 == 1 ? -1 : (s % 3 == 0 ? 1 : 0); /* 1,4,7 -> -1; 3,6,9 -> +1 */
  int sy = s <= 3 ? -1 : (s >= 7 ? 1 : 0);
  int dx = d % 3 == 1 ? -1 : (d % 3 == 0 ? 1 : 0);
  int dy = d <= 3 ? -1 : (d >= 7 ? 1 : 0);
  if (d == 5) return s == 5;
  if (dx && sx != dx) return false;
  if (dy && sy != dy) return false;
  return true;
}
static inline bool diagonal(int d) { return d == 1 || d == 3 || d == 7 || d == 9; }

/* Match steps [0..last] ending at or before age `from` (the newest frame the
 * last step may use). Returns the age of the last step's frame, or -1. The
 * motion's first step must be at an age <= end_age + window - 1. */
static int match_from(const cw_cmd_buf *b, const cw_motion *m, int window, int from, int max_end) {
  int last_age = -1;
  int prev_dir = 0;
  int cur = from; /* oldest age the next (earlier) step may use is > cur */
  int first = m->charge ? 1 : 0;
  int end_age = -1;
  for (int i = m->count - 1; i >= first; i--) {
    int d = m->steps[i];
    int found = -1;
    for (int a = cur; a < CW_CMD_HISTORY; a++) {
      const cw_cmd_frame *f = at(b, a);
      if (!f) break;
      if (end_age < 0 && a > max_end) break;
      if (end_age >= 0 && a - end_age >= window) break;
      if (prev_dir && f->dir == prev_dir && !(i + 1 < m->count && m->steps[i + 1] == d)) continue; /* a held diagonal is not two steps */
      if (has(f->dir, d)) { found = a; prev_dir = f->dir; break; }
    }
    if (found < 0) {
      if (diagonal(d) && i != m->count - 1 && i != first) continue; /* optional */
      return -1;
    }
    if (end_age < 0) end_age = found, last_age = found;
    cur = found + 1;
    /* a repeated step needs a gap: skip the run that matched */
    if (i > first && m->steps[i - 1] == d) {
      while (1) {
        const cw_cmd_frame *f = at(b, cur);
        if (!f || !has(f->dir, d)) break;
        cur++;
      }
    }
  }
  if (m->charge) {
    int d = m->steps[0];
    /* a run of frames containing d, at least charge_frames long, whose newest
     * frame is at age >= cur, inside the window */
    int run = 0, newest = -1;
    for (int a = cur; a < CW_CMD_HISTORY; a++) {
      const cw_cmd_frame *f = at(b, a);
      if (!f) break;
      if (end_age >= 0 && a - end_age >= window) break;
      if (has(f->dir, d)) {
        if (run == 0) newest = a;
        run++;
        if (run >= m->charge_frames) return last_age;
      } else run = 0;
    }
    (void)newest;
    return -1;
  }
  return last_age;
}

int cw_motion_match(const cw_cmd_buf *b, const cw_motion *m, int window) {
  if (!b || !m || m->count == 0) return -1;
  return match_from(b, m, window, 0, window - 1);
}

int cw_special_match(const cw_cmd_buf *b, const cw_motion *m, uint16_t mask, int window, int buffer) {
  if (!b || !m || m->count == 0) return -1;
  int a = cw_cmd_pressed_ago(b, mask, buffer);
  if (a < 0) return -1;
  /* the motion's last step may sit from 2 frames after the press (age a-2)
   * back; ages below 0 do not exist */
  int from = a - 2 < 0 ? 0 : a - 2;
  int r = match_from(b, m, window, from, a + window);
  return r < 0 ? -1 : a;
}
