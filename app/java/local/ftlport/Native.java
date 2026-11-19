package local.ftlport;

import android.view.Surface;

/** The app's half of the link to the game process (app/jni/ftlview.c). */
final class Native {
    static { System.loadLibrary("ftlview"); }

    static final int MOUSE_MOVE = 0, MOUSE_DOWN = 1, MOUSE_UP = 2;

    static native boolean listen(MainActivity activity, String socketName, int screenW, int screenH, int refreshHz);
    static native void setSurface(Surface surface);
    static native void mouse(int action, int button, float x, float y);
    static native void key(boolean down, int keysym, String text);
    static native void focus(boolean focused);
    static native void close();

    private Native() {}
}
