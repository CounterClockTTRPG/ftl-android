// A stand-in for libX11 + libXi: one display, one screen, the game's window, and input from
// the app turned into X events. Only what FTL calls, with real Xlib structure layouts
// (box64 passes some Display fields through, and the game reads Display/Screen directly).
#define _GNU_SOURCE
#include "shim.h"
#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XInput2.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#define XI_OPCODE 131
#define XI_FIRST_EVENT 66
#define XI_FIRST_ERROR 129
#define ROOT_WINDOW 1
#define DEFAULT_CMAP 2
#define VISUAL_ID 0x21
#define DEV_MASTER_POINTER 2
#define DEV_MASTER_KEYBOARD 3
#define DEV_TOUCH 10
#define DEV_MOUSE 11

static Display *the_dpy;
static int trace_events = -1;
static Screen the_screen;
static Depth the_depth;
static Visual the_visual;
static pthread_mutex_t dpy_lock;    // XLockDisplay (recursive)
static int wake_pipe[2] = {-1, -1};

// The window
static Window main_win;
static int win_w, win_h, win_mapped;
static long win_mask;
static XID next_id = 0x200000;

// Atoms
#define MAX_ATOMS 256
static char *atom_names[MAX_ATOMS];
static int n_atoms;
static Atom atom_wm_protocols, atom_wm_delete;

// Pointer state
static double ptr_x, ptr_y;
static unsigned ptr_buttons;

// Keys: keycodes are handed out as keysyms arrive
static KeySym key_syms[256];
static char key_text[256][8];

// Event queue
typedef struct { XEvent ev; void *data; } qentry;
#define QSIZE 1024
static qentry queue[QSIZE];
static int q_head, q_count;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcond = PTHREAD_COND_INITIALIZER;
static unsigned long serial;
static unsigned int cookie_next = 1;

