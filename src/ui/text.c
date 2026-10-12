/* UTF-8 helpers and growable strings for the text widgets. */
#include "ui_internal.h"

int ui_utf8_next(const char *s, int len, int i) {
  if (i >= len) return len;
  i++;
  while (i < len && ((unsigned char)s[i] & 0xc0) == 0x80) i++;
  return i;
}

int ui_utf8_prev(const char *s, int i) {
  if (i <= 0) return 0;
  i--;
  while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80) i--;
  return i;
}

int ui_utf8_count(const char *s, int len) {
  int n = 0;
  for (int i = 0; i < len; i++)
    if (((unsigned char)s[i] & 0xc0) != 0x80) n++;
  return n;
}

uint32_t ui_utf8_decode(const char *s, int len, int *i) {
  unsigned char c = (unsigned char)s[*i];
  uint32_t cp;
  int n;
  if (c < 0x80) cp = c, n = 1;
  else if ((c >> 5) == 6) cp = c & 31, n = 2;
  else if ((c >> 4) == 14) cp = c & 15, n = 3;
  else cp = c & 7, n = 4;
  for (int k = 1; k < n && *i + k < len; k++) cp = (cp << 6) | ((unsigned char)s[*i + k] & 63);
  *i += n;
  return cp;
}

bool ui_is_word_char(uint32_t c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

static void grow(ui_str *b, int need) {
  if (need + 1 <= b->cap) return;
  int nc = b->cap ? b->cap : 32;
  while (nc < need + 1) nc *= 2;
  bool fresh = !b->s;
  b->s = cw_realloc(b->s, (size_t)nc, CW_MEM_UI);
  if (fresh) b->s[0] = 0, b->len = 0;
  b->cap = nc;
}

void ui_str_set(ui_str *b, const char *s, int len) {
  if (!s) s = "", len = 0;
  if (len < 0) len = (int)strlen(s);
  grow(b, len);
  memmove(b->s, s, (size_t)len);
  b->s[len] = 0;
  b->len = len;
}

void ui_str_insert(ui_str *b, int at, const char *s, int len) {
  if (len < 0) len = (int)strlen(s);
  if (at < 0) at = 0;
  if (at > b->len) at = b->len;
  grow(b, b->len + len);
  memmove(b->s + at + len, b->s + at, (size_t)(b->len - at + 1));
  memcpy(b->s + at, s, (size_t)len);
  b->len += len;
}

void ui_str_erase(ui_str *b, int at, int n) {
  if (at < 0) n += at, at = 0;
  if (at + n > b->len) n = b->len - at;
  if (n <= 0) return;
  memmove(b->s + at, b->s + at + n, (size_t)(b->len - at - n + 1));
  b->len -= n;
}

void ui_str_free(ui_str *b) {
  cw_free(b->s);
  b->s = NULL;
  b->len = b->cap = 0;
}
