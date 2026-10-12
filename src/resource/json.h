/* A small JSON document model for the asset pipeline: parse, edit, write.
 *
 * Shared by the engine (glTF rewriting at load: meshopt decode, image
 * placeholders) and cwpack (glTF import). Object keys keep their order and
 * numbers keep their original text, so parse + write of an unedited document
 * reproduces it exactly (minus whitespace), and output is byte-identical for
 * identical input on every host.
 *
 * Internal (src/), not a public engine API. Memory: CW_MEM_RESOURCE. */
#ifndef CW_RESOURCE_JSON_H
#define CW_RESOURCE_JSON_H

#include "cartwheel/core.h"

typedef enum { CWJ_NULL, CWJ_BOOL, CWJ_NUM, CWJ_STR, CWJ_ARR, CWJ_OBJ } cwj_type;

typedef struct cwj cwj;
struct cwj {
  cwj_type type;
  bool b;
  double num;
  char *text;      /* CWJ_STR: the string (unescaped); CWJ_NUM: original text or NULL */
  int count, cap;  /* CWJ_ARR / CWJ_OBJ */
  char **keys;     /* CWJ_OBJ */
  cwj **items;
};

/* NULL on a syntax error, with "line N: what" in err. */
cwj *cwj_parse(const char *s, size_t len, char *err, size_t err_cap);
void cwj_free(cwj *v);
/* Compact JSON, cw_alloc'd, NUL-terminated (not counted in *len). */
char *cwj_write(const cwj *v, size_t *len);

cwj *cwj_null(void);
cwj *cwj_bool(bool b);
cwj *cwj_num(double n);
cwj *cwj_numf(float f); /* written with %.9g: a float that round-trips, no float noise */
cwj *cwj_str(const char *s);
cwj *cwj_arr(void);
cwj *cwj_obj(void);
cwj *cwj_clone(const cwj *v);

cwj *cwj_get(const cwj *obj, const char *key); /* NULL when absent or not an object */
cwj *cwj_at(const cwj *arr, int i);            /* NULL when out of range */
void cwj_set(cwj *obj, const char *key, cwj *val); /* replaces (in place) or appends; takes val */
void cwj_remove(cwj *obj, const char *key);
void cwj_push(cwj *arr, cwj *val);               /* takes val */

double cwj_num_or(const cwj *v, double def);
int64_t cwj_int(const cwj *obj, const char *key, int64_t def);
const char *cwj_string(const cwj *obj, const char *key); /* NULL when absent */

#endif
