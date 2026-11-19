// A stand-in for libGL's GLX: contexts are Mesa EGL contexts (surfaceless platform, Zink on
// Turnip), the game's window is a pbuffer, and glXSwapBuffers reads the frame into shared
// memory for the app. All gl* functions come from Mesa through glXGetProcAddress.
#define _GNU_SOURCE
#include "shim.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/glx.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { int id; } fbconfig;
typedef struct ctx { EGLContext egl; } ctx;
typedef struct { GLXDrawable id; EGLSurface surf; int w, h; } drawable;

static EGLDisplay edpy = EGL_NO_DISPLAY;
static EGLConfig ecfg;
static pthread_once_t egl_once = PTHREAD_ONCE_INIT;
static fbconfig the_fbconfig = {1};
static drawable draws[4];
static __thread ctx *cur_ctx;
static __thread GLXDrawable cur_draw;
static __thread Display *cur_dpy;
static int swap_interval = 1;

static void (*p_glGetIntegerv)(GLenum, GLint *);
static void (*p_glBindFramebuffer)(GLenum, GLuint);
static void (*p_glBindBuffer)(GLenum, GLuint);
static void (*p_glPixelStorei)(GLenum, GLint);
static void (*p_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void (*p_glReadBuffer)(GLenum);
static void (*p_glFinish)(void);
static const GLubyte *(*p_glGetString)(GLenum);

static void egl_init(void) {
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 0);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);   // the no-DRM path; patched Zink still picks the GPU
    PFNEGLGETPLATFORMDISPLAYEXTPROC getdpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    edpy = getdpy(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint maj, min;
    if (!eglInitialize(edpy, &maj, &min)) {
        SHIM_LOG("eglInitialize failed: 0x%x", eglGetError());
        edpy = EGL_NO_DISPLAY;
        return;
    }
    eglBindAPI(EGL_OPENGL_API);
    EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                      EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(edpy, attrs, &ecfg, 1, &n) || n < 1) SHIM_LOG("no EGL config");
    p_glGetIntegerv = (void *)eglGetProcAddress("glGetIntegerv");
    p_glBindFramebuffer = (void *)eglGetProcAddress("glBindFramebuffer");
    p_glBindBuffer = (void *)eglGetProcAddress("glBindBuffer");
    p_glPixelStorei = (void *)eglGetProcAddress("glPixelStorei");
    p_glReadPixels = (void *)eglGetProcAddress("glReadPixels");
    p_glReadBuffer = (void *)eglGetProcAddress("glReadBuffer");
    p_glFinish = (void *)eglGetProcAddress("glFinish");
    p_glGetString = (void *)eglGetProcAddress("glGetString");
    SHIM_LOG("EGL %d.%d ready", maj, min);
}
static int egl_ready(void) { pthread_once(&egl_once, egl_init); return edpy != EGL_NO_DISPLAY; }

static drawable *find_draw(GLXDrawable id, int create) {
    for (int i = 0; i < 4; i++) if (draws[i].id == id) return &draws[i];
    if (!create) return NULL;
    for (int i = 0; i < 4; i++) if (!draws[i].id) { draws[i].id = id; return &draws[i]; }
    return NULL;
}

