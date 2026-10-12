/* Captions and subtitles (cartwheel/a11y.h): caption tracks timed against a
 * voice or the frame clock, one-off sound captions, and CaptionPanel, the
 * Control that draws them. */
#include "ui_internal.h"
#include "cartwheel/a11y.h"
#include "cartwheel/l10n.h"

#define MAX_TRACKS 32
#define MAX_SHOWN 8

typedef struct cue { float start, end; char *speaker, *text; } cue;
typedef struct track {
  char *name;
  CW_ARRAY(cue) cues;
  bool playing;
  cw_voice voice;
  cw_sound sound;
  double t;          /* clock (frame-time tracks) */
  int announced;     /* last cue index announced */
} track;

typedef struct oneoff { char speaker[96], text[256]; float left; } oneoff;

static struct {
  track tracks[MAX_TRACKS];
  int ntracks;
  oneoff shown[MAX_SHOWN];
  int nshown;
  bool disabled;
} C;

static char *dup_range(const char *s, const char *e) {
  while (s < e && (*s == ' ' || *s == '\t')) s++;
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
  char *o = cw_alloc((size_t)(e - s) + 1, CW_MEM_UI);
  memcpy(o, s, (size_t)(e - s));
  o[e - s] = 0;
  return o;
}

int cw_captions_parse(const char *text, size_t len, const char *name) {
  if (C.ntracks >= MAX_TRACKS) {
    cw_error("captions: more than %d tracks", MAX_TRACKS);
    return -1;
  }
  track tr = { 0 };
  tr.name = cw_strdup(name ? name : "captions", CW_MEM_UI);
  int lineno = 0;
  for (const char *p = text, *end = text + len; p < end;) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    const char *le = nl ? nl : end;
    lineno++;
    const char *s = p;
    p = nl ? nl + 1 : end;
    while (s < le && (*s == ' ' || *s == '\t')) s++;
    if (s >= le || *s == '#' || *s == '\r') continue;
    const char *bar = memchr(s, '|', (size_t)(le - s));
    char head[256];
    snprintf(head, sizeof head, "%.*s", bar ? (int)(bar - s) : (int)(le - s), s);
    float a, b;
    char spk[128] = "";
    int got = sscanf(head, "%f %f %127s", &a, &b, spk);
    if (!bar || got < 2 || b < a) {
      cw_error("captions: %s:%d: expected 'start end [speaker] | text' (seconds, end >= start)", tr.name, lineno);
      for (int i = 0; i < tr.cues.count; i++) cw_free(tr.cues.data[i].speaker), cw_free(tr.cues.data[i].text);
      cw_array_free(&tr.cues);
      cw_free(tr.name);
      return -1;
    }
    cue c = { a, b, cw_strdup(got >= 3 ? spk : "", CW_MEM_UI), dup_range(bar + 1, le) };
    cw_array_push(&tr.cues, c);
  }
  tr.announced = -1;
  C.tracks[C.ntracks] = tr;
  return C.ntracks++;
}

int cw_captions_load(const char *path) {
  size_t len;
  char *t = cw_asset_load(path, &len);
  if (!t) {
    cw_error("captions: '%s' not found", path);
    return -1;
  }
  int id = cw_captions_parse(t, len, path);
  free(t);
  return id;
}

static track *T(int id) { return id >= 0 && id < C.ntracks ? &C.tracks[id] : NULL; }

void cw_captions_play(int id, cw_voice voice, cw_sound sound) {
  track *t = T(id);
  if (!t) return;
  t->playing = true;
  t->voice = voice;
  t->sound = sound;
  t->t = 0;
  t->announced = -1;
}
void cw_captions_stop(int id) {
  track *t = T(id);
  if (t) t->playing = false;
}
bool cw_captions_playing(int id) {
  track *t = T(id);
  return t && t->playing;
}

void cw_captions_show(const char *text, const char *speaker, float seconds) {
  if (C.nshown == MAX_SHOWN) {
    memmove(C.shown, C.shown + 1, sizeof C.shown[0] * (MAX_SHOWN - 1));
    C.nshown--;
  }
  oneoff *o = &C.shown[C.nshown++];
  snprintf(o->text, sizeof o->text, "%s", text ? text : "");
  snprintf(o->speaker, sizeof o->speaker, "%s", speaker ? speaker : "");
  o->left = seconds > 0 ? seconds : 2;
  if (!C.disabled) cw_a11y_announce(cw_tr_auto(o->text), false);
}

void cw_captions_set_enabled(bool on) { C.disabled = !on; }
bool cw_captions_enabled(void) { return !C.disabled; }

/* the track's clock in seconds */
static double clock_of(track *t) {
  if (!CW_HANDLE_IS_NULL(t->voice)) {
    int64_t f = cw_voice_frames_played(t->voice);
    int rate = CW_HANDLE_IS_NULL(t->sound) ? cw_audio_sample_rate() : cw_sound_source_rate(t->sound);
    if (f >= 0 && rate > 0) return (double)f / rate;
  }
  return t->t;
}

void cw_captions_update(double dt) {
  for (int i = 0; i < C.nshown;) {
    C.shown[i].left -= (float)dt;
    if (C.shown[i].left <= 0) {
      memmove(C.shown + i, C.shown + i + 1, sizeof C.shown[0] * (size_t)(C.nshown - i - 1));
      C.nshown--;
    } else i++;
  }
  for (int k = 0; k < C.ntracks; k++) {
    track *t = &C.tracks[k];
    if (!t->playing) continue;
    t->t += dt;
    double now = clock_of(t);
    for (int c = 0; c < t->cues.count; c++)
      if (now >= t->cues.data[c].start && now < t->cues.data[c].end && c > t->announced) {
        t->announced = c;
        if (!C.disabled) cw_a11y_announce(cw_tr_auto(t->cues.data[c].text), false);
      }
    bool voice_done = !CW_HANDLE_IS_NULL(t->voice) && !cw_voice_is_playing(t->voice);
    if (voice_done || (t->cues.count && now >= t->cues.data[t->cues.count - 1].end)) t->playing = false;
  }
}

