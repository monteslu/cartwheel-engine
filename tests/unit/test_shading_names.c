/* A game's shading part may not take a name the engine owns (cartwheel/shading.h cw_shading_part_engine_owned):
 * one name is one program state and one hook, so a game's "foliage" and the engine's replaced each other's shading
 * without a word. Each way a game registers a part (cw_shading_part, cw_shading_part_everywhere,
 * cw_shading_part_traits, cw_shading_part_batching, cw_surface_register) is refused with an error naming the part
 * and the engine module, and a name the engine does not own registers as before.
 *
 * The suite registers the name in CW_TEST_SHADING_NAME (default "foliage") and expects it refused; the gate's control
 * (tests/gates.json shading-names) passes a name the engine does not own, "forest", which registers, so the
 * expectations fail. A second name, "forest" or "foliage" (whichever the first is not), is held to the opposite. */
#include <string.h>

#include "test.h"
#include "cartwheel/shading.h"
#include "cartwheel/surface.h"

static int refused_errors; /* errors that say REFUSED and name the part */
static char watch[32];
static void listen(cw_log_level level, const char *msg, void *user) {
  (void)user;
  if (level == CW_LOG_ERROR && strstr(msg, "REFUSED") && strstr(msg, watch)) refused_errors++;
}
static void fill(t3_extension_ctx *c, void *user) { (void)c; (void)user; }

/* register `name` every way a game can; true when every way was refused with an error */
static bool all_refused(const char *name) {
  snprintf(watch, sizeof watch, "\"%s\"", name);
  refused_errors = 0;
  unsigned before = cw_shading_part_traits_of(name);
  cw_shading_part_traits(name, CW_PART_SURFACE | CW_PART_MOVES_VERTICES);
  bool traits_kept = cw_shading_part_traits_of(name) == before;
  cw_shading_part_everywhere(name, fill, NULL, true);
  cw_shading_part_batching(NULL, name, true);
  static const cw_surface_param p[] = { { .name = "amount", .type = CW_SURFACE_FLOAT } };
  bool registered = cw_surface_register(&(cw_surface_desc){ .name = name, .params = p, .param_count = 1 });
  cw_info("shading_names: %s: owned %d, traits kept %d, surface registered %d, %d refusal error(s)", name,
          cw_shading_part_engine_owned(name), traits_kept, registered, refused_errors);
  return cw_shading_part_engine_owned(name) && traits_kept && !registered && !cw_surface_known(name) && refused_errors >= 4;
}
/* register `name` as a game does; true when it registered with no refusal */
static bool registers(const char *name) {
  snprintf(watch, sizeof watch, "\"%s\"", name);
  refused_errors = 0;
  static const cw_surface_param p[] = { { .name = "amount", .type = CW_SURFACE_FLOAT } };
  bool registered = cw_surface_register(&(cw_surface_desc){ .name = name, .params = p, .param_count = 1 });
  return !cw_shading_part_engine_owned(name) && registered && cw_surface_known(name) &&
         (cw_shading_part_traits_of(name) & CW_PART_SURFACE) && !refused_errors;
}

static void test_owned_list(void) {
  static const char *const owned[] = { "cluster", "csm", "plain", "foliage", "folalpha", "hfog", "gi", "terrain", "water", "meshdecal" };
  for (size_t i = 0; i < CW_ARRAY_LEN(owned); i++) CHECK(cw_shading_part_engine_owned(owned[i]));
  CHECK(!cw_shading_part_engine_owned("forest"));
  CHECK(!cw_shading_part_engine_owned("dissolve"));
}

void cw_test_late(void) {
  cw_log_add_listener(listen, NULL);
  const char *e = cw_test_env("CW_TEST_SHADING_NAME");
  const char *name = e && *e ? e : "foliage";
  CHECK(all_refused(name));
  CHECK(registers(!strcmp(name, "forest") ? "dissolve" : "forest"));
}

CW_TEST_SUITE("shading_names", { "owned_list", test_owned_list })
