/* Path canonicalization and default UIDs: shared by the runtime and cwpack,
 * so both agree on what a path and its UID are. */
#include <stdio.h>
#include <string.h>

#include "cartwheel/resource.h"
#include "resource_internal.h"

uint64_t cw_res_path_uid(const char *path) {
  char c[CW_RES_PATH_MAX];
  if (!cw__res_canon(path, c, sizeof c)) return 0;
  uint64_t h = 14695981039346656037ull; /* the FNV-1a 64 offset basis: any FNV-1a tool computes the same UID */
  for (const char *p = c; *p; p++) {
    h ^= (uint8_t)*p;
    h *= 1099511628211ull;
  }
  return h ? h : 1;
}

bool cw__res_canon(const char *in, char *out, size_t cap) {
  if (!in) return false;
  if (!strncmp(in, "res://", 6)) in += 6;
  size_t k = 0;
  const char *p = in;
  while (*p == '/') p++;
  while (*p) {
    /* one segment */
    const char *e = p;
    while (*e && *e != '/' && *e != '#') e++;
    size_t n = (size_t)(e - p);
    if (n == 1 && p[0] == '.') { /* skip "." */
    } else if (n == 2 && p[0] == '.' && p[1] == '.') {
      /* "a/b/../c" is "a/c"; above the asset root is out of bounds, as in a
       * cart bundle */
      if (!k) return false;
      while (k && out[k - 1] != '/') k--;
      if (k) k--;
    } else if (n) {
      if (k && out[k - 1] != '/') { if (k + 1 >= cap) return false; out[k++] = '/'; }
      if (k + n >= cap) return false;
      memcpy(out + k, p, n);
      k += n;
    }
    if (*e == '#') { /* the sub-resource name is kept verbatim */
      size_t m = strlen(e);
      if (!k || k + m >= cap) return false;
      memcpy(out + k, e, m);
      k += m;
      break;
    }
    p = *e ? e + 1 : e;
    while (*p == '/') p++;
  }
  if (!k) return false;
  for (size_t i = 0; i < k; i++)
    if (out[i] == '\\') return false;
  out[k] = 0;
  return true;
}
