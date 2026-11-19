// Draws one frame through Mesa EGL (surfaceless) + desktop GL and writes it as a PPM.
// Proves Zink on Turnip works on the device from the glibc world.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#define glGetString p_glGetString
static const GLubyte*(*p_glGetString)(GLenum);
#define glViewport p_glViewport
static void(*p_glViewport)(GLint,GLint,GLsizei,GLsizei);
#define glClearColor p_glClearColor
static void(*p_glClearColor)(GLfloat,GLfloat,GLfloat,GLfloat);
#define glClear p_glClear
static void(*p_glClear)(GLbitfield);
#define glBegin p_glBegin
static void(*p_glBegin)(GLenum);
#define glEnd p_glEnd
static void(*p_glEnd)(void);
#define glColor3f p_glColor3f
static void(*p_glColor3f)(GLfloat,GLfloat,GLfloat);
#define glVertex2f p_glVertex2f
static void(*p_glVertex2f)(GLfloat,GLfloat);
#define glPixelStorei p_glPixelStorei
static void(*p_glPixelStorei)(GLenum,GLint);
#define glReadPixels p_glReadPixels
static void(*p_glReadPixels)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*);
#define glGetError p_glGetError
static GLenum(*p_glGetError)(void);

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "frame.ppm";
    PFNEGLGETPLATFORMDISPLAYEXTPROC getdpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay d = getdpy(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint maj, min;
    if (!eglInitialize(d, &maj, &min)) { printf("eglInitialize failed %x\n", eglGetError()); return 1; }
    printf("EGL %d.%d %s\n", maj, min, eglQueryString(d, EGL_VENDOR));
    eglBindAPI(EGL_OPENGL_API);
    EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg; EGLint n;
    if (!eglChooseConfig(d, ca, &cfg, 1, &n) || n < 1) { printf("no config %x\n", eglGetError()); return 1; }
    EGLint sa[] = {EGL_WIDTH, 1280, EGL_HEIGHT, 720, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, cfg, sa);
    EGLint xa[] = {EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT, EGL_NONE};
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
    if (!c || !eglMakeCurrent(d, s, s, c)) { printf("context failed %x\n", eglGetError()); return 1; }
    *(void**)&p_glGetString = (void*)eglGetProcAddress("glGetString");
    *(void**)&p_glViewport = (void*)eglGetProcAddress("glViewport");
    *(void**)&p_glClearColor = (void*)eglGetProcAddress("glClearColor");
    *(void**)&p_glClear = (void*)eglGetProcAddress("glClear");
    *(void**)&p_glBegin = (void*)eglGetProcAddress("glBegin");
    *(void**)&p_glEnd = (void*)eglGetProcAddress("glEnd");
    *(void**)&p_glColor3f = (void*)eglGetProcAddress("glColor3f");
    *(void**)&p_glVertex2f = (void*)eglGetProcAddress("glVertex2f");
    *(void**)&p_glPixelStorei = (void*)eglGetProcAddress("glPixelStorei");
    *(void**)&p_glReadPixels = (void*)eglGetProcAddress("glReadPixels");
    *(void**)&p_glGetError = (void*)eglGetProcAddress("glGetError");
    printf("GL_RENDERER %s\nGL_VERSION %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glViewport(0, 0, 1280, 720);
    glClearColor(0.1f, 0.1f, 0.3f, 1); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0); glVertex2f(-0.6f, -0.6f);
    glColor3f(0, 1, 0); glVertex2f(0.6f, -0.6f);
    glColor3f(0, 0, 1); glVertex2f(0, 0.6f);
    glEnd();
    unsigned char *px = malloc(1280 * 720 * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, 1280, 720, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(out, "wb");
    fprintf(f, "P6 1280 720 255\n");
    for (int y = 719; y >= 0; y--) fwrite(px + y * 1280 * 3, 1, 1280 * 3, f);
    fclose(f);
    printf("wrote %s, glGetError %x\n", out, glGetError());
    return 0;
}