// Makes sure the drawable's pbuffer matches its window's size.
static int sync_surface(drawable *dr) {
    int w = 1280, h = 720;
    x11_window_size(dr->id, &w, &h);
    if (dr->surf != EGL_NO_SURFACE && dr->w == w && dr->h == h) return 0;
    EGLint sa[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(edpy, ecfg, sa);
    if (s == EGL_NO_SURFACE) { SHIM_LOG("pbuffer %dx%d failed: 0x%x", w, h, eglGetError()); return -1; }
    EGLSurface old = dr->surf;
    dr->surf = s;
    dr->w = w;
    dr->h = h;
    if (cur_ctx && cur_draw == dr->id) eglMakeCurrent(edpy, s, s, cur_ctx->egl);
    if (old != EGL_NO_SURFACE) eglDestroySurface(edpy, old);
    SHIM_LOG("surface %dx%d", w, h);
    return 1;
}

// ---- Configs and visuals ---------------------------------------------------------------

Bool glXQueryExtension(Display *d, int *err, int *ev) { (void)d; if (err) *err = 0; if (ev) *ev = 0; return egl_ready(); }
Bool glXQueryVersion(Display *d, int *maj, int *min) { (void)d; *maj = 1; *min = 4; return True; }
const char *glXQueryExtensionsString(Display *d, int screen) {
    (void)d; (void)screen;
    return "GLX_ARB_create_context GLX_ARB_create_context_profile GLX_ARB_get_proc_address "
           "GLX_EXT_swap_control GLX_MESA_swap_control GLX_SGI_swap_control";
}
const char *glXGetClientString(Display *d, int name) { (void)d; return name == GLX_VENDOR ? "ftlhost" : "1.4"; }
const char *glXQueryServerString(Display *d, int screen, int name) { (void)screen; return glXGetClientString(d, name); }

GLXFBConfig *glXChooseFBConfig(Display *d, int screen, const int *attribs, int *n) {
    (void)d; (void)screen; (void)attribs;
    if (!egl_ready()) { *n = 0; return NULL; }
    GLXFBConfig *l = malloc(sizeof *l);
    l[0] = (GLXFBConfig)&the_fbconfig;
    *n = 1;
    return l;
}
GLXFBConfig *glXGetFBConfigs(Display *d, int screen, int *n) { return glXChooseFBConfig(d, screen, NULL, n); }

int glXGetFBConfigAttrib(Display *d, GLXFBConfig c, int attr, int *v) {
    (void)d; (void)c;
    switch (attr) {
    case GLX_RED_SIZE: case GLX_GREEN_SIZE: case GLX_BLUE_SIZE: case GLX_ALPHA_SIZE: *v = 8; break;
    case GLX_DEPTH_SIZE: *v = 24; break;
    case GLX_STENCIL_SIZE: *v = 8; break;
    case GLX_BUFFER_SIZE: *v = 32; break;
    case GLX_DOUBLEBUFFER: *v = True; break;
    case GLX_RENDER_TYPE: *v = GLX_RGBA_BIT; break;
    case GLX_DRAWABLE_TYPE: *v = GLX_WINDOW_BIT | GLX_PBUFFER_BIT; break;
    case GLX_X_RENDERABLE: *v = True; break;
    case GLX_X_VISUAL_TYPE: *v = GLX_TRUE_COLOR; break;
    case GLX_VISUAL_ID: *v = 0x21; break;
    case GLX_FBCONFIG_ID: *v = 1; break;
    case GLX_CONFIG_CAVEAT: *v = GLX_NONE; break;
    case GLX_SAMPLE_BUFFERS: case GLX_SAMPLES: *v = 0; break;
    default: *v = 0;
    }
    return Success;
}

XVisualInfo *glXGetVisualFromFBConfig(Display *d, GLXFBConfig c) {
    (void)c;
    XVisualInfo *vi = calloc(1, sizeof *vi);
    Screen *s = ScreenOfDisplay(d, 0);
    vi->visual = s->root_visual;
    vi->visualid = s->root_visual->visualid;
    vi->screen = 0;
    vi->depth = 24;
    vi->class = TrueColor;
    vi->red_mask = 0xff0000;
    vi->green_mask = 0xff00;
    vi->blue_mask = 0xff;
    vi->colormap_size = 256;
    vi->bits_per_rgb = 8;
    return vi;
}
XVisualInfo *glXChooseVisual(Display *d, int screen, int *attribs) { (void)screen; (void)attribs; egl_ready(); return glXGetVisualFromFBConfig(d, NULL); }

// ---- Contexts ----------------------------------------------------------------------------

static GLXContext make_ctx(GLXContext share, const int *glx_attribs) {
    if (!egl_ready()) return NULL;
    EGLint a[16];
    int n = 0, profile = EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT;
    for (const int *p = glx_attribs; p && *p; p += 2) {
        switch (p[0]) {
        case GLX_CONTEXT_MAJOR_VERSION_ARB: a[n++] = EGL_CONTEXT_MAJOR_VERSION; a[n++] = p[1]; break;
        case GLX_CONTEXT_MINOR_VERSION_ARB: a[n++] = EGL_CONTEXT_MINOR_VERSION; a[n++] = p[1]; break;
        case GLX_CONTEXT_FLAGS_ARB: a[n++] = EGL_CONTEXT_FLAGS_KHR; a[n++] = p[1]; break;
        case GLX_CONTEXT_PROFILE_MASK_ARB:
            profile = p[1] & GLX_CONTEXT_CORE_PROFILE_BIT_ARB ? EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT
                                                               : EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT;
            break;
        }
    }
    a[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK;
    a[n++] = profile;
    a[n] = EGL_NONE;
    EGLContext e = eglCreateContext(edpy, ecfg, share ? ((ctx *)share)->egl : EGL_NO_CONTEXT, a);
    if (e == EGL_NO_CONTEXT) { SHIM_LOG("eglCreateContext failed: 0x%x", eglGetError()); return NULL; }
    ctx *c = calloc(1, sizeof *c);
    c->egl = e;
    return (GLXContext)c;
}

GLXContext glXCreateNewContext(Display *d, GLXFBConfig c, int type, GLXContext share, Bool direct) {
    (void)d; (void)c; (void)type; (void)direct;
    return make_ctx(share, NULL);
}
GLXContext glXCreateContextAttribsARB(Display *d, GLXFBConfig c, GLXContext share, Bool direct, const int *attribs) {
    (void)d; (void)c; (void)direct;
    return make_ctx(share, attribs);
}
GLXContext glXCreateContext(Display *d, XVisualInfo *vi, GLXContext share, Bool direct) {
    (void)d; (void)vi; (void)direct;
    return make_ctx(share, NULL);
}
void glXDestroyContext(Display *d, GLXContext c) {
    (void)d;
    if (!c) return;
    if ((ctx *)c == cur_ctx) { eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); cur_ctx = NULL; }
    eglDestroyContext(edpy, ((ctx *)c)->egl);
    free(c);
}
Bool glXIsDirect(Display *d, GLXContext c) { (void)d; (void)c; return True; }

GLXWindow glXCreateWindow(Display *d, GLXFBConfig c, Window w, const int *attribs) {
    (void)d; (void)c; (void)attribs;
    find_draw(w, 1);
    return w;
}
void glXDestroyWindow(Display *d, GLXWindow w) {
    (void)d;
    drawable *dr = find_draw(w, 0);
    if (!dr) return;
    if (cur_draw == w) { eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, cur_ctx ? cur_ctx->egl : EGL_NO_CONTEXT); cur_draw = 0; }
    if (dr->surf != EGL_NO_SURFACE) eglDestroySurface(edpy, dr->surf);
    memset(dr, 0, sizeof *dr);
}