static Time now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (Time)(uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void wake_set(void) {
    char c = 1;
    if (wake_pipe[1] >= 0 && write(wake_pipe[1], &c, 1) < 0) {}
}
static void wake_drain(void) {
    char buf[64];
    while (wake_pipe[0] >= 0 && read(wake_pipe[0], buf, sizeof buf) > 0) {}
}

static void push(XEvent *ev, void *data) {
    pthread_mutex_lock(&qlock);
    if (q_count == QSIZE) {   // drop the oldest
        free(queue[q_head].data);
        q_head = (q_head + 1) % QSIZE;
        q_count--;
    }
    ev->xany.serial = ++serial;
    ev->xany.display = the_dpy;
    qentry *e = &queue[(q_head + q_count) % QSIZE];
    e->ev = *ev;
    e->data = data;
    q_count++;
    if (the_dpy) the_dpy->qlen = q_count;
    pthread_cond_broadcast(&qcond);
    pthread_mutex_unlock(&qlock);
    wake_set();
}

// Removes entry i (relative to head); call with qlock held.
static qentry take(int i) {
    int at = (q_head + i) % QSIZE;
    qentry e = queue[at];
    for (int k = i; k > 0; k--) queue[(q_head + k) % QSIZE] = queue[(q_head + k - 1) % QSIZE];
    q_head = (q_head + 1) % QSIZE;
    q_count--;
    if (the_dpy) the_dpy->qlen = q_count;
    if (!q_count) wake_drain();
    return e;
}

// Cookie data waiting for XGetEventData
typedef struct cookie_slot { unsigned int cookie; void *data; struct cookie_slot *next; } cookie_slot;
static cookie_slot *cookies;

static void deliver(qentry e, XEvent *out) {
    *out = e.ev;
    if (trace_events < 0) { const char *t = getenv("FTLHOST_TRACE"); trace_events = t && *t; }
    if (trace_events) {
        if (e.ev.type == GenericEvent) {
            XIDeviceEvent *d = e.data;
            SHIM_LOG("event XI %d detail %d at %.0f,%.0f time %lu", d->evtype, d->detail, d->event_x, d->event_y, d->time);
        } else SHIM_LOG("event %d", e.ev.type);
    }
    if (e.ev.type == GenericEvent) {
        cookie_slot *s = malloc(sizeof *s);
        s->cookie = out->xcookie.cookie = cookie_next++;
        s->data = e.data;
        s->next = cookies;
        cookies = s;
        out->xcookie.data = NULL;
    }
}

// ---- Input from the app ----------------------------------------------------------------

static int touches_selected, buttons_selected;
static unsigned touch_seq = 1;
static struct { unsigned app_id, detail; int active; } touch_map[16];

static XIDeviceEvent *xi_event(int evtype, int source, int detail, double x, double y) {
    // One block: the event, then button mask, valuator mask, values.
    size_t size = sizeof(XIDeviceEvent) + 4 + 4 + 2 * sizeof(double);
    XIDeviceEvent *e = calloc(1, size);
    unsigned char *extra = (unsigned char *)(e + 1);
    e->type = GenericEvent;
    e->display = the_dpy;
    e->extension = XI_OPCODE;
    e->evtype = evtype;
    e->time = now_ms();
    e->deviceid = DEV_MASTER_POINTER;
    e->sourceid = source;
    e->detail = detail;
    e->root = ROOT_WINDOW;
    e->event = main_win;
    e->child = None;
    e->root_x = e->event_x = x;
    e->root_y = e->event_y = y;
    e->buttons.mask_len = 4;
    e->buttons.mask = extra;
    for (int b = 1; b < 8; b++) if (ptr_buttons & (1u << b)) XISetMask(e->buttons.mask, b);
    e->valuators.mask_len = 4;
    e->valuators.mask = extra + 4;
    XISetMask(e->valuators.mask, 0);
    XISetMask(e->valuators.mask, 1);
    e->valuators.values = (double *)(extra + 8);
    e->valuators.values[0] = x;
    e->valuators.values[1] = y;
    return e;
}

// Pointer events are spaced by rendered frames: FTL reads the mouse once per frame, so a press
// and release (or a move and a press) that arrive between two frames are lost. A button event
// goes out only when two frames have passed since the previous pointer event went out; anything
// behind it waits its turn, in order.
#define PTR_GAP_FRAMES 2
#define PTR_DEFER 256
static qentry deferred[PTR_DEFER];
static int def_head, def_count;
static int frame_no, last_ptr_frame = -100;
static pthread_mutex_t ptr_lock = PTHREAD_MUTEX_INITIALIZER;

static int is_button(const XEvent *ev, const void *data) {
    if (ev->type == ButtonPress || ev->type == ButtonRelease) return 1;
    if (ev->type == GenericEvent && data) {
        int t = ((const XIDeviceEvent *)data)->evtype;
        return t == XI_ButtonPress || t == XI_ButtonRelease || t == XI_TouchBegin || t == XI_TouchEnd;
    }
    return 0;
}

// Sends queued pointer events that are due; call with ptr_lock held.
static void ptr_flush(void) {
    while (def_count) {
        qentry *e = &deferred[def_head];
        if (is_button(&e->ev, e->data) && frame_no - last_ptr_frame < PTR_GAP_FRAMES) break;
        push(&e->ev, e->data);
        last_ptr_frame = frame_no;
        def_head = (def_head + 1) % PTR_DEFER;
        def_count--;
    }
}

static void push_pointer(XEvent *ev, void *data) {
    pthread_mutex_lock(&ptr_lock);
    if (def_count == PTR_DEFER) {   // the game stopped drawing; keep the newest
        free(deferred[def_head].data);
        def_head = (def_head + 1) % PTR_DEFER;
        def_count--;
    }
    qentry *e = &deferred[(def_head + def_count) % PTR_DEFER];
    e->ev = *ev;
    e->data = data;
    def_count++;
    ptr_flush();
    pthread_mutex_unlock(&ptr_lock);
}

void x11_frame_tick(void) {
    pthread_mutex_lock(&ptr_lock);
    frame_no++;
    ptr_flush();
    pthread_mutex_unlock(&ptr_lock);
}

static void push_xi(XIDeviceEvent *d) {
    XEvent ev = {0};
    ev.xcookie.type = GenericEvent;
    ev.xcookie.extension = XI_OPCODE;
    ev.xcookie.evtype = d->evtype;
    push_pointer(&ev, d);
}

void x11_on_touch(const msg_touch *t) {
    if (!main_win) return;
    int slot = -1, free_slot = -1, active = 0;
    for (int i = 0; i < 16; i++) {
        if (touch_map[i].active) { active++; if (touch_map[i].app_id == t->id) slot = i; }
        else if (free_slot < 0) free_slot = i;
    }
    int evtype;
    if (t->action == TOUCH_DOWN) {
        if (slot < 0) { slot = free_slot; if (slot < 0) return; }
        touch_map[slot].app_id = t->id;
        touch_map[slot].detail = touch_seq++;
        touch_map[slot].active = 1;
        evtype = XI_TouchBegin;
    } else {
        if (slot < 0) return;
        evtype = t->action == TOUCH_MOVE ? XI_TouchUpdate : XI_TouchEnd;
    }
    XIDeviceEvent *d = xi_event(evtype, DEV_TOUCH, touch_map[slot].detail, t->x, t->y);
    if (evtype == XI_TouchBegin && active == 0) d->flags = XITouchEmulatingPointer;
    ptr_x = t->x;
    ptr_y = t->y;
    if (evtype == XI_TouchEnd) touch_map[slot].active = 0;
    if (touches_selected) push_xi(d); else free(d);
}

void x11_on_mouse(const msg_mouse *m) {
    if (!main_win) return;
    ptr_x = m->x;
    ptr_y = m->y;
    int evtype = m->action == MOUSE_MOVE ? XI_Motion : m->action == MOUSE_DOWN ? XI_ButtonPress : XI_ButtonRelease;
    XIDeviceEvent *d = xi_event(evtype, DEV_MOUSE, m->action == MOUSE_MOVE ? 0 : m->button, m->x, m->y);
    if (m->action == MOUSE_DOWN) ptr_buttons |= 1u << m->button;
    if (m->action == MOUSE_UP) ptr_buttons &= ~(1u << m->button);
    if (buttons_selected) { push_xi(d); return; }
    free(d);
    // No XI2: core events
    XEvent ev = {0};
    if (m->action == MOUSE_MOVE) {
        ev.xmotion.type = MotionNotify;
        ev.xmotion.window = main_win; ev.xmotion.root = ROOT_WINDOW;
        ev.xmotion.x = ev.xmotion.x_root = (int)m->x;
        ev.xmotion.y = ev.xmotion.y_root = (int)m->y;
        ev.xmotion.time = now_ms(); ev.xmotion.same_screen = True;
    } else {
        ev.xbutton.type = m->action == MOUSE_DOWN ? ButtonPress : ButtonRelease;
        ev.xbutton.window = main_win; ev.xbutton.root = ROOT_WINDOW;
        ev.xbutton.x = ev.xbutton.x_root = (int)m->x;
        ev.xbutton.y = ev.xbutton.y_root = (int)m->y;
        ev.xbutton.button = m->button; ev.xbutton.time = now_ms(); ev.xbutton.same_screen = True;
    }
    push_pointer(&ev, NULL);
}

static int keycode_for(KeySym sym) {
    for (int k = 8; k < 256; k++) if (key_syms[k] == sym) return k;
    for (int k = 8; k < 256; k++) if (!key_syms[k]) { key_syms[k] = sym; return k; }
    return 8;
}

void x11_on_key(const msg_key *k) {
    if (!main_win) return;
    int kc = keycode_for(k->keysym);
    memcpy(key_text[kc], k->text, sizeof key_text[kc]);
    key_text[kc][7] = 0;
    XEvent ev = {0};
    ev.xkey.type = k->down ? KeyPress : KeyRelease;
    ev.xkey.window = main_win;
    ev.xkey.root = ROOT_WINDOW;
    ev.xkey.time = now_ms();
    ev.xkey.x = (int)ptr_x;
    ev.xkey.y = (int)ptr_y;
    ev.xkey.keycode = kc;
    ev.xkey.same_screen = True;
    if (k->keysym >= 'A' && k->keysym <= 'Z') ev.xkey.state = ShiftMask;
    push(&ev, NULL);
}

void x11_on_focus(int focused) {
    if (!main_win) return;
    XEvent ev = {0};
    ev.xfocus.type = focused ? FocusIn : FocusOut;
    ev.xfocus.window = main_win;
    ev.xfocus.mode = NotifyNormal;
    ev.xfocus.detail = NotifyNonlinear;
    push(&ev, NULL);
}

void x11_on_close(void) {
    if (!main_win) _exit(0);
    XEvent ev = {0};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = main_win;
    ev.xclient.message_type = atom_wm_protocols;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = atom_wm_delete;
    ev.xclient.data.l[1] = now_ms();
    push(&ev, NULL);
}

static void push_configure(void) {
    XEvent ev = {0};
    ev.xconfigure.type = ConfigureNotify;
    ev.xconfigure.event = ev.xconfigure.window = main_win;
    ev.xconfigure.width = win_w;
    ev.xconfigure.height = win_h;
    push(&ev, NULL);
}

void x11_on_config(int w, int h) {
    the_screen.width = w;
    the_screen.height = h;
}

int x11_window_size(unsigned long win, int *w, int *h) {
    (void)win;
    *w = win_w;
    *h = win_h;
    return main_win != 0;
}

// ---- Game patches ------------------------------------------------------------------------

// The game draws its own mouse pointer every frame; on a touchscreen it's just a stray arrow.
// MouseControl::OnRender skips the pointer images (but not tooltips) when its hide flag is set;
// this turns that check into an unconditional jump. Only for FTL 1.6.13 Linux (FTL.amd64),
// verified byte for byte; anything else is left alone. FTLHOST_POINTER=1 keeps the pointer.
#include <sys/mman.h>
static void patch_hide_pointer(void) {
    const char *keep = getenv("FTLHOST_POINTER");
    if (keep && *keep == '1') return;
    static const unsigned char expect[] = {0x0f, 0x85, 0xb5, 0x01, 0x00, 0x00};   // jne +0x1b5
    unsigned char *at = (unsigned char *)0x53c6abUL;
    // The executable is mapped at its link address (0x400000, not PIE); check before touching.
    FILE *maps = fopen("/proc/self/maps", "r");
    int mapped = 0;
    char line[512];
    while (maps && fgets(line, sizeof line, maps)) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && lo <= (unsigned long)at && (unsigned long)at + 6 <= hi
            && strstr(line, "FTL.amd64")) mapped = 1;
    }
    if (maps) fclose(maps);
    if (!mapped || memcmp(at, expect, sizeof expect)) { SHIM_LOG("pointer patch: game code not as expected, pointer left visible"); return; }
    unsigned long page = (unsigned long)at & ~4095UL;
    if (mprotect((void *)page, 8192, PROT_READ | PROT_WRITE | PROT_EXEC)) { SHIM_LOG("pointer patch: mprotect failed"); return; }
    at[0] = 0x90;   // nop
    at[1] = 0xe9;   // jmp rel32 (same target)
    mprotect((void *)page, 8192, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)at, (char *)at + 6);
    SHIM_LOG("pointer hidden");
}

