/* Editor command tests, natively and as a cart (gate unit-editor):
 *   - EVERY undoable command has an example line, and its do / undo / redo /
 *     undo round trip restores the scene text exactly (a new command
 *     without an example fails this suite, so later modules are covered);
 *   - every demo-project scene loads and saves byte for byte;
 *   - merging, dirty tracking, refusals that change nothing, the same node
 *     coming back on undo of a delete, tokenizer quoting, the debug channel
 *     binding, panels and settings. */
#include "test.h"
#include "internal.h"

static cwed_doc *D(void) { return cwed_doc_active(); }

static char *text(void) { return cwed_doc_text(D()); }

static bool run(const char *line) { return cwed_run(line, NULL); }

/* quoted literal tokens vs key="quoted value" */
static void fresh(void) {
  while (cwed_doc_count()) run("scene.close force=true");
  CHECK(run("scene.open path=main.cwscene"));
  CHECK(run("select paths=Ball"));
}

static void fold_text(const char *t) { cw_test_checksum = cw_hash32(t, strlen(t), cw_test_checksum); }

static void test_init(void) {
  cwed_init(0, NULL);
  CHECK(cwed_command_count() > 30);
  CHECK(cwed__module_count >= 5);
  CHECK(cwed_doc_count() == 1); /* the demo project's main scene opened at startup */
}

static const char *expand(const char *ex) {
  /* {sel} = Ball, {sel2} = Props (demo project) */
  char *o = cw_frame_alloc(strlen(ex) + 64), *w = o;
  for (const char *s = ex; *s;) {
    if (!strncmp(s, "{sel2}", 6)) { w += sprintf(w, "Props"); s += 6; }
    else if (!strncmp(s, "{sel}", 5)) { w += sprintf(w, "Ball"); s += 5; }
    else *w++ = *s++;
  }
  *w = 0;
  return o;
}

/* editor-wide state: the settings, and the project's files (an editor-wide
 * command may move, write or delete project files: fs.*, import.set) */
typedef CW_ARRAY(char) text_buf;
static void snap_file(const char *path, const cw_vfs_entry *e, void *user) {
  (void)e;
  text_buf *t = user;
  size_t n;
  void *b = cw_vfs_read(path, &n);
  const char *line = cw_frame_printf("%s %08x\n", path, b ? cw_hash32(b, n, CW_HASH32_INIT) : 0);
  free(b);
  for (const char *c = line; *c; c++) cw_array_push(t, *c);
}
static char *settings_snapshot(void) {
  char *j;
  cwed_run("settings.list", &j);
  text_buf t = { 0 };
  for (const char *c = j; *c; c++) cw_array_push(&t, *c);
  cw_array_push(&t, '\n');
  cw_vfs_walk("", 0, snap_file, &t);
  cw_array_push(&t, 0);
  cw_free(j);
  char *out = cw_strdup(t.data, CW_MEM_EDITOR);
  cw_array_free(&t);
  return out;
}

