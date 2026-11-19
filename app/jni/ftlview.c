// The app's half of the link (bionic): listens for the game process, shows its frames on the
// Surface, plays its audio ring through AAudio, and sends input to it.
#include "../../shim/host.h"
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "ftlport", __VA_ARGS__)

static int listen_fd = -1, conn = -1;
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t win_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *window;
static int win_geom_w, win_geom_h;
static int screen_w = 1280, screen_h = 720, refresh = 60;

static uint8_t *frames;
static size_t frames_size;
static int frames_max_w, frames_max_h;

static audio_ring *ring;
static size_t ring_size;
static AAudioStream *stream;
static atomic_int audio_paused;

static int send_msg(uint32_t type, const void *data, uint32_t len) {
    if (conn < 0) return -1;
    msg_header h = {type, len};
    struct iovec iov[2] = {{&h, sizeof h}, {(void *)data, len}};
    struct msghdr m = {0};
    m.msg_iov = iov;
    m.msg_iovlen = len ? 2 : 1;
    pthread_mutex_lock(&send_lock);
    ssize_t r = sendmsg(conn, &m, MSG_NOSIGNAL);
    pthread_mutex_unlock(&send_lock);
    return r < 0 ? -1 : 0;
}

// Reads one message; an attached fd lands in *fd.
static int recv_msg(msg_header *h, uint8_t *buf, size_t cap, int *fd) {
    *fd = -1;
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {h, sizeof *h};
    struct msghdr m = {0};
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = cbuf;
    m.msg_controllen = sizeof cbuf;
    ssize_t r;
    do r = recvmsg(conn, &m, MSG_WAITALL); while (r < 0 && errno == EINTR);
    if (r != sizeof *h) return -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(fd, CMSG_DATA(c), sizeof(int));
    if (h->len > cap) return -1;
    size_t got = 0;
    while (got < h->len) {
        r = read(conn, buf + got, h->len - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

static void show_frame(const msg_frame *f) {
    if (!frames || f->index >= FTLHOST_FRAMES || (int)f->w > frames_max_w || (int)f->h > frames_max_h) return;
    const uint8_t *src = frames + (size_t)f->index * frames_max_w * frames_max_h * 4;
    pthread_mutex_lock(&win_lock);
    if (window) {
        if (win_geom_w != (int)f->w || win_geom_h != (int)f->h) {
            ANativeWindow_setBuffersGeometry(window, f->w, f->h, WINDOW_FORMAT_RGBX_8888);
            win_geom_w = f->w;
            win_geom_h = f->h;
        }
        ANativeWindow_Buffer b;
        if (ANativeWindow_lock(window, &b, NULL) == 0) {
            int w = f->w < (uint32_t)b.width ? (int)f->w : b.width;
            int h = f->h < (uint32_t)b.height ? (int)f->h : b.height;
            for (int y = 0; y < h; y++)   // GL rows are bottom-up
                memcpy((uint8_t *)b.bits + (size_t)y * b.stride * 4, src + (size_t)(f->h - 1 - y) * f->w * 4, (size_t)w * 4);
            ANativeWindow_unlockAndPost(window);
        }
    }
    pthread_mutex_unlock(&win_lock);
}

// ---- Audio -------------------------------------------------------------------------------

static aaudio_data_callback_result_t audio_cb(AAudioStream *s, void *user, void *data, int32_t n) {
    (void)s;
    audio_ring *r = user;
    int16_t *out = data;
    int ch = r->channels;
    uint64_t rp = r->read_pos, wp = __atomic_load_n(&r->write_pos, __ATOMIC_ACQUIRE);
    int32_t have = (int32_t)(wp - rp);
    if (have < 0) have = 0;
    int32_t k = atomic_load(&audio_paused) ? 0 : (have < n ? have : n);
    for (int32_t i = 0; i < k; i++) {
        uint64_t at = (rp + i) % r->frames;
        memcpy(&out[i * ch], &r->data[at * ch], ch * 2);
    }
    memset(&out[k * ch], 0, (size_t)(n - k) * ch * 2);
    // Paused: keep draining so the game's audio thread never blocks.
    if (atomic_load(&audio_paused)) k = have < n ? have : n;
    __atomic_store_n(&r->read_pos, rp + k, __ATOMIC_RELEASE);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void audio_open(int fd) {
    audio_ring head;
    if (pread(fd, &head, sizeof head, 0) != sizeof head) return;
    size_t size = sizeof(audio_ring) + (size_t)head.frames * head.channels * 2;
    audio_ring *r = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (r == MAP_FAILED) return;
    if (stream) { AAudioStream_requestStop(stream); AAudioStream_close(stream); stream = NULL; }
    if (ring) munmap(ring, ring_size);
    ring = r;
    ring_size = size;
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, r->channels);
    AAudioStreamBuilder_setSampleRate(b, r->rate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setDataCallback(b, audio_cb, r);
    if (AAudioStreamBuilder_openStream(b, &stream) == AAUDIO_OK) {
        AAudioStream_requestStart(stream);
        LOG("audio %u Hz %u ch, ring %u frames", r->rate, r->channels, r->frames);
    } else {
        stream = NULL;
    }
    AAudioStreamBuilder_delete(b);
}

// ---- Connection thread ---------------------------------------------------------------------

static JavaVM *jvm;
static jobject activity;

static void call_java(const char *name, const char *sig, ...) {
    JNIEnv *env;
    if ((*jvm)->AttachCurrentThread(jvm, &env, NULL)) return;
    jclass c = (*env)->GetObjectClass(env, activity);
    jmethodID m = (*env)->GetMethodID(env, c, name, sig);
    va_list ap;
    va_start(ap, sig);
    if (m) (*env)->CallVoidMethodV(env, activity, m, ap);
    va_end(ap);
    (*env)->DeleteLocalRef(env, c);
}

static void *conn_thread(void *arg) {
    (void)arg;
    for (;;) {
        int c = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) { if (errno == EINTR) continue; break; }
        if (conn >= 0) close(conn);
        conn = c;
        LOG("game connected");
        msg_config cfg = {screen_w, screen_h, refresh, 160};
        send_msg(MSG_CONFIG, &cfg, sizeof cfg);
        call_java("onGameConnected", "()V");
        for (;;) {
            msg_header h;
            uint8_t buf[512];
            int fd;
            if (recv_msg(&h, buf, sizeof buf - 1, &fd)) break;
            switch (h.type) {
            case MSG_FRAMEBUF: {
                msg_framebuf *fb = (msg_framebuf *)buf;
                size_t size = (size_t)fb->max_w * fb->max_h * 4 * fb->count;
                uint8_t *p = fd >= 0 ? mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
                if (p != MAP_FAILED) {
                    if (frames) munmap(frames, frames_size);
                    frames = p;
                    frames_size = size;
                    frames_max_w = fb->max_w;
                    frames_max_h = fb->max_h;
                }
                break;
            }
            case MSG_FRAME: {
                msg_frame *f = (msg_frame *)buf;
                show_frame(f);
                uint32_t idx = f->index;
                send_msg(MSG_FRAME_DONE, &idx, 4);
                break;
            }
            case MSG_AUDIOBUF:
                if (fd >= 0) audio_open(fd);
                break;
            case MSG_TEXTINPUT:
                call_java("onTextInput", "(Z)V", (jboolean)(*(uint32_t *)buf != 0));
                break;
            }
            if (fd >= 0) close(fd);
        }
        LOG("game disconnected");
        close(conn);
        conn = -1;
    }
    return NULL;
}

// ---- JNI -----------------------------------------------------------------------------------

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *r) { (void)r; jvm = vm; return JNI_VERSION_1_6; }

JNIEXPORT jboolean JNICALL Java_local_ftlport_Native_listen(JNIEnv *env, jclass c, jobject act, jstring jname,
                                                             jint w, jint h, jint hz) {
    (void)c;
    if (listen_fd >= 0) return JNI_TRUE;
    activity = (*env)->NewGlobalRef(env, act);
    screen_w = w;
    screen_h = h;
    refresh = hz;
    const char *name = (*env)->GetStringUTFChars(env, jname, NULL);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    size_t n = strlen(name);
    memcpy(a.sun_path + 1, name, n);
    int ok = bind(s, (struct sockaddr *)&a, offsetof(struct sockaddr_un, sun_path) + 1 + n) == 0 && listen(s, 1) == 0;
    (*env)->ReleaseStringUTFChars(env, jname, name);
    if (!ok) { LOG("listen failed: %s", strerror(errno)); close(s); return JNI_FALSE; }
    listen_fd = s;
    pthread_t t;
    pthread_create(&t, NULL, conn_thread, NULL);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_local_ftlport_Native_setSurface(JNIEnv *env, jclass c, jobject surface) {
    (void)c;
    pthread_mutex_lock(&win_lock);
    if (window) ANativeWindow_release(window);
    window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    win_geom_w = win_geom_h = 0;
    pthread_mutex_unlock(&win_lock);
}

JNIEXPORT void JNICALL Java_local_ftlport_Native_mouse(JNIEnv *env, jclass c, jint action, jint button, jfloat x, jfloat y) {
    (void)env; (void)c;
    msg_mouse m = {action, button, x, y};
    send_msg(MSG_MOUSE, &m, sizeof m);
}

JNIEXPORT void JNICALL Java_local_ftlport_Native_key(JNIEnv *env, jclass c, jboolean down, jint keysym, jstring text) {
    (void)c;
    msg_key k = {down, keysym, {0}};
    if (text) {
        const char *t = (*env)->GetStringUTFChars(env, text, NULL);
        strncpy(k.text, t, sizeof k.text - 1);
        (*env)->ReleaseStringUTFChars(env, text, t);
    }
    send_msg(MSG_KEY, &k, sizeof k);
}

JNIEXPORT void JNICALL Java_local_ftlport_Native_focus(JNIEnv *env, jclass c, jboolean on) {
    (void)env; (void)c;
    uint32_t v = on;
    atomic_store(&audio_paused, !on);
    send_msg(MSG_FOCUS, &v, 4);
}

JNIEXPORT void JNICALL Java_local_ftlport_Native_close(JNIEnv *env, jclass c) {
    (void)env; (void)c;
    send_msg(MSG_CLOSE, NULL, 0);
}