// ---- Display ---------------------------------------------------------------------------

Display *XOpenDisplay(_Xconst char *name) {
    (void)name;
    if (the_dpy) return the_dpy;
    host_init();
    int sw, sh;
    host_screen_size(&sw, &sh);
    if (pipe2(wake_pipe, O_CLOEXEC | O_NONBLOCK)) return NULL;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&dpy_lock, &a);

    struct _XDisplay *d = calloc(1, sizeof *d);
    the_visual.visualid = VISUAL_ID;
    the_visual.class = TrueColor;
    the_visual.red_mask = 0xff0000;
    the_visual.green_mask = 0x00ff00;
    the_visual.blue_mask = 0x0000ff;
    the_visual.bits_per_rgb = 8;
    the_visual.map_entries = 256;
    the_depth.depth = 24;
    the_depth.nvisuals = 1;
    the_depth.visuals = &the_visual;
    the_screen.display = (Display *)d;
    the_screen.root = ROOT_WINDOW;
    the_screen.width = sw;
    the_screen.height = sh;
    the_screen.mwidth = sw * 254 / 960;   // ~96 dpi
    the_screen.mheight = sh * 254 / 960;
    the_screen.ndepths = 1;
    the_screen.depths = &the_depth;
    the_screen.root_depth = 24;
    the_screen.root_visual = &the_visual;
    the_screen.cmap = DEFAULT_CMAP;
    the_screen.white_pixel = 0xffffff;
    the_screen.black_pixel = 0;
    the_screen.max_maps = the_screen.min_maps = 1;
    the_screen.backing_store = NotUseful;
    the_screen.root_input_mask = 0;

    d->fd = wake_pipe[0];
    d->proto_major_version = 11;
    d->proto_minor_version = 0;
    d->vendor = "ftlhost";
    d->release = 12101000;
    d->byte_order = LSBFirst;
    d->bitmap_unit = 32;
    d->bitmap_pad = 32;
    d->bitmap_bit_order = LSBFirst;
    d->max_request_size = 65535;
    d->display_name = ":0";
    d->default_screen = 0;
    d->nscreens = 1;
    d->screens = &the_screen;
    d->min_keycode = 8;
    d->max_keycode = 255;
    d->resource_base = 0x200000;
    d->resource_mask = 0x1fffff;
    the_dpy = (Display *)d;
    atom_wm_protocols = XInternAtom(the_dpy, "WM_PROTOCOLS", False);
    atom_wm_delete = XInternAtom(the_dpy, "WM_DELETE_WINDOW", False);
    SHIM_LOG("XOpenDisplay: screen %dx%d", sw, sh);
    patch_hide_pointer();
    return the_dpy;
}

