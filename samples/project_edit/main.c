/* project_edit: spike S0.5, an editor-as-cart round trip with no new ABI.
 *
 * The cart runs scene3d's level. `project save <node path> <file>` writes a
 * subtree as .cwscene text into the writable project layer (cartwheel/vfs.h);
 * in a cart that layer lives in memory, and `vfs export` carries every change
 * out as one archive through the debug channel. Natively, CW_PROJECT makes
 * the same writes land on disk. tests/vfs.mjs edits the level inside a cart
 * through romdev, exports, imports into a native run and checks the file. */
#include "cartwheel/debug.h"
#include "cartwheel/scene_file.h"
#include "cartwheel/vfs.h"

/* #region command */
static void cmd_project(int argc, const char **argv, cw_reply *r) {
  if (argc < 4 || strcmp(argv[1], "save")) { cw_reply_error(r, "usage: project save <node path> <file.cwscene>"); return; }
  cw_node n = cw_node_get(cw_tree_root(cw_scene_app_tree()), argv[2]);
  if (CW_HANDLE_IS_NULL(n)) { cw_reply_error(r, "no node %s", argv[2]); return; }
  char *text = cw_scene_save(n);
  bool ok = cw_vfs_write(argv[3], text, strlen(text));
  cw_reply_printf(r, "{\"saved\":%s,\"bytes\":%zu}", ok ? "true" : "false", strlen(text));
  cw_free(text);
}
/* #endregion */

static void init(void *u) {
  (void)u;
  /* #region register */
  cw_debug_command("project", "project save <node path> <file.cwscene>: write a subtree into the project", cmd_project);
  /* #endregion */
}

cw_app_desc cw_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  static cw_app_desc base = { .title = "project_edit", .width = 320, .height = 180, .debug = true, .init = init };
  return cw_scene_app("main.cwscene", &base);
}