static void test_every_command_round_trips(void) {
  int checked = 0;
  for (int i = 0; i < cwed_command_count(); i++) {
    const cwed_command_desc *d = cwed_command_at(i);
    if (!(d->flags & CWED_CMD_UNDO)) continue;
    if (!d->example) {
      cw_error("command %s is undoable but has no .example: add one (editor/include/cwed/editor.h)", d->name);
      CHECK(!"undoable command without an example");
      continue;
    }
    fresh();
    bool scene = d->flags & CWED_CMD_SCENE;
    /* an example of several lines: the first ones set the stage (a
     * connection to disconnect), the last is the command under test */
    const char *line = expand(d->example);
    for (const char *nl; (nl = strchr(line, '\n'));) {
      const char *setup = cw_frame_printf("%.*s", (int)(nl - line), line);
      if (!run(setup)) cw_error("%s: example setup '%s' failed", d->name, setup);
      line = nl + 1;
    }
    cwed_history_seal();
    char *before = scene ? text() : settings_snapshot();
    int undo0 = cwed_history(scene ? D() : NULL).undo_count;
    char *reply;
    bool ok = cwed_run(line, &reply);
    if (!ok) cw_error("example `%s` failed: %s", line, reply);
    cw_free(reply);
    CHECK(ok);
    char *after = scene ? text() : settings_snapshot();
    CHECK(strcmp(before, after) != 0); /* it did something */
    CHECK_EQ_INT(cwed_history(scene ? D() : NULL).undo_count, undo0 + 1);
    CHECK(run(scene ? "edit.undo" : "edit.undo scope=editor"));
    char *undone = scene ? text() : settings_snapshot();
    if (strcmp(before, undone)) cw_error("%s: undo did not restore the scene:\n%s\n---\n%s", d->name, before, undone);
    CHECK(!strcmp(before, undone));
    CHECK(run(scene ? "edit.redo" : "edit.redo scope=editor"));
    char *redone = scene ? text() : settings_snapshot();
    if (strcmp(after, redone)) cw_error("%s: redo differs from do", d->name);
    CHECK(!strcmp(after, redone));
    CHECK(run(scene ? "edit.undo" : "edit.undo scope=editor"));
    char *again = scene ? text() : settings_snapshot();
    CHECK(!strcmp(before, again));
    if (scene) fold_text(after);
    cw_free(before), cw_free(after), cw_free(undone), cw_free(redone), cw_free(again);
    checked++;
  }
  CHECK(checked >= 9);
  cw_info("round-tripped %d undoable commands", checked);
}

static void test_scenes_lossless(void) {
  const char **sc;
  int n = cwed__project_scenes(&sc);
  CHECK(n >= 3);
  for (int i = 0; i < n; i++) {
    char *j;
    CHECK(cwed_run(cw_frame_printf("scene.check path=%s", sc[i]), &j));
    if (!strstr(j, "\"lossless\":true,\"canonical\":true")) cw_error("%s does not round-trip: %s", sc[i], j);
    CHECK(strstr(j, "\"lossless\":true,\"canonical\":true") != NULL); /* the demo project is saved by the editor: canonical */
    cw_free(j);
    /* open, save under another name, compare the bytes with the original */
    while (cwed_doc_count()) run("scene.close force=true");
    CHECK(run(cw_frame_printf("scene.open path=%s", sc[i])));
    const char *copy = cw_frame_printf("roundtrip/%d.cwscene", i);
    CHECK(run(cw_frame_printf("scene.save path=%s", copy)));
    size_t la, lb;
    char *a = cw_vfs_read(sc[i], &la), *b = cw_vfs_read(copy, &lb);
    CHECK(a && b && la == lb && !memcmp(a, b, la));
    if (a) fold_text(a);
    free(a), free(b);
  }
}

static void test_edit_then_undo_all_is_lossless(void) {
  fresh();
  size_t len;
  char *orig = cw_vfs_read("main.cwscene", &len);
  const char *lines[] = { "node.add type=Node3D parent=. name=Group", "node.reparent path=Ball parent=Group",
                          "node.set path=Group/Ball prop=roughness value=0.9", "node.rename path=Floor name=Ground",
                          "node.delete path=Props/CrateB", "node.instance scene=props/crate.cwscene parent=Group",
                          "node.move path=Group index=0", "node.group path=Ground group=static" };
  for (size_t i = 0; i < CW_ARRAY_LEN(lines); i++) CHECK(run(lines[i]));
  char *edited = text();
  CHECK(strcmp(edited, orig) != 0);
  fold_text(edited);
  CHECK(run("history.goto index=0"));
  char *back = text();
  CHECK(!strcmp(back, orig));
  CHECK(!cwed_doc_dirty(D()));
  CHECK(run(cw_frame_printf("history.goto index=%d", (int)CW_ARRAY_LEN(lines))));
  char *forward = text();
  CHECK(!strcmp(forward, edited));
  free(orig);
  cw_free(edited), cw_free(back), cw_free(forward);
}