int XCloseDisplay(Display *d) { (void)d; return 0; }
Status XInitThreads(void) { return 1; }
void XLockDisplay(Display *d) { (void)d; pthread_mutex_lock(&dpy_lock); }
void XUnlockDisplay(Display *d) { (void)d; pthread_mutex_unlock(&dpy_lock); }
int XFlush(Display *d) { (void)d; return 1; }
int XSync(Display *d, Bool discard) {
    (void)d;
    if (discard) {
        pthread_mutex_lock(&qlock);
        while (q_count) free(take(0).data);
        pthread_mutex_unlock(&qlock);
    }
    return 1;
}
int (*XSynchronize(Display *d, Bool on))(Display *) { (void)d; (void)on; return NULL; }
static XErrorHandler err_handler;
XErrorHandler XSetErrorHandler(XErrorHandler h) { XErrorHandler o = err_handler; err_handler = h; return o; }
static XIOErrorHandler ioerr_handler;
XIOErrorHandler XSetIOErrorHandler(XIOErrorHandler h) { XIOErrorHandler o = ioerr_handler; ioerr_handler = h; return o; }
int XGetErrorText(Display *d, int code, char *buf, int n) { (void)d; snprintf(buf, n, "X error %d", code); return 0; }
int XGetErrorDatabaseText(Display *d, _Xconst char *name, _Xconst char *msg, _Xconst char *def, char *buf, int n) {
    (void)d; (void)name; (void)msg;
    snprintf(buf, n, "%s", def ? def : "");
    return 0;
}
int XFree(void *p) { free(p); return 1; }
int XResetScreenSaver(Display *d) { (void)d; return 1; }
int XGrabServer(Display *d) { (void)d; return 1; }
int XUngrabServer(Display *d) { (void)d; return 1; }

char **XListExtensions(Display *d, int *n) {
    (void)d;
    char **l = malloc(2 * sizeof *l);
    l[0] = strdup("XInputExtension");
    l[1] = NULL;
    *n = 1;
    return l;
}
int XFreeExtensionList(char **l) {
    if (l) { for (char **p = l; *p; p++) free(*p); free(l); }
    return 1;
}
Bool XQueryExtension(Display *d, _Xconst char *name, int *op, int *ev, int *err) {
    (void)d;
    SHIM_LOG("XQueryExtension %s", name ? name : "(null)");
    if (name && !strcmp(name, "XInputExtension")) {
        *op = XI_OPCODE; *ev = XI_FIRST_EVENT; *err = XI_FIRST_ERROR;
        return True;
    }
    *op = *ev = *err = 0;
    return False;
}

Atom XInternAtom(Display *d, _Xconst char *name, Bool only_if_exists) {
    (void)d;
    if (!name) return None;
    for (int i = 0; i < n_atoms; i++) if (!strcmp(atom_names[i], name)) return 100 + i;
    if (only_if_exists || n_atoms == MAX_ATOMS) return None;
    atom_names[n_atoms] = strdup(name);
    return 100 + n_atoms++;
}
char *XGetAtomName(Display *d, Atom a) {
    (void)d;
    if (a >= 100 && a < 100 + (Atom)n_atoms) return strdup(atom_names[a - 100]);
    return NULL;
}

// ---- Windows and resources ------------------------------------------------------------------

