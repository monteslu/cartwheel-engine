/* glTF documents as editable data: the JSON (json.h) plus every bufferView's
 * bytes, resolved. The engine uses it at load time (decode
 * EXT_meshopt_compression, swap images for placeholders) and cwpack at import
 * time (rewrite optimized meshes, add LODs, compress). Writing always produces
 * one GLB with one buffer holding exactly the bufferViews still referenced, in
 * index order, so the output is a pure function of the document.
 *
 * Internal (src/). Memory: CW_MEM_RESOURCE. */
#ifndef CW_RESOURCE_GLTF_DOC_H
#define CW_RESOURCE_GLTF_DOC_H

#include "json.h"

/* Fetches a file a .gltf references by uri (relative to the .gltf); returns
 * cw_alloc'd bytes (CW_MEM_RESOURCE) or NULL. */
typedef void *(*cw_gltf_read_fn)(void *ctx, const char *uri, size_t *len);

typedef enum { CW_MESHOPT_NONE, CW_MESHOPT_ATTRIBUTES, CW_MESHOPT_TRIANGLES, CW_MESHOPT_INDICES } cw_meshopt_mode;

typedef struct cw_gltf_view {
  const uint8_t *data; /* NULL: not resolvable (a meshopt fallback view before decoding) */
  size_t len;
  uint8_t *owned;      /* data, when the document owns it */
  /* writer: compress this view with EXT_meshopt_compression */
  cw_meshopt_mode compress;
  int count, stride;   /* elements and element size for compress */
} cw_gltf_view;

typedef struct cw_gltf_doc {
  cwj *root;
  cw_gltf_view *views;
  int view_count, view_cap;
  uint8_t **owned_buffers; /* external / data-uri buffers */
  int owned_count;
} cw_gltf_doc;

/* Accepts GLB or .gltf JSON. The file bytes are borrowed: keep them alive
 * until the document is freed. External buffers come through read (NULL =
 * only self-contained files). false with a message in err on failure. */
bool cw_gltf_doc_open(cw_gltf_doc *d, const void *file, size_t len, cw_gltf_read_fn read, void *ctx, char *err,
                      size_t err_cap);
void cw_gltf_doc_free(cw_gltf_doc *d);

/* Decode every EXT_meshopt_compression bufferView in place and drop the
 * extension (and the fallback buffer it implies). A no-op without it. */
bool cw_gltf_doc_meshopt_decode(cw_gltf_doc *d, char *err, size_t err_cap);

/* Append a bufferView owning data (cw_alloc'd, CW_MEM_RESOURCE); returns its
 * index. stride 0 = none; target 0 = none (34962 vertex, 34963 index). */
int cw_gltf_doc_add_view(cw_gltf_doc *d, uint8_t *owned, size_t len, int stride, int target);

/* One GLB: the JSON (compact) + one bin holding every referenced view,
 * 4-byte aligned (16 with compression); unreferenced views are dropped and
 * references renumbered. Views marked for compression are written with
 * EXT_meshopt_compression (vertex codec version 0 as that extension
 * requires). Returns cw_alloc'd bytes. */
uint8_t *cw_gltf_doc_write_glb(cw_gltf_doc *d, size_t *out_len);

/* GLB container helpers. split: json/bin point into data (bin NULL when the
 * file is plain .gltf JSON); false if it is neither. */
bool cw_glb_split(const void *data, size_t len, const char **json, size_t *json_len, const uint8_t **bin,
                  size_t *bin_len);
uint8_t *cw_glb_build(const char *json, size_t json_len, const uint8_t *bin, size_t bin_len, size_t *out_len);

/* Element size of an accessor (componentType x type), 0 if unknown. */
int cw_gltf_accessor_elem_size(const cwj *accessor);

#endif
