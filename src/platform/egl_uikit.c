/* iOS: GLES 3.0 through ANGLE's EGL on the window's CAMetalLayer.
 *
 * SDL2's UIKit driver has no EGL: its SDL_GL_CreateContext makes an Apple
 * EAGL context (deprecated since iOS 12, and not the libGLESv2 the engine
 * links), so every gl* call the engine made would reach ANGLE with no
 * current context. Here the window is a Metal window (SDL_WINDOW_METAL,
 * sdl_common.c), SDL_Metal_CreateView gives it a view whose layer is a
 * CAMetalLayer, and ANGLE's Metal backend renders into that layer through
 * EGL: a display of type EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, a window
 * surface on the layer, an ES 3.0 context, eglSwapBuffers at the end of each
 * frame. OpenGLES.framework is never linked. The drawable's size follows
 * the view (SDL sets the layer's drawableSize from its bounds and scale on
 * every layout), which ANGLE picks up at the next swap.
 *
 * Compiled on iOS only (CMakeLists.txt, CW_EGL_UIKIT); sdl_common.c's
 * cw__gl_* shim routes here instead of SDL_GL_*. The same code would drive
 * ANGLE on macOS; there SDL's own EGL path over ANGLE's dylibs works, so it
 * is left alone. */
#include <stdio.h>
#include <string.h>

#include <SDL.h>
#include <SDL_metal.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>

#include "internal.h"
#include "cartwheel/core.h"

static struct {
  EGLDisplay dpy;
  EGLSurface surf;
  EGLContext ctx;
  SDL_MetalView view;
} E;

static const char *egl_err(void) {
  static char buf[32];
  snprintf(buf, sizeof buf, "EGL error 0x%04x", (unsigned)eglGetError());
  return buf;
}

/* a config, surface and context for the layer with `samples` multisampling
 * (0 = none); false (nothing left allocated but the display) when ANGLE has
 * no such config or the surface or context cannot be made */