Window XCreateWindow(Display *d, Window parent, int x, int y, unsigned w, unsigned h, unsigned bw,
                     int depth, unsigned cls, Visual *vis, unsigned long mask, XSetWindowAttributes *attr) {
    (void)d; (void)parent; (void)x; (void)y; (void)bw; (void)depth; (void)cls; (void)vis;
    Window id = ++next_id;
    if (!main_win) {
        main_win = id;
        win_w = w;
        win_h = h;
        if (attr && (mask & CWEventMask)) win_mask = attr->event_mask;
        SHIM_LOG("XCreateWindow %ux%u", w, h);
    }
    return id;
}
int XDestroyWindow(Display *d, Window w) { (void)d; if (w == main_win) main_win = 0; return 1; }
int XSelectInput(Display *d, Window w, long mask) { (void)d; if (w == main_win) win_mask = mask; return 1; }

int XMapRaised(Display *d, Window w) {
    (void)d;
    if (w != main_win) return 1;
    XEvent ev = {0};
    if (!win_mapped) {
        win_mapped = 1;
        ev.xmap.type = MapNotify;
        ev.xmap.event = ev.xmap.window = w;
        push(&ev, NULL);
        push_configure();
        memset(&ev, 0, sizeof ev);
        ev.xvisibility.type = VisibilityNotify;
        ev.xvisibility.window = w;
        ev.xvisibility.state = VisibilityUnobscured;
        push(&ev, NULL);
        memset(&ev, 0, sizeof ev);
        ev.xexpose.type = Expose;
        ev.xexpose.window = w;
        ev.xexpose.width = win_w;
        ev.xexpose.height = win_h;
        push(&ev, NULL);
        x11_on_focus(1);
    }
    return 1;
}
int XMapWindow(Display *d, Window w) { return XMapRaised(d, w); }
int XUnmapWindow(Display *d, Window w) {
    (void)d;
    if (w == main_win && win_mapped) {
        win_mapped = 0;
        XEvent ev = {0};
        ev.xunmap.type = UnmapNotify;
        ev.xunmap.event = ev.xunmap.window = w;
        push(&ev, NULL);
    }
    return 1;
}
Status XIconifyWindow(Display *d, Window w, int screen) { (void)d; (void)w; (void)screen; return 1; }
int XMoveWindow(Display *d, Window w, int x, int y) { (void)d; (void)w; (void)x; (void)y; return 1; }
int XResizeWindow(Display *d, Window w, unsigned width, unsigned height) {
    (void)d;
    if (w == main_win && (win_w != (int)width || win_h != (int)height)) {
        win_w = width;
        win_h = height;
        SHIM_LOG("window resized to %ux%u", width, height);
        push_configure();
    }
    return 1;
}
int XMoveResizeWindow(Display *d, Window w, int x, int y, unsigned width, unsigned height) {
    (void)x; (void)y;
    return XResizeWindow(d, w, width, height);
}
Status XQueryTree(Display *d, Window w, Window *root, Window *parent, Window **children, unsigned *n) {
    (void)d; (void)w;
    *root = ROOT_WINDOW;
    *parent = ROOT_WINDOW;
    *children = NULL;
    *n = 0;
    return 1;
}
Bool XTranslateCoordinates(Display *d, Window src, Window dst, int x, int y, int *dx, int *dy, Window *child) {
    (void)d; (void)src; (void)dst;
    *dx = x; *dy = y; *child = None;
    return True;
}
Bool XQueryPointer(Display *d, Window w, Window *root, Window *child, int *rx, int *ry, int *wx, int *wy, unsigned *mask) {
    (void)d; (void)w;
    static int n;
    if (trace_events > 0 && n++ < 5) SHIM_LOG("XQueryPointer -> %.0f,%.0f", ptr_x, ptr_y);
    *root = ROOT_WINDOW; *child = None;
    *rx = *wx = (int)ptr_x;
    *ry = *wy = (int)ptr_y;
    *mask = (ptr_buttons & 2 ? Button1Mask : 0) | (ptr_buttons & 4 ? Button2Mask : 0) | (ptr_buttons & 8 ? Button3Mask : 0);
    return True;
}
int XWarpPointer(Display *d, Window src, Window dst, int sx, int sy, unsigned sw, unsigned sh, int dx, int dy) {
    (void)d; (void)src; (void)dst; (void)sx; (void)sy; (void)sw; (void)sh;
    if (trace_events > 0) SHIM_LOG("XWarpPointer %d,%d", dx, dy);
    ptr_x = dx; ptr_y = dy;
    return 1;
}
int XGrabPointer(Display *d, Window w, Bool oe, unsigned mask, int pm, int km, Window confine, Cursor c, Time t) {
    (void)d; (void)w; (void)oe; (void)mask; (void)pm; (void)km; (void)confine; (void)c; (void)t;
    if (trace_events > 0) SHIM_LOG("XGrabPointer");
    return GrabSuccess;
}
int XUngrabPointer(Display *d, Time t) { (void)d; (void)t; return 1; }

