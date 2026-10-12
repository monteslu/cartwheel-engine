#include <stdio.h>
#include <string.h>

#include "cartwheel/debug.h"

#define MAX_FIELDS 512

/* Same layout as wasmcart's wc_debug_field_t (16 bytes, 32-bit pointers):
 * in a cart this array IS the table wc_debug_state() returns. */
typedef struct {
  uint32_t name_ptr, value_ptr;
  uint8_t type, _pad[3];
  uint32_t len;
} wc_field;

static struct { const char *name; void *ptr; cw_debug_type type; uint32_t len; } fields[MAX_FIELDS];
static int field_count;
#if CW_CART
static wc_field table[MAX_FIELDS + 1];
#endif

void cw_debug_field(const char *name, void *ptr, cw_debug_type type, uint32_t len) {
  for (int i = 0; i < field_count; i++)
    if (!strcmp(fields[i].name, name)) { /* re-registration moves the field */
      fields[i].ptr = ptr; fields[i].type = type; fields[i].len = len;
      return;
    }
  CW_ASSERT(field_count < MAX_FIELDS, "more than %d debug fields", MAX_FIELDS);
  fields[field_count].name = name;
  fields[field_count].ptr = ptr;
  fields[field_count].type = type;
  fields[field_count].len = len ? len : 1;
  field_count++;
}

#if CW_CART
__attribute__((export_name("wc_debug_state"))) wc_field *wc_debug_state(void) {
  for (int i = 0; i < field_count; i++) {
    table[i].name_ptr = (uint32_t)(uintptr_t)fields[i].name;
    table[i].value_ptr = (uint32_t)(uintptr_t)fields[i].ptr;
    table[i].type = (uint8_t)fields[i].type;
    table[i].len = fields[i].len;
  }
  memset(&table[field_count], 0, sizeof table[0]);
  return table;
}
#endif

static void put_value(FILE *f, cw_debug_type t, const void *p) {
  switch (t) {
  case CW_DBG_U8: fprintf(f, "%u", *(const uint8_t *)p); break;
  case CW_DBG_I8: fprintf(f, "%d", *(const int8_t *)p); break;
  case CW_DBG_U16: fprintf(f, "%u", *(const uint16_t *)p); break;
  case CW_DBG_I16: fprintf(f, "%d", *(const int16_t *)p); break;
  case CW_DBG_U32: fprintf(f, "%u", *(const uint32_t *)p); break;
  case CW_DBG_I32: fprintf(f, "%d", *(const int32_t *)p); break;
  case CW_DBG_F32: fprintf(f, "%.17g", (double)*(const float *)p); break; /* exact, as JS reads it */
  case CW_DBG_F64: fprintf(f, "%.17g", *(const double *)p); break;
  default: break;
  }
}

static const int type_size[] = { 1, 1, 2, 2, 4, 4, 4, 8, 1 };

void cw_debug_dump_json(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) { cw_error("debug dump: cannot write %s", path); return; }
  fputc('{', f);
  bool first = true;
  for (int i = 0; i < field_count; i++) {
    if (!strncmp(fields[i].name, "dbg.", 4)) continue; /* the debug channel's mailboxes */
    fprintf(f, "%s\"%s\":", first ? "" : ",", fields[i].name);
    first = false;
    if (fields[i].type == CW_DBG_BYTES) {
      fputc('"', f);
      for (uint32_t k = 0; k < fields[i].len; k++) fprintf(f, "%02x", ((const uint8_t *)fields[i].ptr)[k]);
      fputc('"', f);
    } else if (fields[i].len == 1) {
      put_value(f, fields[i].type, fields[i].ptr);
    } else {
      fputc('[', f);
      for (uint32_t k = 0; k < fields[i].len; k++) {
        if (k) fputc(',', f);
        put_value(f, fields[i].type, (const uint8_t *)fields[i].ptr + k * type_size[fields[i].type]);
      }
      fputc(']', f);
    }
  }
  fputs("}\n", f);
  fclose(f);
}

/* The channel's "fields [name...]" command: the named debug fields' values
 * (every field but the channel's own dbg.* mailboxes when none is named), as
 * one JSON object, the same values CW_DEBUG_DUMP writes. */
static void reply_value(cw_reply *r, cw_debug_type t, const void *p) {
  switch (t) {
  case CW_DBG_U8: cw_reply_printf(r, "%u", *(const uint8_t *)p); break;
  case CW_DBG_I8: cw_reply_printf(r, "%d", *(const int8_t *)p); break;
  case CW_DBG_U16: cw_reply_printf(r, "%u", *(const uint16_t *)p); break;
  case CW_DBG_I16: cw_reply_printf(r, "%d", *(const int16_t *)p); break;
  case CW_DBG_U32: cw_reply_printf(r, "%u", *(const uint32_t *)p); break;
  case CW_DBG_I32: cw_reply_printf(r, "%d", *(const int32_t *)p); break;
  case CW_DBG_F32: cw_reply_printf(r, "%.17g", (double)*(const float *)p); break;
  case CW_DBG_F64: cw_reply_printf(r, "%.17g", *(const double *)p); break;
  default: cw_reply_raw(r, "null"); break;
  }
}

void cw__debug_cmd_fields(int argc, const char **argv, cw_reply *r) {
  for (int a = 1; a < argc; a++) {
    bool found = false;
    for (int i = 0; i < field_count && !found; i++) found = !strcmp(fields[i].name, argv[a]);
    if (!found) { cw_reply_error(r, "no debug field '%s' (fields with no names lists them all)", argv[a]); return; }
  }
  cw_reply_raw(r, "{");
  bool first = true;
  for (int i = 0; i < field_count; i++) {
    bool want = argc <= 1 ? strncmp(fields[i].name, "dbg.", 4) != 0 : false;
    for (int a = 1; a < argc && !want; a++) want = !strcmp(fields[i].name, argv[a]);
    if (!want) continue;
    cw_reply_printf(r, "%s\"%s\":", first ? "" : ",", fields[i].name);
    first = false;
    if (fields[i].type == CW_DBG_BYTES) {
      cw_reply_raw(r, "\"");
      for (uint32_t k = 0; k < fields[i].len; k++) cw_reply_printf(r, "%02x", ((const uint8_t *)fields[i].ptr)[k]);
      cw_reply_raw(r, "\"");
    } else if (fields[i].len == 1) {
      reply_value(r, fields[i].type, fields[i].ptr);
    } else {
      cw_reply_raw(r, "[");
      for (uint32_t k = 0; k < fields[i].len; k++) {
        if (k) cw_reply_raw(r, ",");
        reply_value(r, fields[i].type, (const uint8_t *)fields[i].ptr + k * type_size[fields[i].type]);
      }
      cw_reply_raw(r, "]");
    }
  }
  cw_reply_raw(r, "}");
}
