package local.ftlport;

import android.view.KeyEvent;
import android.view.MotionEvent;

/** Android key codes and mouse buttons to X11 keysyms and button numbers. */
final class Keys {
    static int keysymForCodePoint(int cp) {
        if (cp < 0x100) return cp;           // Latin-1 keysyms are the code points
        return 0x01000000 | cp;              // Unicode keysyms
    }

    static int keysym(KeyEvent e) {
        int k = e.getKeyCode();
        switch (k) {
            case KeyEvent.KEYCODE_ENTER: case KeyEvent.KEYCODE_NUMPAD_ENTER: return 0xff0d;
            case KeyEvent.KEYCODE_DEL: return 0xff08;
            case KeyEvent.KEYCODE_FORWARD_DEL: return 0xffff;
            case KeyEvent.KEYCODE_ESCAPE: return 0xff1b;
            case KeyEvent.KEYCODE_TAB: return 0xff09;
            case KeyEvent.KEYCODE_SPACE: return 0x20;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 0xff51;
            case KeyEvent.KEYCODE_DPAD_UP: return 0xff52;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 0xff53;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 0xff54;
            case KeyEvent.KEYCODE_SHIFT_LEFT: return 0xffe1;
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return 0xffe2;
            case KeyEvent.KEYCODE_CTRL_LEFT: return 0xffe3;
            case KeyEvent.KEYCODE_CTRL_RIGHT: return 0xffe4;
            case KeyEvent.KEYCODE_ALT_LEFT: return 0xffe9;
            case KeyEvent.KEYCODE_ALT_RIGHT: return 0xffea;
            case KeyEvent.KEYCODE_BACK: return 0;   // handled as Escape by the back callback
            case KeyEvent.KEYCODE_VOLUME_UP: case KeyEvent.KEYCODE_VOLUME_DOWN: case KeyEvent.KEYCODE_VOLUME_MUTE: return 0;
        }
        if (k >= KeyEvent.KEYCODE_F1 && k <= KeyEvent.KEYCODE_F12) return 0xffbe + (k - KeyEvent.KEYCODE_F1);
        int u = e.getUnicodeChar(0);         // unshifted: X keysyms for letters are lowercase
        if (u > 0) return keysymForCodePoint(u);
        return 0;
    }

    static int button(int android) {
        switch (android) {
            case MotionEvent.BUTTON_SECONDARY: return 3;
            case MotionEvent.BUTTON_TERTIARY: return 2;
            default: return 1;
        }
    }

    private Keys() {}
}
