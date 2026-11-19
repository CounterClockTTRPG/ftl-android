package local.ftlport;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.window.OnBackInvokedDispatcher;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Runs the user's own Linux FTL (FTL.amd64 + ftl.dat) under box64 in a child process and shows
 * it here. See docs/DESIGN.md.
 */
public class MainActivity extends Activity implements SurfaceHolder.Callback {
    static final String TAG = "ftlport";
    static final int GAME_W = 1280, GAME_H = 720;
    static final int PICK_ZIP = 1;

    private static Process game;          // survives activity recreation
    private static boolean listening;

    private boolean trace;
    private FrameLayout root;
    private GameView view;
    private LinearLayout panel;
    private TextView status;
    private final Handler ui = new Handler(Looper.getMainLooper());

    // ---- Lifecycle ---------------------------------------------------------------------------

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        trace = new File(getFilesDir(), "trace").exists();
        root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        view = new GameView(this);
        view.getHolder().addCallback(this);
        root.addView(view, new FrameLayout.LayoutParams(1, 1, Gravity.CENTER));
        root.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> fitView(r - l, b - t));

        panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setGravity(Gravity.CENTER);
        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setTextSize(18);
        status.setGravity(Gravity.CENTER);
        status.setPadding(48, 24, 48, 24);
        panel.addView(status);
        Button pick = new Button(this);
        pick.setText("Import game files (.zip)");
        pick.setOnClickListener(b -> pickZip());
        panel.addView(pick);
        root.addView(panel, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        setContentView(root);
        hideSystemBars();
        getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT, () -> tapKey(0xff1b, null));   // Escape

        if (game != null) {
            panel.setVisibility(View.GONE);
        } else if (gameInstalled()) {
            startGame();
        } else {
            status.setText("FTL's Linux game files are needed.\n\nPick the zip of the Linux depot (or of the "
                    + "game's data folder): it must contain FTL.amd64, ftl.dat and amd64/libsteam_api.so.");
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        Native.focus(true);
    }

    @Override
    protected void onPause() {
        Native.focus(false);
        super.onPause();
    }

    private void hideSystemBars() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) {
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    /** Largest 16:9 rectangle that fits, centred. */
    private void fitView(int w, int h) {
        if (w <= 0 || h <= 0) return;
        int vw = w, vh = w * GAME_H / GAME_W;
        if (vh > h) { vh = h; vw = h * GAME_W / GAME_H; }
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) view.getLayoutParams();
        if (lp.width != vw || lp.height != vh) {
            lp.width = vw;
            lp.height = vh;
            view.post(() -> view.setLayoutParams(lp));
        }
    }

    @Override public void surfaceCreated(SurfaceHolder h) { Native.setSurface(h.getSurface()); }
    @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) { Native.setSurface(h.getSurface()); }
    @Override public void surfaceDestroyed(SurfaceHolder h) { Native.setSurface(null); }

    // ---- Game files ----------------------------------------------------------------------------

    private File gameDir() { return new File(getFilesDir(), "game"); }

    private boolean gameInstalled() {
        File d = gameDir();
        return new File(d, "FTL.amd64").isFile() && new File(d, "ftl.dat").isFile();
    }

    private void pickZip() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, PICK_ZIP);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != PICK_ZIP || res != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        status.setText("Importing...");
        new Thread(() -> {
            String err = importZip(uri);
            ui.post(() -> {
                if (err == null) startGame();
                else status.setText("Import failed: " + err);
            });
        }, "import").start();
    }

    /** Copies FTL.amd64, ftl.dat and amd64/libsteam_api.so out of a zip, wherever they sit in it. */
    private String importZip(Uri uri) {
        File dir = gameDir();
        new File(dir, "amd64").mkdirs();
        int found = 0;
        try (InputStream in = getContentResolver().openInputStream(uri);
             ZipInputStream zip = new ZipInputStream(in)) {
            ZipEntry e;
            byte[] buf = new byte[1 << 16];
            while ((e = zip.getNextEntry()) != null) {
                String n = e.getName();
                String out = null;
                if (n.endsWith("/FTL.amd64") || n.equals("FTL.amd64")) out = "FTL.amd64";
                else if (n.endsWith("/ftl.dat") || n.equals("ftl.dat")) out = "ftl.dat";
                else if (n.endsWith("amd64/libsteam_api.so")) out = "amd64/libsteam_api.so";
                if (out == null) continue;
                File f = new File(dir, out);
                final String label = out;
                ui.post(() -> status.setText("Importing " + label + "..."));
                try (OutputStream o = new FileOutputStream(f)) {
                    int r;
                    while ((r = zip.read(buf)) > 0) o.write(buf, 0, r);
                }
                f.setExecutable(true);
                found++;
            }
        } catch (IOException ex) {
            return ex.getMessage();
        }
        return gameInstalled() ? null : "FTL.amd64 or ftl.dat not found in the zip (" + found + " files found)";
    }

    // ---- Running the game ---------------------------------------------------------------------

    /** Links the runtime (packaged as lib*.so so the installer extracts it) under its real names. */
    private File prepareRuntime() throws IOException {
        File rt = new File(getFilesDir(), "rt");
        rt.mkdirs();
        String libDir = getApplicationInfo().nativeLibraryDir;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(getAssets().open("runtime.txt")))) {
            String line;
            while ((line = r.readLine()) != null) {
                String[] p = line.trim().split(" ");
                if (p.length != 2) continue;
                File link = new File(rt, p[1]);
                link.delete();
                try {
                    android.system.Os.symlink(libDir + "/" + p[0], link.getPath());
                } catch (android.system.ErrnoException ex) {
                    throw new IOException("symlink " + p[1] + ": " + ex.getMessage());
                }
            }
        }
        try (OutputStream o = new FileOutputStream(new File(rt, "freedreno_icd.json"))) {
            o.write(("{\"file_format_version\":\"1.0.1\",\"ICD\":{\"library_path\":\"" + rt
                    + "/libvulkan_freedreno.so\",\"api_version\":\"1.4.0\"}}\n").getBytes());
        }
        return rt;
    }

    private void startGame() {
        panel.setVisibility(View.GONE);
        try {
            File rt = prepareRuntime();
            File home = new File(getFilesDir(), "home");
            new File(home, ".local/share/FasterThanLight").mkdirs();
            File cache = new File(getCacheDir(), "mesa");
            cache.mkdirs();
            String sock = "ftlhost-" + android.os.Process.myPid();
            if (!listening) listening = Native.listen(this, sock, GAME_W, GAME_H, 60);

            List<String> cmd = new ArrayList<>();
            // The launcher turns calls the app sandbox blocks (glibc's set_robust_list, rseq...)
            // into ENOSYS instead of SIGSYS; then glibc's ld.so runs box64 (app/jni/launch.c).
            cmd.add(getApplicationInfo().nativeLibraryDir + "/libgl_launch.so");
            cmd.add(getApplicationInfo().nativeLibraryDir + "/libgl_ld.so");
            cmd.add("--library-path");
            cmd.add(rt.getPath());
            cmd.add(rt.getPath() + "/box64");
            cmd.add("./FTL.amd64");
            // Debugging: files/cmdline.txt replaces the command, one argument per line, with
            // $LIB (native library dir) and $RT (runtime links) expanded.
            File override = new File(getFilesDir(), "cmdline.txt");
            if (override.isFile()) {
                cmd.clear();
                for (String a : java.nio.file.Files.readAllLines(override.toPath()))
                    if (!a.isEmpty()) cmd.add(a.replace("$LIB", getApplicationInfo().nativeLibraryDir)
                            .replace("$RT", rt.getPath()));
                Log.i(TAG, "command override: " + cmd);
            }
            ProcessBuilder pb = new ProcessBuilder(cmd).directory(gameDir()).redirectErrorStream(true);
            Map<String, String> env = pb.environment();
            env.put("HOME", home.getPath());
            env.put("XDG_DATA_HOME", home.getPath() + "/.local/share");
            env.put("XDG_CACHE_HOME", cache.getPath());
            env.put("TMPDIR", getCacheDir().getPath());
            env.put("DISPLAY", ":0");
            env.put("LC_ALL", "");
            env.put("LC_NUMERIC", "C");
            env.put("GLIBC_TUNABLES", "glibc.pthread.rseq=0");
            if (!new File(getFilesDir(), "nosocket").exists()) env.put("FTLHOST_SOCKET", sock);   // debugging
            if (new File(getFilesDir(), "trace").exists()) {   // debugging: trace, test input, frame dumps
                File dump = new File(getFilesDir(), "dump");
                dump.mkdirs();
                env.put("FTLHOST_TRACE", "1");
                env.put("FTLHOST_INPUT", getFilesDir() + "/input");
                env.put("FTLHOST_DUMP", dump.getPath());
                env.put("FTLHOST_DUMP_EVERY", "100000");
            }
            env.put("VK_ICD_FILENAMES", rt.getPath() + "/freedreno_icd.json");
            env.put("BOX64_LD_LIBRARY_PATH", gameDir().getPath() + "/amd64");
            env.put("BOX64_LIBGL", rt.getPath() + "/libGL.so.1");
            env.put("BOX64_LOG", "1");
            env.put("BOX64_NOBANNER", "1");
            env.remove("LD_PRELOAD");
            env.remove("LD_LIBRARY_PATH");
            game = pb.start();
            Process p = game;
            new Thread(() -> pumpLog(p), "game-log").start();
            new Thread(() -> {
                int code = -1;
                try { code = p.waitFor(); } catch (InterruptedException ignored) {}
                Log.i(TAG, "game exited with " + code);
                final int c = code;
                ui.post(() -> gameExited(c));
            }, "game-wait").start();
        } catch (IOException ex) {
            Log.e(TAG, "start failed", ex);
            panel.setVisibility(View.VISIBLE);
            status.setText("Could not start the game: " + ex.getMessage());
        }
    }

    private void pumpLog(Process p) {
        File log = new File(getFilesDir(), "ftl.log");
        try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
             OutputStream o = new FileOutputStream(log)) {
            String line;
            while ((line = r.readLine()) != null) {
                Log.i(TAG, line);
                o.write((line + "\n").getBytes());
                o.flush();
            }
        } catch (IOException ignored) {}
    }

    private void gameExited(int code) {
        game = null;
        if (code == 0) { finishAndRemoveTask(); return; }
        panel.setVisibility(View.VISIBLE);
        status.setText("The game stopped (exit code " + code + ").\nIts log is in files/ftl.log.");
    }

    /** Called from native code when the game connects. */
    @SuppressWarnings("unused")
    void onGameConnected() { Log.i(TAG, "game connected"); }

    /** Called from native code: the game wants (or no longer wants) text. */
    @SuppressWarnings("unused")
    void onTextInput(boolean on) {}

    // ---- Input ---------------------------------------------------------------------------------

    void tapKey(int keysym, String text) {
        Native.key(true, keysym, text);
        ui.postDelayed(() -> Native.key(false, keysym, text), 60);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int sym = Keys.keysym(e);
        if (sym == 0) return super.dispatchKeyEvent(e);
        if (e.getAction() == KeyEvent.ACTION_DOWN) {
            int u = e.getUnicodeChar();
            Native.key(true, sym, u >= 0x20 ? new String(Character.toChars(u)) : null);
        } else if (e.getAction() == KeyEvent.ACTION_UP) {
            Native.key(false, sym, null);
        }
        return true;
    }

    void toggleKeyboard() {
        InputMethodManager imm = getSystemService(InputMethodManager.class);
        view.requestFocus();
        imm.toggleSoftInput(0, 0);
    }

    /**
     * The game view: touches become mouse actions, the way FTL expects them.
     * Tap = left click; hold still = right click; quick flick up = left click and flick down =
     * right click where the flick started (power a system or weapon up / down); slower or
     * sideways movement = left drag; two fingers = pause (space); three fingers = keyboard.
     */
    final class GameView extends SurfaceView {
        static final long HOLD_MS = 300;       // hold still this long: right click
        static final long FLICK_MS = 250;      // a flick finishes within this
        private float downX, downY;
        private boolean dragging, held, multi, flickDone;
        private int maxPointers;
        private long downAt;
        private final float slop, flickDist;
        private final Runnable hold = () -> {
            if (dragging || multi || flickDone) return;
            held = true;
            clickAt(downX, downY, 3);
            performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS);
        };

        GameView(Context c) {
            super(c);
            setFocusable(true);
            setFocusableInTouchMode(true);
            float dp = c.getResources().getDisplayMetrics().density;
            slop = 12 * dp;
            flickDist = 36 * dp;
        }

        float[] toGame(float x, float y) {
            return new float[]{x * GAME_W / Math.max(1, getWidth()), y * GAME_H / Math.max(1, getHeight())};
        }

        /** A click at a view position (the shim spaces press and release by frames). */
        private void clickAt(float x, float y, int button) {
            float[] g = toGame(x, y);
            Native.mouse(Native.MOUSE_MOVE, 0, g[0], g[1]);
            Native.mouse(Native.MOUSE_DOWN, button, g[0], g[1]);
            Native.mouse(Native.MOUSE_UP, button, g[0], g[1]);
        }

        private void startDrag(float x, float y) {
            float[] d = toGame(downX, downY);
            Native.mouse(Native.MOUSE_DOWN, 1, d[0], d[1]);
            float[] g = toGame(x, y);
            Native.mouse(Native.MOUSE_MOVE, 0, g[0], g[1]);
            dragging = true;
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            if (trace) Log.i(TAG, "touch " + MotionEvent.actionToString(e.getAction()) + " " + e.getX() + "," + e.getY()
                    + " view " + getWidth() + "x" + getHeight());
            if (e.isFromSource(android.view.InputDevice.SOURCE_MOUSE)) return mouseEvent(e);
            float[] g = toGame(e.getX(0), e.getY(0));
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    downX = e.getX(); downY = e.getY();
                    downAt = e.getEventTime();
                    dragging = held = multi = flickDone = false;
                    maxPointers = 1;
                    Native.mouse(Native.MOUSE_MOVE, 0, g[0], g[1]);
                    ui.postDelayed(hold, HOLD_MS);
                    return true;
                case MotionEvent.ACTION_POINTER_DOWN:
                    multi = true;
                    maxPointers = Math.max(maxPointers, e.getPointerCount());
                    ui.removeCallbacks(hold);
                    if (dragging) { Native.mouse(Native.MOUSE_UP, 1, g[0], g[1]); dragging = false; }
                    return true;
                case MotionEvent.ACTION_MOVE: {
                    if (multi || held || flickDone) return true;
                    float dx = e.getX() - downX, dy = e.getY() - downY;
                    if (dragging) { Native.mouse(Native.MOUSE_MOVE, 0, g[0], g[1]); return true; }
                    if (Math.hypot(dx, dy) <= slop) return true;
                    ui.removeCallbacks(hold);
                    long t = e.getEventTime() - downAt;
                    boolean vertical = Math.abs(dy) > 2 * Math.abs(dx);
                    if (vertical && t <= FLICK_MS) {
                        if (Math.abs(dy) >= flickDist) {   // a flick: click where it started
                            flickDone = true;
                            clickAt(downX, downY, dy < 0 ? 1 : 3);
                            performHapticFeedback(android.view.HapticFeedbackConstants.CLOCK_TICK);
                        }
                        return true;                       // might still become a flick
                    }
                    startDrag(e.getX(), e.getY());
                    return true;
                }
                case MotionEvent.ACTION_UP:
                    ui.removeCallbacks(hold);
                    if (multi) {
                        if (e.getEventTime() - downAt < 400) {
                            if (maxPointers == 2) tapKey(0x20, " ");   // pause
                            else if (maxPointers >= 3) toggleKeyboard();
                        }
                    } else if (dragging) {
                        Native.mouse(Native.MOUSE_UP, 1, g[0], g[1]);
                    } else if (!held && !flickDone) {
                        if (Math.hypot(e.getX() - downX, e.getY() - downY) <= slop) {
                            clickAt(e.getX(), e.getY(), 1);
                        } else {   // moved vertically but too slowly or too little for a flick
                            startDrag(e.getX(), e.getY());
                            Native.mouse(Native.MOUSE_UP, 1, g[0], g[1]);
                        }
                    }
                    dragging = held = flickDone = false;
                    return true;
                case MotionEvent.ACTION_CANCEL:
                    ui.removeCallbacks(hold);
                    if (dragging) Native.mouse(Native.MOUSE_UP, 1, g[0], g[1]);
                    dragging = held = multi = false;
                    return true;
            }
            return true;
        }

        /** A real mouse goes straight through. */
        private boolean mouseEvent(MotionEvent e) {
            float[] g = toGame(e.getX(), e.getY());
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_BUTTON_PRESS:
                    Native.mouse(Native.MOUSE_DOWN, Keys.button(e.getActionButton()), g[0], g[1]);
                    break;
                case MotionEvent.ACTION_BUTTON_RELEASE:
                    Native.mouse(Native.MOUSE_UP, Keys.button(e.getActionButton()), g[0], g[1]);
                    break;
                default:
                    Native.mouse(Native.MOUSE_MOVE, 0, g[0], g[1]);
            }
            return true;
        }

        @Override
        public boolean onGenericMotionEvent(MotionEvent e) {
            if (e.isFromSource(android.view.InputDevice.SOURCE_MOUSE)) {
                if (e.getActionMasked() == MotionEvent.ACTION_SCROLL) {
                    float[] g = toGame(e.getX(), e.getY());
                    int b = e.getAxisValue(MotionEvent.AXIS_VSCROLL) > 0 ? 4 : 5;
                    Native.mouse(Native.MOUSE_DOWN, b, g[0], g[1]);
                    Native.mouse(Native.MOUSE_UP, b, g[0], g[1]);
                    return true;
                }
                return mouseEvent(e);
            }
            return super.onGenericMotionEvent(e);
        }

        @Override public boolean onCheckIsTextEditor() { return true; }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo info) {
            info.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
            info.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI;
            return new BaseInputConnection(this, false) {
                @Override
                public boolean commitText(CharSequence t, int pos) {
                    t.codePoints().forEach(cp -> {
                        if (cp == '\n') tapKey(0xff0d, null);
                        else tapKey(Keys.keysymForCodePoint(cp), new String(Character.toChars(cp)));
                    });
                    return true;
                }

                @Override
                public boolean deleteSurroundingText(int before, int after) {
                    for (int i = 0; i < Math.max(1, before); i++) tapKey(0xff08, null);
                    return true;
                }

                @Override
                public boolean performEditorAction(int action) {
                    tapKey(0xff0d, null);
                    return true;
                }
            };
        }
    }
}
