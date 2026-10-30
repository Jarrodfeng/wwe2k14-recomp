// wwe2k14 Android port (Session 63, ANDROID_PLAN.md 4.2; Session 71: started by the launcher): the game's activity, in
// its own process (android:process=":game"). SDL's activity (org.libsdl.app, zlib licence, vendored in the SDK) owns
// the window, input and sound; this class names our libraries, the app's files folder and the run's arguments, puts the
// touch controls over the game (TouchOverlay) and asks before Back leaves the game (ANDROID_PLAN F10; Jarrod
// 2026-10-28: "Quit after a confirm", back to the launcher).
package io.github.jarrodfeng.wwe2k14;

import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

import org.libsdl.app.SDLActivity;

public class GameActivity extends SDLActivity {
    private static final String TAG = "wwe2k14";
    static final String EXTRA_ARGS = "wwe2k14.args", EXTRA_GAME_URI = "wwe2k14.game_uri", EXTRA_TOUCH = "wwe2k14.touch";
    // The launcher's placeholder for the game file the game's process opens itself (mobile_settings.h kGameFdToken).
    private static final String GAME_FD_TOKEN = "@GAME_FD@";

    private TouchOverlay overlay;
    private OnBackInvokedCallback backCallback;
    private AlertDialog quitDialog;
    private boolean fromArgsFile;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // The app's own folder (Android/data/<package>/files): the native side makes it the working folder and HOME
        // (logs, user data, files the PC build keeps next to its exe). Set before SDL starts the native thread.
        File files = filesDir();
        try {
            Os.setenv("WWE2K14_FILES_DIR", files.getAbsolutePath(), true);
        } catch (ErrnoException e) {
            Log.e(TAG, "setenv WWE2K14_FILES_DIR failed", e);
        }
        fromArgsFile = new File(files, "args.txt").isFile();
        super.onCreate(savedInstanceState);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // Session 72 part 9: the picture reaches the screen's edges past the camera cut-out. By default a landscape
        // window stops short of a cut-out on the short edge (a black strip on that side) before Android 15; SDL sets
        // the mode on its full-screen switch but only on the window's copy of its parameters, which never reaches the
        // window manager (SDLActivity COMMAND_CHANGE_WINDOW_STYLE: no setAttributes).
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setAttributes(lp);

