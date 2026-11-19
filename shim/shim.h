// Internal interface between the shim's parts (x11.c, glx.c, alsa.c, host.c).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include "host.h"

#define SHIM_LOG(...) do { fprintf(stderr, "[ftlhost] " __VA_ARGS__); fputc('\n', stderr); } while (0)

// host.c
void host_init(void);                 // idempotent; connects or goes headless
int host_connected(void);
void host_screen_size(int *w, int *h);
int host_refresh_hz(void);
// Frame publishing: get a free buffer, fill it, publish.
uint8_t *host_frame_begin(int w, int h, int *index);
void host_frame_publish(int index, int w, int h);
void host_set_title(const char *utf8);
void host_text_input(int on);
audio_ring *host_audio_open(int rate, int channels, int frames);

// Input arriving from the app (called on the host reader thread).
void x11_on_touch(const msg_touch *t);
void x11_on_mouse(const msg_mouse *m);
void x11_on_key(const msg_key *k);
void x11_on_focus(int focused);
void x11_on_close(void);
void x11_on_config(int w, int h);

// x11.c helpers for glx.c
int x11_window_size(unsigned long win, int *w, int *h);
void x11_frame_tick(void);   // after each presented frame
