/* vpad: virtual game controllers through Linux uinput, for tests of a
 * player's real controller path (SDL's joystick driver, hot-plug included).
 * Commands on stdin, one per line; each answers "ok" (or "error ...") on stdout:
 *   add <slot>                 plug in controller <slot> (0-7)
 *   del <slot>                 unplug it
 *   btn <slot> <name> <0|1>    south east west north l r back start guide l3 r3
 *   abs <slot> <axis> <value>  lx ly rx ry (-32768..32767), lt rt (0..255), hatx haty (-1..1)
 *   wait <ms>                  sleep
 * Every controller is vendor 0x1209 product 0x7764 (SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT
 * keeps a run to these), named "vpad <slot>", with an Xbox 360 pad's buttons and axes.
 * EOF unplugs everything. Needs write access to /dev/uinput. */
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SLOTS 8
static int fd[SLOTS];
static const struct { const char *n; int code; } BTN[] = {
  /* xpad's codes: the west face button is BTN_X and the north one BTN_Y (in
   * linux/input-event-codes.h BTN_X is BTN_NORTH and BTN_Y is BTN_WEST, which
   * is what SDL's mapping of such a pad follows) */
  { "south", BTN_A }, { "east", BTN_B }, { "west", BTN_X }, { "north", BTN_Y },
  { "l", BTN_TL }, { "r", BTN_TR }, { "back", BTN_SELECT }, { "start", BTN_START },
  { "guide", BTN_MODE }, { "l3", BTN_THUMBL }, { "r3", BTN_THUMBR } };
static const struct { const char *n; int code, min, max; } AX[] = {
  { "lx", ABS_X, -32768, 32767 }, { "ly", ABS_Y, -32768, 32767 }, { "lt", ABS_Z, 0, 255 },
  { "rx", ABS_RX, -32768, 32767 }, { "ry", ABS_RY, -32768, 32767 }, { "rt", ABS_RZ, 0, 255 },
  { "hatx", ABS_HAT0X, -1, 1 }, { "haty", ABS_HAT0Y, -1, 1 } };
#define N(a) (int)(sizeof a / sizeof a[0])

static void emit(int f, int type, int code, int value) {
  struct input_event e;
  memset(&e, 0, sizeof e);
  e.type = type, e.code = code, e.value = value;
  if (write(f, &e, sizeof e) != sizeof e) perror("vpad: write");
}

static const char *add(int s) {
  if (fd[s] > 0) return "already plugged in";
  int f = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (f < 0) return strerror(errno);
  ioctl(f, UI_SET_EVBIT, EV_KEY);
  ioctl(f, UI_SET_EVBIT, EV_ABS);
  for (int i = 0; i < N(BTN); i++) ioctl(f, UI_SET_KEYBIT, BTN[i].code);
  for (int i = 0; i < N(AX); i++) {
    struct uinput_abs_setup a;
    memset(&a, 0, sizeof a);
    a.code = AX[i].code;
    a.absinfo.minimum = AX[i].min, a.absinfo.maximum = AX[i].max;
    ioctl(f, UI_SET_ABSBIT, AX[i].code);
    if (ioctl(f, UI_ABS_SETUP, &a) < 0) { close(f); return "UI_ABS_SETUP failed"; }
  }
  struct uinput_setup u;
  memset(&u, 0, sizeof u);
  u.id.bustype = BUS_USB, u.id.vendor = 0x1209, u.id.product = 0x7764, u.id.version = 1;
  snprintf(u.name, sizeof u.name, "vpad %d", s);
  if (ioctl(f, UI_DEV_SETUP, &u) < 0 || ioctl(f, UI_DEV_CREATE) < 0) { close(f); return strerror(errno); }
  fd[s] = f;
  return NULL;
}

int main(void) {
  char line[256];
  setvbuf(stdout, NULL, _IOLBF, 0);
  while (fgets(line, sizeof line, stdin)) {
    char cmd[16] = "", a[16] = "";
    int s = -1, v = 0;
    const char *err = NULL;
    int n = sscanf(line, "%15s %d %15s %d", cmd, &s, a, &v);
    if (n < 1) continue;
    if (!strcmp(cmd, "wait")) {
      struct timespec t = { s / 1000, (long)(s % 1000) * 1000000L };
      nanosleep(&t, NULL);
    } else if (s < 0 || s >= SLOTS) err = "bad slot";
    else if (!strcmp(cmd, "add")) err = add(s);
    else if (!strcmp(cmd, "del")) {
      if (fd[s] > 0) { ioctl(fd[s], UI_DEV_DESTROY); close(fd[s]); fd[s] = 0; } else err = "not plugged in";
    } else if (fd[s] <= 0) err = "not plugged in";
    else if (!strcmp(cmd, "btn") || !strcmp(cmd, "abs")) {
      int code = -1;
      if (cmd[0] == 'b') { for (int i = 0; i < N(BTN); i++) if (!strcmp(a, BTN[i].n)) code = BTN[i].code; }
      else for (int i = 0; i < N(AX); i++) if (!strcmp(a, AX[i].n)) code = AX[i].code;
      if (code < 0) err = "unknown button or axis";
      else { emit(fd[s], cmd[0] == 'b' ? EV_KEY : EV_ABS, code, v); emit(fd[s], EV_SYN, SYN_REPORT, 0); }
    } else err = "unknown command";
    if (err) printf("error %s\n", err); else printf("ok\n");
  }
  for (int s = 0; s < SLOTS; s++) if (fd[s] > 0) { ioctl(fd[s], UI_DEV_DESTROY); close(fd[s]); }
  return 0;
}
