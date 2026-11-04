/* check-prompts control: every line marked FLAG must be reported, and nothing else.
 * (A comment that says "press A" is not text a player reads.) */
static const char *const HINTS[] = {
  "A: jump",                       /* FLAG */
  "(A) to ride",                   /* FLAG */
  "[B] back",                      /* FLAG */
  "A / X change weapon",           /* FLAG */
  "Flick the stick or A B X Y",    /* FLAG */
  "press A",                       /* FLAG */
  "Press X to dash",               /* FLAG */
  "A or Enter: join",              /* FLAG */
  "hold the Y button",             /* FLAG */
  "B button: leave",               /* FLAG */
  "LB / RB: switch sides",         /* FLAG */
  "RT: gas",                       /* FLAG */
  "R2 to brake",                   /* FLAG */
  "ZL",                            /* FLAG */
  "Cross: confirm",                /* FLAG */
  "press Square",                  /* FLAG */
  "SELECT: checkpoint",            /* FLAG */
  "the green button",              /* FLAG */
  "press red to stop",             /* FLAG */
  "P%d: (A) to join",              /* FLAG */
  "Y to rotate",                   /* FLAG */
  "(a) resume",                    /* FLAG */
  "(b): back to the menu",         /* FLAG */
  "P%d: ( x ) to join",            /* FLAG */
  "{pause} pause   (y) map",       /* FLAG */
};
static void hud(void) {
  draw("A: bomb"); draw("{jump}: hop"); /* FLAG: one finding for the line */
  cw_error("press A"); /* a log line: the developer reads it */
}
