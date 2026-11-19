// Connection to the Android app, or a headless stand-in for tests from adb shell.
#define _GNU_SOURCE
#include "shim.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int sock = -1;
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static int screen_w = 1280, screen_h = 720, refresh = 60;
static int got_config;
static pthread_mutex_t cfg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cfg_cond = PTHREAD_COND_INITIALIZER;

// Frames
static uint8_t *frames;
static int frame_max_w, frame_max_h;
static volatile int frame_busy[FTLHOST_FRAMES];   // held by the app
static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t frame_cond = PTHREAD_COND_INITIALIZER;
static int last_index = -1;

// Headless
static const char *dump_dir;
static int dump_every = 60, dump_every_set = 60, frame_count;

static int send_msg(uint32_t type, const void *data, uint32_t len, int fd) {
    if (sock < 0) return -1;
    msg_header h = {type, len};
    struct iovec iov[2] = {{&h, sizeof h}, {(void *)data, len}};
    struct msghdr m = {0};
    m.msg_iov = iov;
    m.msg_iovlen = len ? 2 : 1;
    char cbuf[CMSG_SPACE(sizeof(int))];
    if (fd >= 0) {
        memset(cbuf, 0, sizeof cbuf);
        m.msg_control = cbuf;
        m.msg_controllen = sizeof cbuf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof fd);
    }
    pthread_mutex_lock(&send_lock);
    ssize_t r = sendmsg(sock, &m, MSG_NOSIGNAL);
    pthread_mutex_unlock(&send_lock);
    return r < 0 ? -1 : 0;
}

static int read_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= r;
    }
    return 0;
}

static void apply_config(const msg_config *c) {
    pthread_mutex_lock(&cfg_lock);
    int changed = got_config && (screen_w != (int)c->screen_w || screen_h != (int)c->screen_h);
    screen_w = c->screen_w;
    screen_h = c->screen_h;
    if (c->refresh_hz) refresh = c->refresh_hz;
    got_config = 1;
    pthread_cond_broadcast(&cfg_cond);
    pthread_mutex_unlock(&cfg_lock);
    if (changed) x11_on_config(screen_w, screen_h);
}

static void *reader(void *arg) {
    (void)arg;
    for (;;) {
        msg_header h;
        if (read_full(sock, &h, sizeof h)) break;
        uint8_t buf[256];
        if (h.len > sizeof buf) {   // skip what we don't know
            uint8_t junk[256];
            uint32_t left = h.len;
            while (left) {
                uint32_t n = left > sizeof junk ? sizeof junk : left;
                if (read_full(sock, junk, n)) goto out;
                left -= n;
            }
            continue;
        }
        if (h.len && read_full(sock, buf, h.len)) break;
        switch (h.type) {
        case MSG_CONFIG: apply_config((msg_config *)buf); break;
        case MSG_TOUCH: x11_on_touch((msg_touch *)buf); break;
        case MSG_MOUSE: x11_on_mouse((msg_mouse *)buf); break;
        case MSG_KEY: x11_on_key((msg_key *)buf); break;
        case MSG_FOCUS: x11_on_focus(*(uint32_t *)buf); break;
        case MSG_CLOSE: x11_on_close(); break;
        case MSG_FRAME_DONE: {
            uint32_t i = *(uint32_t *)buf;
            pthread_mutex_lock(&frame_lock);
            if (i < FTLHOST_FRAMES) frame_busy[i] = 0;
            pthread_cond_broadcast(&frame_cond);
            pthread_mutex_unlock(&frame_lock);
            break;
        }
        }
    }
out:
    SHIM_LOG("app connection closed, quitting");
    x11_on_close();
    sleep(5);
    _exit(0);
    return NULL;
}