int XChangeProperty(Display *d, Window w, Atom prop, Atom type, int format, int mode, _Xconst unsigned char *data, int n) {
    (void)d; (void)w; (void)type; (void)format; (void)mode;
    const char *name = prop >= 100 && prop < 100 + (Atom)n_atoms ? atom_names[prop - 100] : "";
    if ((!strcmp(name, "_NET_WM_NAME") || !strcmp(name, "WM_NAME")) && data && n > 0) {
        char *t = strndup((const char *)data, n);
        host_set_title(t);
        free(t);
    }
    return 1;
}
int XGetWindowProperty(Display *d, Window w, Atom prop, long off, long len, Bool del, Atom req, Atom *type,
                       int *format, unsigned long *n, unsigned long *after, unsigned char **data) {
    (void)d; (void)w; (void)prop; (void)off; (void)len; (void)del; (void)req;
    *type = None; *format = 0; *n = 0; *after = 0; *data = NULL;
    return Success;
}
Status XSetWMProtocols(Display *d, Window w, Atom *p, int n) { (void)d; (void)w; (void)p; (void)n; return 1; }
void XSetWMNormalHints(Display *d, Window w, XSizeHints *h) { (void)d; (void)w; (void)h; }
int XSetTransientForHint(Display *d, Window w, Window p) { (void)d; (void)w; (void)p; return 1; }
void Xutf8SetWMProperties(Display *d, Window w, _Xconst char *name, _Xconst char *icon, char **argv, int argc,
                          XSizeHints *nh, XWMHints *wh, XClassHint *ch) {
    (void)d; (void)w; (void)icon; (void)argv; (void)argc; (void)nh; (void)wh; (void)ch;
    host_set_title(name);
}
int XSendEvent(Display *d, Window w, Bool prop, long mask, XEvent *ev) {
    (void)d; (void)w; (void)prop; (void)mask; (void)ev;   // EWMH requests to the window manager
    return 1;
}

Colormap XCreateColormap(Display *d, Window w, Visual *v, int alloc) { (void)d; (void)w; (void)v; (void)alloc; return ++next_id; }
int XFreeColormap(Display *d, Colormap c) { (void)d; (void)c; return 1; }
Pixmap XCreatePixmap(Display *d, Drawable dr, unsigned w, unsigned h, unsigned depth) { (void)d; (void)dr; (void)w; (void)h; (void)depth; return ++next_id; }
Pixmap XCreateBitmapFromData(Display *d, Drawable dr, _Xconst char *data, unsigned w, unsigned h) { (void)d; (void)dr; (void)data; (void)w; (void)h; return ++next_id; }
int XFreePixmap(Display *d, Pixmap p) { (void)d; (void)p; return 1; }
Cursor XCreatePixmapCursor(Display *d, Pixmap s, Pixmap m, XColor *fg, XColor *bg, unsigned x, unsigned y) {
    (void)d; (void)s; (void)m; (void)fg; (void)bg; (void)x; (void)y;
    return ++next_id;
}
int XDefineCursor(Display *d, Window w, Cursor c) { (void)d; (void)w; (void)c; return 1; }
int XFreeCursor(Display *d, Cursor c) { (void)d; (void)c; return 1; }
GC XCreateGC(Display *d, Drawable dr, unsigned long mask, XGCValues *v) { (void)d; (void)dr; (void)mask; (void)v; return calloc(1, 256); }
int XFreeGC(Display *d, GC gc) { (void)d; free(gc); return 1; }
int XFillRectangle(Display *d, Drawable dr, GC gc, int x, int y, unsigned w, unsigned h) { (void)d; (void)dr; (void)gc; (void)x; (void)y; (void)w; (void)h; return 1; }
int XDrawString16(Display *d, Drawable dr, GC gc, int x, int y, _Xconst XChar2b *s, int n) { (void)d; (void)dr; (void)gc; (void)x; (void)y; (void)s; (void)n; return 1; }
XImage *XGetImage(Display *d, Drawable dr, int x, int y, unsigned w, unsigned h, unsigned long pm, int f) {
    (void)d; (void)dr; (void)x; (void)y; (void)w; (void)h; (void)pm; (void)f;
    return NULL;
}
XFontStruct *XLoadQueryFont(Display *d, _Xconst char *name) {
    (void)d; (void)name;
    XFontStruct *f = calloc(1, sizeof *f);
    f->ascent = 12; f->descent = 3;
    f->max_bounds.width = f->min_bounds.width = 7;
    f->max_bounds.ascent = 12; f->max_bounds.descent = 3;
    return f;
}
int XFreeFont(Display *d, XFontStruct *f) { (void)d; free(f); return 1; }
int XTextExtents16(XFontStruct *f, _Xconst XChar2b *s, int n, int *dir, int *asc, int *desc, XCharStruct *o) {
    (void)s;
    *dir = 0; *asc = f ? f->ascent : 12; *desc = f ? f->descent : 3;
    memset(o, 0, sizeof *o);
    o->width = 7 * n; o->ascent = *asc; o->descent = *desc; o->rbearing = 7 * n;
    return 1;
}

// ---- Input methods and keys ---------------------------------------------------------------

static int im_dummy, ic_dummy;
char *XSetLocaleModifiers(_Xconst char *m) { (void)m; return ""; }
XIM XOpenIM(Display *d, struct _XrmHashBucketRec *db, char *rn, char *rc) { (void)d; (void)db; (void)rn; (void)rc; return (XIM)&im_dummy; }
Status XCloseIM(XIM im) { (void)im; return 1; }
XIC XCreateIC(XIM im, ...) { (void)im; return (XIC)&ic_dummy; }
void XDestroyIC(XIC ic) { (void)ic; }
void XSetICFocus(XIC ic) { (void)ic; host_text_input(1); }
void XUnsetICFocus(XIC ic) { (void)ic; host_text_input(0); }
char *XGetICValues(XIC ic, ...) {
    (void)ic;
    va_list ap;
    va_start(ap, ic);
    for (;;) {
        const char *name = va_arg(ap, const char *);
        if (!name) break;
        void *out = va_arg(ap, void *);
        if (!strcmp(name, XNFilterEvents) && out) *(unsigned long *)out = 0;
    }
    va_end(ap);
    return NULL;
}
Bool XFilterEvent(XEvent *ev, Window w) { (void)ev; (void)w; return False; }