        // Touch controls (the launcher's choices; test runs started with args.txt get none).
        int[] touch = getIntent().getIntArrayExtra(EXTRA_TOUCH);
        int mode = touch != null && touch.length == 4 ? touch[0] : TouchOverlay.MODE_OFF;
        if (fromArgsFile) mode = TouchOverlay.MODE_OFF;
        if (mode != TouchOverlay.MODE_OFF && mLayout != null) {
            overlay = new TouchOverlay(this, mode, touch[1], touch[2], touch[3] != 0);
            mLayout.addView(overlay, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT));
        }

        // Back asks first (Android 13+ with predictive back: this callback; older: onBackPressed below).
        if (Build.VERSION.SDK_INT >= 33) {
            backCallback = this::askQuit;
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback);
        }
    }

    @Override
    protected void onDestroy() {
        if (overlay != null) overlay.release();
        if (Build.VERSION.SDK_INT >= 33 && backCallback != null)
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backCallback);
        super.onDestroy();
    }

    // The game is landscape only. SDL asks for an orientation from the game's window when it is made: a resizable window
    // with no SDL_HINT_ORIENTATIONS gets FULL_USER (any way the phone is held), which overrode the manifest's
    // sensorLandscape - b41 started from the portrait launcher came up in portrait (Jarrod, 2026-10-28).
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        askQuit();
    }

    // Session 74: the same menu moves the touch controls while they are on the screen (Back while moving them = CANCEL).
    private void askQuit() {
        if (overlay != null && overlay.editing()) {
            overlay.cancelEdit();
            return;
        }
        if (quitDialog != null && quitDialog.isShowing()) return;
        AlertDialog.Builder b = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Quit to the launcher?")
                .setMessage("Anything not saved in the game is lost.")
                .setPositiveButton("Quit", (d, w) -> quitToLauncher())
                .setNegativeButton("Keep playing", null);
        if (overlay != null && overlay.controlsShown())
            b.setNeutralButton("Move touch controls", (d, w) -> overlay.startEdit(null));
        quitDialog = b.create();
        quitDialog.show();
    }

    // The launcher in the same task, then this process ends at once: no game shutdown to wait for (the old exit crash,
    // ANDROID_PLAN known issues, cannot happen this way). finish() first so Android does not bring this activity back.
    private void quitToLauncher() {
        Log.i(TAG, "quit to the launcher");
        blackBoxNote("the player quit to the launcher");  // Session 73: a normal end (phone_check.h LastRun)
        startActivity(new Intent(this, LauncherActivity.class));
        finish();
        overridePendingTransition(0, 0);
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    private File filesDir() {
        File files = getExternalFilesDir(null);
        return files != null ? files : getFilesDir();
    }

    // SDL lives inside librexruntime.so (no libSDL3.so); libmain.so (last) holds SDL_main.
    @Override
    protected String[] getLibraries() {
        return new String[] {"c++_shared", "rexruntime", "main"};
    }

    // A test run's arguments: <files>/args.txt, one argument per line (tools/android/run.ps1 writes it). Read once and
    // deleted, so a normal start afterwards does not reuse test arguments. Empty lines and lines starting with # skip.
    // Otherwise the launcher's (EXTRA_ARGS), with its game-file placeholder replaced by the file this process opens.
    @Override
    protected String[] getArguments() {
        File args = new File(filesDir(), "args.txt");
        List<String> list = new ArrayList<>();
        if (args.isFile()) {
            try (BufferedReader r = new BufferedReader(
                    new InputStreamReader(new FileInputStream(args), StandardCharsets.UTF_8))) {
                String line;
                while ((line = r.readLine()) != null) {
                    line = line.trim();
                    if (!line.isEmpty() && !line.startsWith("#")) list.add(line);
                }
            } catch (Exception e) {
                Log.e(TAG, "reading " + args + " failed", e);
            }
            if (!args.delete()) Log.w(TAG, "could not delete " + args);
            addDriverDirs(list);
            Log.i(TAG, "arguments from args.txt: " + list);
            return list.toArray(new String[0]);
        }
        String[] given = getIntent().getStringArrayExtra(EXTRA_ARGS);
        if (given == null) return new String[0];
        String fd = null;
        for (String a : given) {
            if (a.contains(GAME_FD_TOKEN)) {
                if (fd == null) fd = openGameFile(getIntent().getStringExtra(EXTRA_GAME_URI));
                if (fd == null) continue;  // the game then says it has no game files
                a = a.replace(GAME_FD_TOKEN, fd);
            }
            list.add(a);
        }
        addDriverDirs(list);
        Log.i(TAG, "arguments from the launcher: " + list);
        return list.toArray(new String[0]);
    }

    // Session 73 (low memory): Android's warnings while the game runs go to the game's log and black box
    // (src/blackbox.h, src/android_memory.h: it also hands freed memory back), so a report shows them and the launcher
    // can say why Android closed the game.
    private static native void nativeTrimMemory(int level);

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        Log.w(TAG, "onTrimMemory " + level);
        try {
            nativeTrimMemory(level);
        } catch (UnsatisfiedLinkError e) {
            blackBoxNote("memory low (Android trim level " + level + ")");
        }
    }

    // A line in logs/startup.txt from this side (the native side writes it while the game runs; appended either way).
    private void blackBoxNote(String text) {
        try (FileOutputStream out = new FileOutputStream(new File(new File(filesDir(), "logs"), "startup.txt"), true)) {
            out.write(("    java  " + text + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Exception e) {
            Log.w(TAG, "black box note failed", e);
        }
    }

    // Session 72 (Android drivers, src/android_driver.h): where libadrenotools' hooks and the bundled custom drivers are
    // (the app's native library folder) and a writable folder it needs on Android 9. Unused unless a driver is chosen.
    private void addDriverDirs(List<String> list) {
        list.add("--wwe2k14_android_lib_dir=" + getApplicationInfo().nativeLibraryDir + "/");
        list.add("--wwe2k14_android_tmp_dir=" + getCacheDir().getAbsolutePath() + "/");
    }

    // The picked game file, opened by this process (the launcher keeps lasting read access to it): /proc/self/fd/N.
    private String openGameFile(String uri) {
        if (uri == null) return null;
        try {
            ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
            if (pfd == null) return null;
            return "/proc/self/fd/" + pfd.detachFd();
        } catch (Exception e) {
            Log.e(TAG, "cannot open the game file " + uri, e);
            return null;
        }
    }
}
