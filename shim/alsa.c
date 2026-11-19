// A stand-in for libasound's PCM playback: the snd_pcm_* subset FTL uses, writing S16 frames
// into a ring the app plays. Without an app the ring is drained at the real rate and dropped.
#define _GNU_SOURCE
#include "shim.h"
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct _snd_pcm_hw_params { unsigned rate, channels; snd_pcm_format_t format; snd_pcm_access_t access;
                            snd_pcm_uframes_t period, buffer; };
struct _snd_pcm_sw_params { snd_pcm_uframes_t avail_min; };
struct _snd_pcm {
    struct _snd_pcm_hw_params hw;
    snd_pcm_uframes_t avail_min;
    int nonblock;
    audio_ring *ring;
    int64_t local_read;            // headless: frames consumed
    struct timespec started;       // headless clock
};

static uint64_t read_pos(snd_pcm_t *p) {
    if (host_connected()) return p->ring->read_pos;
    if (getenv("FTLHOST_AUDIO_STALL")) return 0;   // debugging: audio that never drains
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!p->started.tv_sec) return 0;
    double s = (now.tv_sec - p->started.tv_sec) + (now.tv_nsec - p->started.tv_nsec) / 1e9;
    uint64_t r = (uint64_t)(s * p->hw.rate);
    return r > p->ring->write_pos ? p->ring->write_pos : r;
}
static snd_pcm_sframes_t avail(snd_pcm_t *p) {
    if (!p->ring) return 0;
    return (snd_pcm_sframes_t)p->ring->frames - (snd_pcm_sframes_t)(p->ring->write_pos - read_pos(p));
}

int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode) {
    (void)name;
    if (stream != SND_PCM_STREAM_PLAYBACK) return -ENOENT;
    snd_pcm_t *p = calloc(1, sizeof *p);
    p->hw.rate = 44100; p->hw.channels = 2; p->hw.format = SND_PCM_FORMAT_S16_LE;
    p->hw.period = 1024; p->hw.buffer = 4096;
    p->nonblock = (mode & SND_PCM_NONBLOCK) != 0;
    *pcm = p;
    return 0;
}
int snd_pcm_close(snd_pcm_t *p) {
    if (p && p->ring) p->ring->running = 0;
    free(p);   // the ring stays mapped: the app may still be reading it
    return 0;
}
int snd_pcm_nonblock(snd_pcm_t *p, int nb) { p->nonblock = nb; return 0; }

size_t snd_pcm_hw_params_sizeof(void) { return sizeof(struct _snd_pcm_hw_params); }
size_t snd_pcm_sw_params_sizeof(void) { return sizeof(struct _snd_pcm_sw_params); }
int snd_pcm_hw_params_any(snd_pcm_t *p, snd_pcm_hw_params_t *h) { *h = p->hw; return 0; }
int snd_pcm_hw_params_set_access(snd_pcm_t *p, snd_pcm_hw_params_t *h, snd_pcm_access_t a) {
    (void)p;
    if (a != SND_PCM_ACCESS_RW_INTERLEAVED) return -EINVAL;
    h->access = a;
    return 0;
}
int snd_pcm_hw_params_set_format(snd_pcm_t *p, snd_pcm_hw_params_t *h, snd_pcm_format_t f) {
    (void)p;
    if (f != SND_PCM_FORMAT_S16_LE) return -EINVAL;
    h->format = f;
    return 0;
}
int snd_pcm_hw_params_set_channels(snd_pcm_t *p, snd_pcm_hw_params_t *h, unsigned c) {
    (void)p;
    if (c < 1 || c > 2) return -EINVAL;
    h->channels = c;
    return 0;
}
int snd_pcm_hw_params_set_rate(snd_pcm_t *p, snd_pcm_hw_params_t *h, unsigned r, int dir) { (void)p; (void)dir; h->rate = r; return 0; }
int snd_pcm_hw_params_set_rate_near(snd_pcm_t *p, snd_pcm_hw_params_t *h, unsigned *r, int *dir) { (void)p; h->rate = *r; if (dir) *dir = 0; return 0; }
int snd_pcm_hw_params_set_rate_resample(snd_pcm_t *p, snd_pcm_hw_params_t *h, unsigned v) { (void)p; (void)h; (void)v; return 0; }
static snd_pcm_uframes_t clampf(snd_pcm_uframes_t v, snd_pcm_uframes_t lo, snd_pcm_uframes_t hi) { return v < lo ? lo : v > hi ? hi : v; }
int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *p, snd_pcm_hw_params_t *h, snd_pcm_uframes_t *v, int *dir) {
    (void)p;
    h->period = *v = clampf(*v, 256, 8192);
    if (dir) *dir = 0;
    return 0;
}
int snd_pcm_hw_params_set_buffer_size_near(snd_pcm_t *p, snd_pcm_hw_params_t *h, snd_pcm_uframes_t *v) {
    (void)p;
    h->buffer = *v = clampf(*v, 1024, 32768);
    return 0;
}
int snd_pcm_hw_params_get_buffer_size(const snd_pcm_hw_params_t *h, snd_pcm_uframes_t *v) { *v = h->buffer; return 0; }
int snd_pcm_hw_params_get_buffer_size_min(const snd_pcm_hw_params_t *h, snd_pcm_uframes_t *v) { (void)h; *v = 1024; return 0; }
int snd_pcm_hw_params_get_buffer_size_max(const snd_pcm_hw_params_t *h, snd_pcm_uframes_t *v) { (void)h; *v = 32768; return 0; }
int snd_pcm_hw_params_get_period_size(const snd_pcm_hw_params_t *h, snd_pcm_uframes_t *v, int *dir) { *v = h->period; if (dir) *dir = 0; return 0; }
int snd_pcm_hw_params_get_period_size_min(const snd_pcm_hw_params_t *h, snd_pcm_uframes_t *v, int *dir) { (void)h; *v = 256; if (dir) *dir = 0; return 0; }
int snd_pcm_hw_params_get_period_size_max(const snd_pcm_hw_params_t *h, snd_pcm_uframes_t *v, int *dir) { (void)h; *v = 8192; if (dir) *dir = 0; return 0; }
int snd_pcm_hw_params_get_rate(const snd_pcm_hw_params_t *h, unsigned *v, int *dir) { *v = h->rate; if (dir) *dir = 0; return 0; }