int cw_captions_active(cw_caption_line *out, int cap) {
  int n = 0;
  for (int k = 0; k < C.ntracks; k++) {
    track *t = &C.tracks[k];
    if (!t->playing) continue;
    double now = clock_of(t);
    for (int c = 0; c < t->cues.count && n < cap; c++) {
      cue *q = &t->cues.data[c];
      if (now >= q->start && now < q->end)
        out[n++] = (cw_caption_line){ *q->speaker ? cw_tr_auto(q->speaker) : "", cw_tr_auto(q->text), (float)(q->end - now), k };
    }
  }
  for (int i = 0; i < C.nshown && n < cap; i++)
    out[n++] = (cw_caption_line){ *C.shown[i].speaker ? cw_tr_auto(C.shown[i].speaker) : "", cw_tr_auto(C.shown[i].text),
                                  C.shown[i].left, -1 };
  return n;
}

void ui_captions_frame(double dt) { cw_captions_update(dt); }

const char *cw_captions_speaker_label(const char *speaker) {
  if (!speaker || !*speaker) return "";
  /* the separator is the locale's ("{speaker} :" in French), not a fixed ": " */
  const char *pat = cw_tr_find("caption.speaker_label");
  const char *names[] = { "speaker" }, *values[] = { speaker };
  return cw_l10n_format(pat ? pat : "{speaker}:", names, values, 1);
}

/* ── CaptionPanel ───────────────────────────────────────────────────────
 * Draws the active lines bottom-up, each in a "panel" style box sized to
 * its text, centred; "speaker: text" with the speaker (as
 * cw_captions_speaker_label gives it) in speaker_color.
 * Theme type "CaptionPanel": panel, font, font_size, font_color,
 * speaker_color, separation. Mouse-transparent. Its minimum height fits
 * two lines so a layout reserves the space. */

static cw_vec2 cap_min(cw_node n, void *d) {
  (void)d;
  cw_ui_style st = T_STYLE(n, "panel");
  if (!st.draw) st = cw_ui_style_flat((cw_color){ 0, 0, 0, 0.78f }, 4, 10, 4);
  float lh = cw_ui_line_height(T_FONT(n), T_FSIZE(n));
  return (cw_vec2){ 0, ceilf((lh + SM_H(&st)) * 2 + T_CONST(n, "separation")) };
}

static void cap_draw(cw_node n, void *d) {
  (void)d;
  if (C.disabled) return;
  cw_caption_line lines[MAX_SHOWN + 8];
  int k = cw_captions_active(lines, (int)CW_ARRAY_LEN(lines));
  if (!k) return;
  cw_control *c = CTL(n);
  cw_ui_style st = T_STYLE(n, "panel");
  cw_font f = T_FONT(n);
  float fs = T_FSIZE(n), sep = T_CONST(n, "separation");
  float lh = cw_ui_line_height(f, fs);
  cw_color fc = T_COLOR(n, "font_color"), sc = T_COLOR(n, "speaker_color");
  /* captions read on any background: white on a dark box unless themed */
  if (!st.draw) {
    st = cw_ui_style_flat((cw_color){ 0, 0, 0, 0.78f }, 4, 10, 4);
    fc = CW_WHITE;
  }
  if (sc.a <= 0) sc = (cw_color){ 1.0f, 0.84f, 0.35f, 1 };
  float y = c->rect.h;
  for (int i = k - 1; i >= 0; i--) {
    const char *spk = lines[i].speaker;
    const char *head = cw_captions_speaker_label(spk);
    float hw = *head ? cw_ui_text_width(f, fs, head) : 0, gap = *head ? cw_font_advance(f, ' ', fs) : 0;
    float tw = cw_ui_text_width(f, fs, lines[i].text);
    float maxw = c->rect.w - SM_W(&st);
    if (hw + gap + tw > maxw) tw = maxw - hw - gap;
    float w = hw + gap + tw + SM_W(&st), h = lh + SM_H(&st);
    y -= h;
    if (y < 0) break;
    cw_rect box = { ui_round((c->rect.w - w) * 0.5f), y, w, h };
    cw_ui_draw_style(&st, box);
    /* "speaker: text"; right to left the speaker sits at the right end */
    bool rtl = ui_draw_rtl();
    float x = box.x + st.margin[0];
    if (*head) cw_ui_draw_text(f, fs, head, (cw_vec2){ rtl ? x + tw + gap : x, box.y + st.margin[1] }, sc);
    cw_ui_draw_text_in(f, fs, lines[i].text, (cw_rect){ rtl ? x : x + hw + gap, box.y + st.margin[1], tw, lh }, CW_HALIGN_LEFT, fc, true);
    y -= sep;
  }
}

static void cap_init(cw_node n, void *d) {
  (void)d;
  CTL(n)->mouse_filter = CW_MOUSE_IGNORE;
  CTL(n)->focus_mode = CW_FOCUS_NONE;
}

static cw_node_type caption_type = { .name = "CaptionPanel", .parent = "Control", .init = cap_init,
                                     .doc = "Shows the active captions and subtitles (cw_captions_*), newest at the bottom." };

void ui_register_captions(void) {
  cw_node_type_register(&caption_type);
  cw_ui_class_register("CaptionPanel", &(cw_ui_class){ .min_size = cap_min, .draw = cap_draw });
}