static int lookup(XKeyEvent *e, char *buf, int n, KeySym *sym) {
    int kc = e->keycode & 255;
    if (sym) *sym = key_syms[kc];
    if (e->type != KeyPress) return 0;
    int len = strlen(key_text[kc]);
    if (len > n) len = 0;
    if (buf && len) memcpy(buf, key_text[kc], len);
    return len;
}
int XLookupString(XKeyEvent *e, char *buf, int n, KeySym *sym, XComposeStatus *st) { (void)st; return lookup(e, buf, n, sym); }
int Xutf8LookupString(XIC ic, XKeyPressedEvent *e, char *buf, int n, KeySym *sym, Status *st) {
    (void)ic;
    int len = lookup(e, buf, n, sym);
    if (st) *st = len ? XLookupBoth : XLookupKeySym;
    return len;
}
KeySym XkbKeycodeToKeysym(Display *d, KeyCode kc, int group, int level) { (void)d; (void)group; (void)level; return key_syms[kc]; }

// ---- Events ----------------------------------------------------------------------------

int XPending(Display *d) { (void)d; pthread_mutex_lock(&qlock); int n = q_count; pthread_mutex_unlock(&qlock); return n; }
int XEventsQueued(Display *d, int mode) { (void)mode; return XPending(d); }

int XNextEvent(Display *d, XEvent *ev) {
    (void)d;
    pthread_mutex_lock(&qlock);
    while (!q_count) pthread_cond_wait(&qcond, &qlock);
    deliver(take(0), ev);
    pthread_mutex_unlock(&qlock);
    return 0;
}
int XPeekEvent(Display *d, XEvent *ev) {
    (void)d;
    pthread_mutex_lock(&qlock);
    while (!q_count) pthread_cond_wait(&qcond, &qlock);
    *ev = queue[q_head].ev;
    if (ev->type == GenericEvent) ev->xcookie.data = NULL;
    pthread_mutex_unlock(&qlock);
    return 0;
}
int XIfEvent(Display *d, XEvent *ev, Bool (*pred)(Display *, XEvent *, XPointer), XPointer arg) {
    pthread_mutex_lock(&qlock);
    for (int seen = 0;;) {
        for (int i = seen; i < q_count; i++) {
            XEvent tmp = queue[(q_head + i) % QSIZE].ev;
            if (pred(d, &tmp, arg)) { deliver(take(i), ev); pthread_mutex_unlock(&qlock); return 0; }
        }
        seen = q_count;
        pthread_cond_wait(&qcond, &qlock);
    }
}
Bool XCheckIfEvent(Display *d, XEvent *ev, Bool (*pred)(Display *, XEvent *, XPointer), XPointer arg) {
    pthread_mutex_lock(&qlock);
    for (int i = 0; i < q_count; i++) {
        XEvent tmp = queue[(q_head + i) % QSIZE].ev;
        if (pred(d, &tmp, arg)) { deliver(take(i), ev); pthread_mutex_unlock(&qlock); return True; }
    }
    pthread_mutex_unlock(&qlock);
    return False;
}

static long mask_of(int type) {
    switch (type) {
    case KeyPress: return KeyPressMask;
    case KeyRelease: return KeyReleaseMask;
    case ButtonPress: return ButtonPressMask;
    case ButtonRelease: return ButtonReleaseMask;
    case MotionNotify: return PointerMotionMask;
    case FocusIn: case FocusOut: return FocusChangeMask;
    case Expose: return ExposureMask;
    case VisibilityNotify: return VisibilityChangeMask;
    case MapNotify: case UnmapNotify: case ConfigureNotify: return StructureNotifyMask;
    default: return 0;
    }
}
int XWindowEvent(Display *d, Window w, long mask, XEvent *ev) {
    (void)d;
    pthread_mutex_lock(&qlock);
    for (int seen = 0;;) {
        for (int i = seen; i < q_count; i++) {
            XEvent *e = &queue[(q_head + i) % QSIZE].ev;
            if (e->xany.window == w && (mask_of(e->type) & mask)) { deliver(take(i), ev); pthread_mutex_unlock(&qlock); return 0; }
        }
        seen = q_count;
        pthread_cond_wait(&qcond, &qlock);
    }
}

Bool XGetEventData(Display *d, XGenericEventCookie *c) {
    (void)d;
    if (trace_events > 0) SHIM_LOG("XGetEventData cookie %u", c->cookie);
    if (c->type != GenericEvent) return False;
    pthread_mutex_lock(&qlock);
    for (cookie_slot **p = &cookies; *p; p = &(*p)->next) {
        if ((*p)->cookie == c->cookie) {
            cookie_slot *s = *p;
            *p = s->next;
            c->data = s->data;
            free(s);
            pthread_mutex_unlock(&qlock);
            return True;
        }
    }
    pthread_mutex_unlock(&qlock);
    return False;
}
void XFreeEventData(Display *d, XGenericEventCookie *c) {
    (void)d;
    if (c->type == GenericEvent && c->data) { free(c->data); c->data = NULL; }
}