static void test_merge(void) {
  fresh();
  int u0 = cwed_history(D()).undo_count;
  CHECK(run("node.set Ball position \"vec3(0, 2, 0)\" merge=drag"));
  CHECK(run("node.set Ball position \"vec3(0, 3, 0)\" merge=drag"));
  CHECK(run("node.set Ball position \"vec3(0, 4, 0)\" merge=drag"));
  CHECK_EQ_INT(cwed_history(D()).undo_count, u0 + 1);
  char *j;
  cwed_run("node.get Ball position", &j);
  CHECK(strstr(j, "vec3(0, 4, 0)") != NULL);
  cw_free(j);
  CHECK(run("edit.undo"));
  cwed_run("node.get Ball position", &j);
  CHECK(strstr(j, "vec3(0, 1.5, -1)") != NULL); /* the value before the whole drag */
  cw_free(j);
  CHECK(run("edit.redo"));
  cwed_run("node.get Ball position", &j);
  CHECK(strstr(j, "vec3(0, 4, 0)") != NULL); /* redo lands on the drag's end */
  cw_free(j);
  /* a seal (pointer release) starts a new step even with the same key */
  cwed_history_seal();
  CHECK(run("node.set Ball position \"vec3(0, 5, 0)\" merge=drag"));
  CHECK_EQ_INT(cwed_history(D()).undo_count, u0 + 2);
  /* another property never merges */
  CHECK(run("node.set Ball roughness 0.5 merge=drag"));
  CHECK_EQ_INT(cwed_history(D()).undo_count, u0 + 3);
}

static void test_dirty(void) {
  fresh();
  CHECK(!cwed_doc_dirty(D()));
  CHECK(run("node.set Ball roughness 0.7"));
  CHECK(cwed_doc_dirty(D()));
  CHECK(run("edit.undo"));
  CHECK(!cwed_doc_dirty(D()));
  CHECK(run("edit.redo"));
  CHECK(run("scene.save path=dirty_test.cwscene"));
  CHECK(!cwed_doc_dirty(D()));
  CHECK(run("node.set Ball roughness 0.6"));
  CHECK(run("scene.save")); /* saving again to where it came from keeps the path */
  CHECK(!strcmp(cwed_doc_path(D()), "dirty_test.cwscene") && !strcmp(cwed_doc_title(D()), "dirty_test.cwscene"));
  CHECK(run("edit.undo"));
  CHECK(cwed_doc_dirty(D()));
  CHECK(run("edit.redo"));
  CHECK(run("edit.undo"));
  CHECK(cwed_doc_dirty(D())); /* the file has the change; the scene does not */
  CHECK(!run("scene.close"));   /* refused: unsaved */
  CHECK(run("scene.close force=true"));
}

static void test_delete_keeps_the_node(void) {
  fresh();
  cw_node ball = cwed_doc_node(D(), "Ball");
  uint64_t uid = cw_node_uid(ball);
  CHECK(run("node.delete Ball"));
  CHECK(CW_HANDLE_IS_NULL(cwed_doc_node(D(), "Ball")));
  CHECK(cwed_selection_count() == 0); /* the selection drops deleted nodes */
  CHECK(run("edit.undo"));
  cw_node back = cwed_doc_node(D(), "Ball");
  CHECK(back.id == ball.id && cw_node_uid(back) == uid);
}

static void test_refusals_change_nothing(void) {
  fresh();
  char *t0 = text();
  int u0 = cwed_history(D()).undo_count;
  const char *bad[] = { "nosuch.command", "node.set", "node.set Ball", "node.delete .", "node.delete Nope",
                        "node.set Ball position notavalue", "node.set Ball nosuchprop 1", "node.add NoSuchType",
                        "node.reparent Props Props/CrateA", "node.rename Props/CrateA/Box Other", "node.delete Props/CrateB/Box",
                        "node.set Ball primitive WRONG", "node.instance main.cwscene", "scene.open ../escape.cwscene",
                        "node.add Node3D . a b c d e", "select paths=Nope" };
  for (size_t i = 0; i < CW_ARRAY_LEN(bad); i++) {
    char *j;
    bool ok = cwed_run(bad[i], &j);
    if (ok) cw_error("`%s` should have failed: %s", bad[i], j);
    CHECK(!ok);
    CHECK(strstr(j, "\"ok\":false") && strstr(j, "\"error\":"));
    cw_free(j);
  }
  char *t1 = text();
  CHECK(!strcmp(t0, t1));
  CHECK_EQ_INT(cwed_history(D()).undo_count, u0);
  cw_free(t0), cw_free(t1);
}

