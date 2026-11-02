// SPDX-License-Identifier: GPL-3.0-or-later
// sde-gl-probe: which OpenGL renderer does this X display give its programs?
// sde-session runs it once at login (0.2.0 Alpha 3.1) to choose the
// compositor's automatic profile: with OpenGL rendered in software
// (Mesa's llvmpipe, softpipe or swrast: a GPU whose OpenGL driver is
// missing, a virtual machine) the compositor's OpenGL effects run on the
// CPU, so the lite profile (XRender) is chosen instead.
//
// One line on standard output:
//   glx=1 accelerated=1 renderer=Mesa Intel(R) UHD Graphics 620 (KBL GT2)
//   glx=1 accelerated=0 renderer=llvmpipe (LLVM 20.1.2, 256 bits)
//   glx=0 reason=no GLX extension
// libGL is opened at run time (no build dependency); X errors are ignored
// and an alarm ends the helper if a driver hangs, so a broken OpenGL stack
// costs the session at most a few seconds and never the session itself.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
typedef struct __GLXcontextRec *GLXContext;
typedef XID GLXDrawable;
constexpr int kGlxRgba = 4, kGlxDoubleBuffer = 5;
constexpr int kRendererDeviceId = 0x8184, kRendererAccelerated = 0x8186;
constexpr unsigned kGlRenderer = 0x1F01;

struct Glx {
  Bool (*query_extension)(Display *, int *, int *);
  const char *(*query_extensions_string)(Display *, int);
  void *(*get_proc_address)(const unsigned char *);
  XVisualInfo *(*choose_visual)(Display *, int, int *);
  GLXContext (*create_context)(Display *, XVisualInfo *, GLXContext, Bool);
  Bool (*make_current)(Display *, GLXDrawable, GLXContext);
  void (*destroy_context)(Display *, GLXContext);
  const unsigned char *(*get_string)(unsigned);
};

int ignore_errors(Display *, XErrorEvent *) { return 0; }

bool software_name(const std::string &r) {
  for (const char *n : {"llvmpipe", "softpipe", "swrast", "Software Rasterizer", "SWR"})
    if (r.find(n) != std::string::npos) return true;
  return false;
}

template <typename F>
bool load(void *lib, const char *name, F &out) {
  out = reinterpret_cast<F>(dlsym(lib, name));
  return out != nullptr;
}
}  // namespace

int main() {
  alarm(5);  // a hanging driver must not hold the session
  Display *dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    printf("error=no display\n");
    return 2;
  }
  XSetErrorHandler(ignore_errors);
  int screen = DefaultScreen(dpy);
  void *lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    printf("glx=0 reason=no libGL\n");
    return 0;
  }
  Glx g{};
  if (!load(lib, "glXQueryExtension", g.query_extension) ||
      !load(lib, "glXQueryExtensionsString", g.query_extensions_string)) {
    printf("glx=0 reason=libGL without GLX\n");
    return 0;
  }
  int error_base = 0, event_base = 0;
  if (!g.query_extension(dpy, &error_base, &event_base)) {
    printf("glx=0 reason=no GLX extension\n");
    return 0;
  }
  load(lib, "glXGetProcAddressARB", g.get_proc_address);
  const char *exts = g.query_extensions_string(dpy, screen);
  // Mesa answers without a context (GLX_MESA_query_renderer).
  // SDE_GL_PROBE_CONTEXT=1 (tests) takes the context path below instead.
  const char *force_context = getenv("SDE_GL_PROBE_CONTEXT");
  bool query = !(force_context && *force_context && strcmp(force_context, "0") != 0);
  if (query && exts && strstr(exts, "GLX_MESA_query_renderer") && g.get_proc_address) {
    auto integer = reinterpret_cast<Bool (*)(Display *, int, int, int, unsigned *)>(
        g.get_proc_address(reinterpret_cast<const unsigned char *>("glXQueryRendererIntegerMESA")));
    auto string = reinterpret_cast<const char *(*)(Display *, int, int, int)>(
        g.get_proc_address(reinterpret_cast<const unsigned char *>("glXQueryRendererStringMESA")));
    unsigned accelerated = 1;
    if (integer && integer(dpy, screen, 0, kRendererAccelerated, &accelerated)) {
      const char *device = string ? string(dpy, screen, 0, kRendererDeviceId) : nullptr;
      std::string name = device ? device : "";
      printf("glx=1 accelerated=%d renderer=%s\n", accelerated && !software_name(name) ? 1 : 0, name.c_str());
      return 0;
    }
  }
  // Other drivers: a context on a small unmapped window, then GL_RENDERER.
  if (!load(lib, "glXChooseVisual", g.choose_visual) || !load(lib, "glXCreateContext", g.create_context) ||
      !load(lib, "glXMakeCurrent", g.make_current) || !load(lib, "glXDestroyContext", g.destroy_context) ||
      !load(lib, "glGetString", g.get_string)) {
    printf("glx=0 reason=incomplete libGL\n");
    return 0;
  }
  int attrs[] = {kGlxRgba, kGlxDoubleBuffer, None};
  XVisualInfo *vi = g.choose_visual(dpy, screen, attrs);
  if (!vi) {
    printf("glx=0 reason=no OpenGL visual\n");
    return 0;
  }
  XSetWindowAttributes a{};
  a.colormap = XCreateColormap(dpy, RootWindow(dpy, screen), vi->visual, AllocNone);
  Window w = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, 1, 1, 0, vi->depth, InputOutput, vi->visual,
                           CWColormap, &a);
  GLXContext ctx = g.create_context(dpy, vi, nullptr, True);
  std::string name;
  if (ctx && g.make_current(dpy, w, ctx)) {
    const unsigned char *r = g.get_string(kGlRenderer);
    if (r) name = reinterpret_cast<const char *>(r);
    g.make_current(dpy, None, nullptr);
  }
  if (ctx) g.destroy_context(dpy, ctx);
  XDestroyWindow(dpy, w);
  XFree(vi);
  if (name.empty()) {
    printf("glx=0 reason=no OpenGL context\n");
    return 0;
  }
  printf("glx=1 accelerated=%d renderer=%s\n", software_name(name) ? 0 : 1, name.c_str());
  return 0;
}