// ---- XInput 2 ----------------------------------------------------------------------------

Status XIQueryVersion(Display *d, int *maj, int *min) {
    (void)d;
    SHIM_LOG("XIQueryVersion asked %d.%d", *maj, *min);
    *maj = 2; *min = 2;
    return Success;
}

XIDeviceInfo *XIQueryDevice(Display *d, int deviceid, int *n) {
    (void)d;
    SHIM_LOG("XIQueryDevice %d", deviceid);
    // Master pointer (buttons, x/y, direct touch) and master keyboard; slaves when asked for all.
    int all = deviceid == XIAllDevices;
    int count = all ? 4 : 2;
    XIDeviceInfo *info = calloc(count, sizeof *info);
    static XIButtonClassInfo btn;
    static XIValuatorClassInfo vx, vy;
    static XITouchClassInfo touch;
    static XIAnyClassInfo *ptr_classes[4];
    static Atom labels[8];
    static unsigned char state_mask[4];
    btn.type = XIButtonClass; btn.sourceid = DEV_MOUSE; btn.num_buttons = 7; btn.labels = labels;
    btn.state.mask_len = 4; btn.state.mask = state_mask;
    vx.type = vy.type = XIValuatorClass; vx.sourceid = vy.sourceid = DEV_TOUCH;
    vx.number = 0; vy.number = 1;
    vx.max = the_screen.width; vy.max = the_screen.height;
    vx.resolution = vy.resolution = 1; vx.mode = vy.mode = XIModeAbsolute;
    touch.type = XITouchClass; touch.sourceid = DEV_TOUCH; touch.mode = XIDirectTouch; touch.num_touches = 10;
    ptr_classes[0] = (XIAnyClassInfo *)&btn;
    ptr_classes[1] = (XIAnyClassInfo *)&vx;
    ptr_classes[2] = (XIAnyClassInfo *)&vy;
    ptr_classes[3] = (XIAnyClassInfo *)&touch;
    // No touch class unless asked for: with one, FTL takes all pointer input through XI2, and
    // it only learns XI2's opcode after an inotify watch on /dev/input succeeds -- which an
    // Android app may not do -- so every event would be dropped. Without it the game uses core
    // pointer events. (The Linux build's UI ignores raw touches anyway; the app turns touches
    // into mouse actions.)
    static int with_touch = -1;
    if (with_touch < 0) { const char *t = getenv("FTLHOST_XI_TOUCH"); with_touch = t && *t == '1'; }
    int nptr = with_touch ? 4 : 3;
    info[0].deviceid = DEV_MASTER_POINTER; info[0].name = "Virtual core pointer";
    info[0].use = XIMasterPointer; info[0].attachment = DEV_MASTER_KEYBOARD; info[0].enabled = True;
    info[0].num_classes = nptr; info[0].classes = ptr_classes;
    info[1].deviceid = DEV_MASTER_KEYBOARD; info[1].name = "Virtual core keyboard";
    info[1].use = XIMasterKeyboard; info[1].attachment = DEV_MASTER_POINTER; info[1].enabled = True;
    if (all) {
        info[2].deviceid = DEV_TOUCH; info[2].name = "Android touchscreen";
        info[2].use = XISlavePointer; info[2].attachment = DEV_MASTER_POINTER; info[2].enabled = True;
        info[2].num_classes = nptr; info[2].classes = ptr_classes;
        info[3].deviceid = DEV_MOUSE; info[3].name = "Android mouse";
        info[3].use = XISlavePointer; info[3].attachment = DEV_MASTER_POINTER; info[3].enabled = True;
        info[3].num_classes = 1; info[3].classes = ptr_classes;
    }
    if (deviceid != XIAllDevices && deviceid != XIAllMasterDevices) {   // one device
        for (int i = 0; i < count; i++) if (info[i].deviceid == deviceid) { info[0] = info[i]; *n = 1; return info; }
        free(info); *n = 0; return NULL;
    }
    *n = count;
    return info;
}
void XIFreeDeviceInfo(XIDeviceInfo *info) { free(info); }

Status XISelectEvents(Display *d, Window w, XIEventMask *masks, int n) {
    (void)d; (void)w;
    for (int i = 0; i < n; i++) {
        XIEventMask *m = &masks[i];
        int has = 0;
        if (m->mask_len * 8 > XI_TouchEnd && XIMaskIsSet(m->mask, XI_TouchBegin)) touches_selected = 1, has = 1;
        if (m->mask_len * 8 > XI_ButtonRelease && XIMaskIsSet(m->mask, XI_ButtonPress)) buttons_selected = 1, has = 1;
        SHIM_LOG("XISelectEvents device %d%s%s", m->deviceid, touches_selected ? " touch" : "",
                 buttons_selected ? " buttons" : "");
        (void)has;
    }
    return Success;
}

// box64's libX11 wrapper bridges these two Xlib hooks at load time; they must point somewhere.
static void lock_mutex_noop(LockInfoPtr l) { (void)l; }
void (*_XLockMutex_fn)(LockInfoPtr) = lock_mutex_noop;
void (*_XUnlockMutex_fn)(LockInfoPtr) = lock_mutex_noop;