static void test_quoting(void) {
  const char *cases[] = { "plain", "two words", "say \"hi\"", "back\\slash", "a=b", "", "multi\nline", "vec3(1, 2, 3)" };
  for (size_t i = 0; i < CW_ARRAY_LEN(cases); i++) {
    const char *line = cw_frame_printf("x %s", cwed_quote(cases[i]));
    const char *tok[4];
    bool q[4];
    int n = cwed__tokenize(line, tok, q, 4);
    CHECK_EQ_INT(n, 2);
    CHECK(n == 2 && !strcmp(tok[1], cases[i]));
  }
  /* key="quoted value" is keyed; a token that starts with a quote is literal */
  {
    const char *tok[4];
    bool q[4];
    CHECK_EQ_INT(cwed__tokenize("x name=\"Big Ball\" \"a=b\"", tok, q, 4), 3);
    CHECK(!strcmp(tok[1], "name=Big Ball") && !q[1]);
    CHECK(!strcmp(tok[2], "a=b") && q[2]);
  }
  /* a string property set through a quoted line keeps its quotes and spaces */
  fresh();
  CHECK(run("scene.open ui/hud.cwscene"));
  CHECK(run("node.set Box/Title text \"She said \\\"go\\\" twice\""));
  char *j;
  cwed_run("node.get Box/Title text", &j);
  CHECK(strstr(j, "She said \\\\\\\"go\\\\\\\" twice") != NULL || strstr(j, "She said") != NULL);
  cw_free(j);
  char *t = text();
  CHECK(strstr(t, "text = \"She said \\\"go\\\" twice\"") != NULL);
  fold_text(t);
  cw_free(t);
}

static void test_debug_channel(void) {
  fresh();
  char *r = cw_debug_run("ed node.set Ball position vec3(1, 2, 3)");
  CHECK(strstr(r, "\"ok\":true") && strstr(r, "vec3(1, 2, 3)"));
  cw_free(r);
  r = cw_debug_run("ed node.set path=Ball prop=roughness value=0.25");
  CHECK(strstr(r, "\"ok\":true") != NULL);
  cw_free(r);
  const char *line = "node.rename path=Ball name=\"Big Ball\"";
  char *b64 = cw_base64_encode(line, strlen(line));
  r = cw_debug_run(cw_frame_printf("ed64 %s", b64));
  CHECK(strstr(r, "Big Ball") != NULL);
  cw_free(r);
  cw_free(b64);
  r = cw_debug_run("ed history.list");
  CHECK(strstr(r, "node.rename path=Ball name=\\\"Big Ball\\\"") != NULL);
  cw_free(r);
  r = cw_debug_run("ed nosuch");
  CHECK(strstr(r, "\"ok\":false") != NULL);
  cw_free(r);
  r = cw_debug_run("ed help node.");
  CHECK(strstr(r, "\"params\"") && strstr(r, "node.set"));
  cw_free(r);
}

static void test_history_replays(void) {
  /* the done history lines, replayed on a freshly opened scene, rebuild it */
  fresh();
  const char *lines[] = { "node.add type=Node3D parent=. name=Extra", "node.set path=Extra prop=position value=\"vec3(1, 0, 0)\"",
                          "node.rename path=Floor name=Ground", "node.reparent path=Ball parent=Extra" };
  for (size_t i = 0; i < CW_ARRAY_LEN(lines); i++) CHECK(run(lines[i]));
  char *want = text();
  CW_ARRAY(char *) hist = { 0 };
  for (int i = 0;; i++) {
    bool done;
    const char *l = cwed_history_line(D(), i, &done);
    if (!l) break;
    if (done) cw_array_push(&hist, cw_strdup(l, CW_MEM_EDITOR));
  }
  run("scene.close force=true");
  CHECK(run("scene.open main.cwscene"));
  for (int i = 0; i < hist.count; i++) {
    CHECK(run(hist.data[i]));
    cw_free(hist.data[i]);
  }
  cw_array_free(&hist);
  char *got = text();
  CHECK(!strcmp(want, got));
  cw_free(want), cw_free(got);
}