Bool glXMakeContextCurrent(Display *d, GLXDrawable draw, GLXDrawable read, GLXContext c) {
    (void)read;
    if (!egl_ready()) return False;
    if (!c) {
        eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        cur_ctx = NULL; cur_draw = 0; cur_dpy = NULL;
        return True;
    }
    EGLSurface s = EGL_NO_SURFACE;
    if (draw) {
        drawable *dr = find_draw(draw, 1);
        cur_draw = 0;   // so sync_surface doesn't make current with a stale context
        if (!dr || sync_surface(dr) < 0) return False;
        s = dr->surf;
    }
    if (!eglMakeCurrent(edpy, s, s, ((ctx *)c)->egl)) { SHIM_LOG("eglMakeCurrent failed: 0x%x", eglGetError()); return False; }
    static int said;
    if (!said++) SHIM_LOG("GL_RENDERER %s / GL_VERSION %s", p_glGetString(GL_RENDERER), p_glGetString(GL_VERSION));
    cur_ctx = (ctx *)c;
    cur_draw = draw;
    cur_dpy = d;
    return True;
}
Bool glXMakeCurrent(Display *d, GLXDrawable draw, GLXContext c) { return glXMakeContextCurrent(d, draw, draw, c); }
GLXContext glXGetCurrentContext(void) { return (GLXContext)cur_ctx; }
GLXDrawable glXGetCurrentDrawable(void) { return cur_draw; }
GLXDrawable glXGetCurrentReadDrawable(void) { return cur_draw; }
Display *glXGetCurrentDisplay(void) { return cur_dpy; }

// ---- Presenting ---------------------------------------------------------------------------

static struct timespec next_deadline;

