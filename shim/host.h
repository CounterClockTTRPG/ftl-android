// The link between the game process (glibc, under box64) and the Android app.
// Shared by the shim (C, glibc) and the app's JNI side (C, bionic).
#pragma once
#include <stdint.h>

#define FTLHOST_SOCKET_ENV "FTLHOST_SOCKET"   // abstract unix socket name the app listens on
#define FTLHOST_DUMP_ENV   "FTLHOST_DUMP"     // headless: directory for frame dumps
#define FTLHOST_INPUT_ENV  "FTLHOST_INPUT"    // headless: FIFO with text input commands
#define FTLHOST_FRAMES     3

enum {
    // game -> app
    MSG_HELLO = 1,      // hello: version
    MSG_FRAMEBUF,       // fd attached: frame shm (FTLHOST_FRAMES buffers of max_w*max_h*4)
    MSG_FRAME,          // frame: buffer index, w, h (RGBA, rows bottom-up)
    MSG_AUDIOBUF,       // fd attached: audio ring
    MSG_TITLE,          // utf-8 window title
    MSG_TEXTINPUT,      // u32: 1 = the game wants text (show the keyboard), 0 = done
    // app -> game
    MSG_CONFIG = 100,   // config: screen size, max frame size, refresh
    MSG_TOUCH,          // touch: action, id, x, y in screen pixels
    MSG_KEY,            // key: down, keysym, utf-8 text
    MSG_FOCUS,          // u32 focused
    MSG_FRAME_DONE,     // u32 buffer index the app no longer reads
    MSG_CLOSE,          // the user wants to quit (window close)
    MSG_MOUSE,          // mouse: action, button, x, y
};

typedef struct { uint32_t type, len; } msg_header;

typedef struct { uint32_t version; } msg_hello;
typedef struct { uint32_t max_w, max_h, count; } msg_framebuf;
typedef struct { uint32_t index, w, h; } msg_frame;
typedef struct { uint32_t screen_w, screen_h, refresh_hz, dpi; } msg_config;

enum { TOUCH_DOWN = 0, TOUCH_MOVE = 1, TOUCH_UP = 2, TOUCH_CANCEL = 3 };
typedef struct { uint32_t action, id; float x, y; } msg_touch;

enum { MOUSE_MOVE = 0, MOUSE_DOWN = 1, MOUSE_UP = 2 };
typedef struct { uint32_t action, button; float x, y; } msg_mouse;

typedef struct { uint32_t down, keysym; char text[8]; } msg_key;

// Audio ring in shared memory: interleaved S16, the game writes, the app reads.
typedef struct {
    uint32_t rate, channels, frames;   // frames = capacity
    uint32_t running;
    volatile uint64_t write_pos, read_pos;   // in frames, monotonically increasing
    int16_t data[];
} audio_ring;