static void test_panels_and_settings(void) {
  char *j;
  CHECK(run("panel.move id=history slot=right_bottom"));
  cwed_run("panel.list", &j);
  CHECK(strstr(j, "\"id\":\"history\",\"title\":\"History\",\"slot\":\"right_bottom\"") != NULL);
  cw_free(j);
  { /* history is among right_bottom's panels (other modules may dock there too) */
    const char *rb = strstr(cwed_setting("layout.panels"), "right_bottom:"), *h = rb ? strstr(rb, "history") : NULL;
    const char *bar = rb ? strchr(rb, '|') : NULL;
    CHECK(h != NULL && (!bar || h < bar));
  }
  CHECK(run("layout.reset"));
  cwed_run("panel.list", &j);
  CHECK(strstr(j, "\"id\":\"history\",\"title\":\"History\",\"slot\":\"left_bottom\"") != NULL);
  cw_free(j);
  CHECK(run("settings.set interface.theme light"));
  CHECK(!strcmp(cwed_setting("interface.theme"), "light"));
  CHECK(run("edit.undo scope=editor"));
  CHECK(!strcmp(cwed_setting("interface.theme"), "dark"));
  CHECK(!run("settings.set interface.theme purple")); /* not a choice */
  CHECK(!run("settings.set interface.scale 9"));      /* out of range */
#if CW_CART
  /* a cart persists settings in the project layer (exported with vfs export) */
  CHECK(run("settings.set interface.font_size 16"));
  CHECK(run("settings.save"));
  size_t len;
  char *f = cw_vfs_read(".cweditor/settings.cfg", &len);
  CHECK(f && strstr(f, "interface.font_size = 16"));
  free(f);
  CHECK(run("edit.undo scope=editor"));
#else
  /* natively a test run (CW_FRAMES) persists nothing unless CW_EDITOR_SETTINGS names a file */
  CHECK(run("settings.set interface.font_size 16"));
  char *sl;
  cwed_run("settings.list", &sl);
  bool persisted = strstr(sl, "\"persisted\":true") != NULL;
  cw_free(sl);
  CHECK(run("settings.save") == persisted);
  CHECK(getenv("CW_EDITOR_SETTINGS") ? persisted : !persisted);
  CHECK(run("edit.undo scope=editor"));
#endif
}

static void test_no_play_backend(void) {
  /* the play backend is registered; the demo project names no game,
   * so Play refuses and says how to name one (natively), or that the cart
   * editor has no transport at all */
  char *j;
  CHECK(!cwed_run("play.start", &j));
#if CW_CART
  CHECK(strstr(j, "not available in the cart editor") != NULL);
#else
  CHECK(strstr(j, "game_app") != NULL);
#endif
  cw_free(j);
  CHECK(!cwed_run("play.step", &j)); /* no game running: an error, not silence */
  CHECK(strstr(j, "play.start") != NULL || strstr(j, "cart editor") != NULL);
  cw_free(j);
  CHECK(cwed_run("play.status", &j));
  CHECK(strstr(j, "\"state\":\"idle\"") != NULL);
  cw_free(j);
}

CW_TEST_SUITE("editor", { "init", test_init }, { "every_command_round_trips", test_every_command_round_trips },
              { "scenes_lossless", test_scenes_lossless }, { "edit_then_undo_all", test_edit_then_undo_all_is_lossless },
              { "merge", test_merge }, { "dirty", test_dirty }, { "delete_keeps_node", test_delete_keeps_the_node },
              { "refusals", test_refusals_change_nothing }, { "quoting", test_quoting },
              { "debug_channel", test_debug_channel }, { "history_replays", test_history_replays },
              { "panels_settings", test_panels_and_settings }, { "no_play_backend", test_no_play_backend })