static void pace(void) {
    int hz = host_refresh_hz();
    if (hz <= 0 || swap_interval <= 0) hz = 60;   // never spin: the phone would cook
    long period = 1000000000L / hz * (swap_interval > 0 ? swap_interval : 1);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (next_deadline.tv_sec == 0) next_deadline = now;
    next_deadline.tv_nsec += period;
    while (next_deadline.tv_nsec >= 1000000000L) { next_deadline.tv_nsec -= 1000000000L; next_deadline.tv_sec++; }
    // Fell behind by more than a frame: don't try to catch up.
    long behind = (now.tv_sec - next_deadline.tv_sec) * 1000000000L + (now.tv_nsec - next_deadline.tv_nsec);
    if (behind > period) { next_deadline = now; return; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_deadline, NULL);
}

void glXSwapBuffers(Display *d, GLXDrawable draw) {
    (void)d;
    drawable *dr = find_draw(draw, 0);
    if (!cur_ctx || !dr) return;
    int index;
    uint8_t *px = host_frame_begin(dr->w, dr->h, &index);
    if (px) {
        GLint read_fb = 0, pack_buf = 0, align = 4, row = 0, skip_r = 0, skip_p = 0, read_buf = GL_BACK;
        p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
        p_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buf);
        p_glGetIntegerv(GL_PACK_ALIGNMENT, &align);
        p_glGetIntegerv(GL_PACK_ROW_LENGTH, &row);
        p_glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_r);
        p_glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_p);
        if (read_fb) p_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        p_glGetIntegerv(GL_READ_BUFFER, &read_buf);
        p_glReadBuffer(GL_BACK);
        if (pack_buf) p_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        p_glPixelStorei(GL_PACK_ALIGNMENT, 4);
        p_glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        p_glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        p_glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        p_glReadPixels(0, 0, dr->w, dr->h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        p_glPixelStorei(GL_PACK_ALIGNMENT, align);
        p_glPixelStorei(GL_PACK_ROW_LENGTH, row);
        p_glPixelStorei(GL_PACK_SKIP_ROWS, skip_r);
        p_glPixelStorei(GL_PACK_SKIP_PIXELS, skip_p);
        if (pack_buf) p_glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buf);
        p_glReadBuffer(read_buf);
        if (read_fb) p_glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
        host_frame_publish(index, dr->w, dr->h);
    }
    pace();
    x11_frame_tick();
    sync_surface(dr);   // follow a window resize
}

void glXWaitGL(void) { if (cur_ctx) p_glFinish(); }
void glXWaitX(void) {}
void glXSwapIntervalEXT(Display *d, GLXDrawable draw, int interval) { (void)d; (void)draw; swap_interval = interval; }
int glXSwapIntervalMESA(unsigned interval) { swap_interval = interval; return 0; }
int glXSwapIntervalSGI(int interval) { swap_interval = interval; return 0; }

typedef void (*fnptr)(void);
static const struct { const char *name; fnptr fn; } glx_fns[] = {
#define F(n) {#n, (fnptr)n}
    F(glXQueryExtension), F(glXQueryVersion), F(glXQueryExtensionsString), F(glXGetClientString),
    F(glXQueryServerString), F(glXChooseFBConfig), F(glXGetFBConfigs), F(glXGetFBConfigAttrib),
    F(glXGetVisualFromFBConfig), F(glXChooseVisual), F(glXCreateNewContext), F(glXCreateContextAttribsARB),
    F(glXCreateContext), F(glXDestroyContext), F(glXIsDirect), F(glXCreateWindow), F(glXDestroyWindow),
    F(glXMakeContextCurrent), F(glXMakeCurrent), F(glXGetCurrentContext), F(glXGetCurrentDrawable),
    F(glXGetCurrentReadDrawable), F(glXGetCurrentDisplay), F(glXSwapBuffers), F(glXWaitGL), F(glXWaitX),
    F(glXSwapIntervalEXT), F(glXSwapIntervalMESA), F(glXSwapIntervalSGI),
#undef F
};

fnptr glXGetProcAddress(const GLubyte *name) {
    const char *n = (const char *)name;
    if (!n) return NULL;
    if (!strncmp(n, "glX", 3)) {
        for (size_t i = 0; i < sizeof glx_fns / sizeof *glx_fns; i++)
            if (!strcmp(glx_fns[i].name, n)) return glx_fns[i].fn;
        return NULL;
    }
    egl_ready();
    return (fnptr)eglGetProcAddress(n);
}
fnptr glXGetProcAddressARB(const GLubyte *name) { return glXGetProcAddress(name); }