// Headless input: commands appended to a file, one per line.
//   down ID X Y | move ID X Y | up ID X Y | tap X Y | key KEYSYM | text STR | close | dump
static void *fifo_reader(void *arg) {
    const char *path = arg;
    FILE *f;
    while (!(f = fopen(path, "r"))) sleep(1);
    fseek(f, 0, SEEK_END);   // only commands written from now on
    for (;;) {
        char line[256];
        if (!fgets(line, sizeof line, f)) { clearerr(f); usleep(50000); continue; }
        {
            char cmd[16] = {0};
            float x = 0, y = 0;
            unsigned id = 0;
            if (sscanf(line, "%15s", cmd) != 1) continue;
            if (!strcmp(cmd, "down") || !strcmp(cmd, "move") || !strcmp(cmd, "up")) {
                if (sscanf(line, "%*s %u %f %f", &id, &x, &y) != 3) continue;
                msg_touch t = {cmd[0] == 'd' ? TOUCH_DOWN : cmd[0] == 'm' ? TOUCH_MOVE : TOUCH_UP, id, x, y};
                x11_on_touch(&t);
            } else if (!strcmp(cmd, "tap")) {
                if (sscanf(line, "%*s %f %f", &x, &y) != 2) continue;
                msg_touch t = {TOUCH_DOWN, 1, x, y};
                x11_on_touch(&t);
                usleep(80000);
                t.action = TOUCH_UP;
                x11_on_touch(&t);
            } else if (!strcmp(cmd, "click")) {
                if (sscanf(line, "%*s %f %f", &x, &y) != 2) continue;
                msg_mouse m = {MOUSE_MOVE, 0, x, y};
                x11_on_mouse(&m);
                m.action = MOUSE_DOWN; m.button = 1;
                x11_on_mouse(&m);
                usleep(80000);
                m.action = MOUSE_UP;
                x11_on_mouse(&m);
            } else if (!strcmp(cmd, "mdown") || !strcmp(cmd, "mup") || !strcmp(cmd, "mmove")) {
                unsigned b = 1;
                if (!strcmp(cmd, "mmove")) { if (sscanf(line, "%*s %f %f", &x, &y) != 2) continue; }
                else if (sscanf(line, "%*s %u %f %f", &b, &x, &y) != 3) continue;
                msg_mouse m = {cmd[1] == 'd' ? MOUSE_DOWN : cmd[1] == 'u' ? MOUSE_UP : MOUSE_MOVE, b, x, y};
                x11_on_mouse(&m);
            } else if (!strcmp(cmd, "key")) {
                unsigned sym;
                if (sscanf(line, "%*s %x", &sym) != 1) continue;
                msg_key k = {1, sym, {0}};
                if (sym >= 0x20 && sym < 0x7f) k.text[0] = (char)sym;
                x11_on_key(&k);
                usleep(50000);
                k.down = 0;
                x11_on_key(&k);
            } else if (!strcmp(cmd, "close")) {
                x11_on_close();
            } else if (!strcmp(cmd, "dump")) {
                dump_every = 1;
            }
        }
    }
    return NULL;
}

static void init(void) {
    const char *name = getenv(FTLHOST_SOCKET_ENV);
    dump_dir = getenv(FTLHOST_DUMP_ENV);
    const char *e = getenv("FTLHOST_DUMP_EVERY");
    if (e) dump_every = dump_every_set = atoi(e) > 0 ? atoi(e) : 60;
    if (name && *name) {
        int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        size_t n = strlen(name);
        if (n > sizeof a.sun_path - 2) n = sizeof a.sun_path - 2;
        memcpy(a.sun_path + 1, name, n);   // abstract namespace
        if (connect(s, (struct sockaddr *)&a, offsetof(struct sockaddr_un, sun_path) + 1 + n) == 0) {
            sock = s;
            msg_hello hello = {1};
            send_msg(MSG_HELLO, &hello, sizeof hello, -1);
            pthread_t t;
            pthread_create(&t, NULL, reader, NULL);
            // Wait for the app's config so the screen size is right from the start.
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 5;
            pthread_mutex_lock(&cfg_lock);
            while (!got_config)
                if (pthread_cond_timedwait(&cfg_cond, &cfg_lock, &ts)) break;
            pthread_mutex_unlock(&cfg_lock);
            SHIM_LOG("connected to app, screen %dx%d @%d", screen_w, screen_h, refresh);
        } else {
            SHIM_LOG("can't connect to app socket %s: %s", name, strerror(errno));
            close(s);
        }
    }
    if (sock < 0) {
        const char *sz = getenv("FTLHOST_SCREEN");
        if (sz) sscanf(sz, "%dx%d", &screen_w, &screen_h);
        SHIM_LOG("headless, screen %dx%d, dumps %s", screen_w, screen_h, dump_dir ? dump_dir : "off");
    }
    const char *fifo = getenv(FTLHOST_INPUT_ENV);   // test input, with or without the app
    if (fifo) {
        pthread_t t;
        pthread_create(&t, NULL, fifo_reader, (void *)fifo);
    }
}