int snd_pcm_hw_params(snd_pcm_t *p, snd_pcm_hw_params_t *h) {
    p->hw = *h;
    if (p->hw.buffer < p->hw.period * 2) p->hw.buffer = p->hw.period * 2;
    p->avail_min = p->hw.period;
    p->ring = host_audio_open(p->hw.rate, p->hw.channels, p->hw.buffer);
    if (!p->ring) return -ENOMEM;
    SHIM_LOG("audio %u Hz, %u ch, period %lu, buffer %lu", p->hw.rate, p->hw.channels, p->hw.period, p->hw.buffer);
    return 0;
}
int snd_pcm_sw_params_current(snd_pcm_t *p, snd_pcm_sw_params_t *s) { s->avail_min = p->avail_min; return 0; }
int snd_pcm_sw_params_set_avail_min(snd_pcm_t *p, snd_pcm_sw_params_t *s, snd_pcm_uframes_t v) { (void)p; s->avail_min = v; return 0; }
int snd_pcm_sw_params(snd_pcm_t *p, snd_pcm_sw_params_t *s) { p->avail_min = s->avail_min ? s->avail_min : 1; return 0; }

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *p, const void *buf, snd_pcm_uframes_t n) {
    if (!p->ring) return -EBADFD;
    audio_ring *r = p->ring;
    if (!r->running) { r->running = 1; clock_gettime(CLOCK_MONOTONIC, &p->started); p->started.tv_sec -= 0; }
    const int16_t *src = buf;
    snd_pcm_uframes_t done = 0;
    while (done < n) {
        snd_pcm_sframes_t room = avail(p);
        if (room <= 0) {
            if (p->nonblock) return done ? (snd_pcm_sframes_t)done : -EAGAIN;
            usleep(2000);
            continue;
        }
        snd_pcm_uframes_t k = n - done < (snd_pcm_uframes_t)room ? n - done : (snd_pcm_uframes_t)room;
        for (snd_pcm_uframes_t i = 0; i < k; i++) {
            uint64_t at = (r->write_pos + i) % r->frames;
            memcpy(&r->data[at * r->channels], &src[(done + i) * r->channels], r->channels * 2);
        }
        __atomic_store_n(&r->write_pos, r->write_pos + k, __ATOMIC_RELEASE);
        done += k;
    }
    return done;
}

int snd_pcm_wait(snd_pcm_t *p, int timeout_ms) {
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (avail(p) >= (snd_pcm_sframes_t)p->avail_min) return 1;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
        if (timeout_ms >= 0 && ms >= timeout_ms) return 0;
        usleep(2000);
    }
}
int snd_pcm_recover(snd_pcm_t *p, int err, int silent) { (void)p; (void)silent; return err == -EPIPE || err == -ESTRPIPE ? 0 : err; }
int snd_pcm_drain(snd_pcm_t *p) {
    for (int i = 0; i < 500 && p->ring && p->ring->write_pos > read_pos(p); i++) usleep(2000);
    return 0;
}
int snd_pcm_drop(snd_pcm_t *p) {
    if (p->ring) __atomic_store_n(&p->ring->write_pos, read_pos(p), __ATOMIC_RELEASE);
    return 0;
}