static bool make(void *layer, int samples, int depth, int stencil) {
  EGLint attrs[32];
  int n = 0;
  attrs[n++] = EGL_RENDERABLE_TYPE;
  attrs[n++] = EGL_OPENGL_ES3_BIT;
  attrs[n++] = EGL_SURFACE_TYPE;
  attrs[n++] = EGL_WINDOW_BIT;
  attrs[n++] = EGL_RED_SIZE;
  attrs[n++] = 8;
  attrs[n++] = EGL_GREEN_SIZE;
  attrs[n++] = 8;
  attrs[n++] = EGL_BLUE_SIZE;
  attrs[n++] = 8;
  attrs[n++] = EGL_ALPHA_SIZE;
  attrs[n++] = 8;
  attrs[n++] = EGL_DEPTH_SIZE;
  attrs[n++] = depth;
  attrs[n++] = EGL_STENCIL_SIZE;
  attrs[n++] = stencil;
  if (samples > 0) {
    attrs[n++] = EGL_SAMPLE_BUFFERS;
    attrs[n++] = 1;
    attrs[n++] = EGL_SAMPLES;
    attrs[n++] = samples;
  }
  attrs[n++] = EGL_NONE;
  EGLConfig cfg = NULL;
  EGLint count = 0;
  if (!eglChooseConfig(E.dpy, attrs, &cfg, 1, &count) || count < 1) {
    cw_warn("iOS GL: no EGL config with %d samples, depth %d, stencil %d (%s)", samples, depth, stencil, egl_err());
    return false;
  }
  E.surf = eglCreateWindowSurface(E.dpy, cfg, (EGLNativeWindowType)layer, NULL);
  if (E.surf == EGL_NO_SURFACE) {
    cw_warn("iOS GL: eglCreateWindowSurface on the CAMetalLayer: %s", egl_err());
    return false;
  }
  static const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
  E.ctx = eglCreateContext(E.dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
  if (E.ctx == EGL_NO_CONTEXT) {
    cw_warn("iOS GL: eglCreateContext (ES 3.0): %s", egl_err());
    eglDestroySurface(E.dpy, E.surf);
    E.surf = EGL_NO_SURFACE;
    return false;
  }
  if (!eglMakeCurrent(E.dpy, E.surf, E.surf, E.ctx)) {
    cw_warn("iOS GL: eglMakeCurrent: %s", egl_err());
    eglDestroyContext(E.dpy, E.ctx);
    eglDestroySurface(E.dpy, E.surf);
    E.ctx = EGL_NO_CONTEXT;
    E.surf = EGL_NO_SURFACE;
    return false;
  }
  return true;
}

/* The display, a window surface on win's Metal layer and an ES 3.0 context,
 * made current. msaa samples when ANGLE offers them, else none (a warning
 * says so). *ctx receives the EGL context (sdl_common.c keeps it where it
 * keeps SDL's). */
bool cw__egl_uikit_create(SDL_Window *win, int msaa, int depth, int stencil, void **ctx);
bool cw__egl_uikit_create(SDL_Window *win, int msaa, int depth, int stencil, void **ctx) {
  E.view = SDL_Metal_CreateView(win);
  if (!E.view) {
    cw_error("iOS GL: SDL_Metal_CreateView: %s", SDL_GetError());
    return false;
  }
  void *layer = SDL_Metal_GetLayer(E.view);
  if (!layer) {
    cw_error("iOS GL: the Metal view has no layer");
    return false;
  }
  static const EGLAttrib dpy_attrs[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE };
  E.dpy = eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, (void *)0, dpy_attrs);
  if (E.dpy == EGL_NO_DISPLAY) {
    cw_error("iOS GL: eglGetPlatformDisplay (ANGLE on Metal): %s", egl_err());
    return false;
  }
  EGLint major = 0, minor = 0;
  if (!eglInitialize(E.dpy, &major, &minor)) {
    cw_error("iOS GL: eglInitialize: %s", egl_err());
    return false;
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  if (!make(layer, msaa, depth, stencil) && (msaa <= 0 || !make(layer, 0, depth, stencil))) {
    cw_error("iOS GL: no ES 3.0 context on the window (EGL %d.%d, %s)", (int)major, (int)minor, eglQueryString(E.dpy, EGL_VENDOR));
    return false;
  }
  cw_log(CW_LOG_INFO, "iOS GL: EGL %d.%d %s on the window's Metal layer", (int)major, (int)minor, eglQueryString(E.dpy, EGL_VERSION));
  *ctx = E.ctx;
  return true;
}

void cw__egl_uikit_swap(void);
void cw__egl_uikit_swap(void) {
  if (E.surf != EGL_NO_SURFACE) eglSwapBuffers(E.dpy, E.surf);
}

void cw__egl_uikit_swap_interval(int interval);
void cw__egl_uikit_swap_interval(int interval) {
  if (E.dpy != EGL_NO_DISPLAY) eglSwapInterval(E.dpy, interval);
}

/* the layer's drawable size: the window's points times the screen's scale */
void cw__egl_uikit_drawable_size(SDL_Window *win, int *w, int *h);
void cw__egl_uikit_drawable_size(SDL_Window *win, int *w, int *h) {
  SDL_Metal_GetDrawableSize(win, w, h);
}

void cw__egl_uikit_destroy(void);
void cw__egl_uikit_destroy(void) {
  if (E.dpy != EGL_NO_DISPLAY) {
    eglMakeCurrent(E.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (E.ctx != EGL_NO_CONTEXT) eglDestroyContext(E.dpy, E.ctx);
    if (E.surf != EGL_NO_SURFACE) eglDestroySurface(E.dpy, E.surf);
    eglTerminate(E.dpy);
  }
  if (E.view) SDL_Metal_DestroyView(E.view);
  memset(&E, 0, sizeof E);
}