void host_init(void) { pthread_once(&once, init); }
int host_connected(void) { host_init(); return sock >= 0; }
void host_screen_size(int *w, int *h) { host_init(); *w = screen_w; *h = screen_h; }
int host_refresh_hz(void) { host_init(); return refresh; }

static int ensure_frames(int w, int h) {
    if (frames && w <= frame_max_w && h <= frame_max_h) return 0;
    int mw = w > screen_w ? w : screen_w, mh = h > screen_h ? h : screen_h;
    size_t size = (size_t)mw * mh * 4 * FTLHOST_FRAMES;
    int fd = memfd_create("ftlhost-frames", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size)) { SHIM_LOG("memfd: %s", strerror(errno)); return -1; }
    uint8_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return -1; }
    pthread_mutex_lock(&frame_lock);
    if (frames) munmap(frames, (size_t)frame_max_w * frame_max_h * 4 * FTLHOST_FRAMES);
    frames = p;
    frame_max_w = mw;
    frame_max_h = mh;
    for (int i = 0; i < FTLHOST_FRAMES; i++) frame_busy[i] = 0;
    pthread_mutex_unlock(&frame_lock);
    msg_framebuf fb = {mw, mh, FTLHOST_FRAMES};
    send_msg(MSG_FRAMEBUF, &fb, sizeof fb, fd);
    close(fd);
    return 0;
}

uint8_t *host_frame_begin(int w, int h, int *index) {
    host_init();
    if (sock < 0 && !dump_dir) return NULL;
    if (ensure_frames(w, h)) return NULL;
    pthread_mutex_lock(&frame_lock);
    int idx = -1;
    for (int tries = 0; idx < 0 && tries < 50; tries++) {
        for (int i = 1; i <= FTLHOST_FRAMES; i++) {
            int j = (last_index + i) % FTLHOST_FRAMES;
            if (!frame_busy[j]) { idx = j; break; }
        }
        if (idx < 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 2000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&frame_cond, &frame_lock, &ts);
        }
    }
    pthread_mutex_unlock(&frame_lock);
    if (idx < 0) return NULL;   // the app is stuck; drop the frame
    *index = idx;
    return frames + (size_t)idx * frame_max_w * frame_max_h * 4;
}

static void dump_frame(const uint8_t *px, int w, int h) {
    char path[512];
    snprintf(path, sizeof path, "%s/frame%05d.ppm", dump_dir, frame_count);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6 %d %d 255\n", w, h);
    uint8_t *row = malloc(w * 3);
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *s = px + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) { row[x * 3] = s[x * 4]; row[x * 3 + 1] = s[x * 4 + 1]; row[x * 3 + 2] = s[x * 4 + 2]; }
        fwrite(row, 1, w * 3, f);
    }
    free(row);
    fclose(f);
    char latest[512];
    snprintf(latest, sizeof latest, "%s/latest.ppm", dump_dir);
    unlink(latest);
    link(path, latest);
}

void host_frame_publish(int index, int w, int h) {
    last_index = index;
    frame_count++;
    if (sock >= 0) {
        pthread_mutex_lock(&frame_lock);
        frame_busy[index] = 1;
        pthread_mutex_unlock(&frame_lock);
        msg_frame m = {index, w, h};
        if (dump_dir && frame_count % dump_every == 0) {
            dump_frame(frames + (size_t)index * frame_max_w * frame_max_h * 4, w, h);
            if (dump_every == 1) dump_every = dump_every_set;
        }
        send_msg(MSG_FRAME, &m, sizeof m, -1);
    } else if (dump_dir && frame_count % dump_every == 0) {
        dump_frame(frames + (size_t)index * frame_max_w * frame_max_h * 4, w, h);
        if (dump_every == 1) dump_every = dump_every_set;
    }
}

void host_set_title(const char *t) { if (t) send_msg(MSG_TITLE, t, strlen(t), -1); }
void host_text_input(int on) { uint32_t v = on; send_msg(MSG_TEXTINPUT, &v, 4, -1); }

audio_ring *host_audio_open(int rate, int channels, int nframes) {
    host_init();
    size_t size = sizeof(audio_ring) + (size_t)nframes * channels * 2;
    int fd = memfd_create("ftlhost-audio", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size)) return NULL;
    audio_ring *r = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (r == MAP_FAILED) { close(fd); return NULL; }
    r->rate = rate;
    r->channels = channels;
    r->frames = nframes;
    if (sock >= 0) send_msg(MSG_AUDIOBUF, NULL, 0, fd);
    close(fd);
    return r;
}
