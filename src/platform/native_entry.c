/* The native entry point, in its own object: a static-library member that is
 * linked only when the executable has no main() of its own (the hot-reload
 * host, tools/cwhot, brings its own). SDL.h renames main to SDL_main where
 * SDL owns the real entry point (Windows' WinMain via SDL2main, Android's
 * SDLActivity, iOS); elsewhere it is plain main. */
#include <SDL.h>
#include <stdlib.h>
#include <string.h>

#include "cartwheel/platform.h"

int cw_native_main(int argc, char **argv, cw_app_desc (*app_fn)(int, char **));

int main(int argc, char **argv) {
#if defined(__ANDROID__)
  /* Android has no command line or environment to launch with: a debuggable
   * build's activity passes "--cw-env KEY=VALUE;..." (from an intent extra,
   * see the export template's CartwheelActivity) and this sets them, so the
   * test hooks (CW_REMOTE, ...) work on a device */
  for (int i = 1; i + 1 < argc; i++) {
    if (strcmp(argv[i], "--cw-env")) continue;
    char *all = strdup(argv[i + 1]), *save = NULL;
    for (char *kv = strtok_r(all, ";", &save); kv; kv = strtok_r(NULL, ";", &save)) {
      char *eq = strchr(kv, '=');
      if (!eq || eq == kv) continue;
      *eq = 0;
      setenv(kv, eq + 1, 1);
    }
    free(all);
  }
#endif
  return cw_native_main(argc, argv, cw_main);
}
