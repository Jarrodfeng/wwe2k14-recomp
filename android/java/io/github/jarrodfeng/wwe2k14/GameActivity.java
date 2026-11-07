// wwe2k14 Android port (Session 63, ANDROID_PLAN.md 4.2; Session 71: started by the launcher): the game's activity, in
// its own process (android:process=":game"). SDL's activity (org.libsdl.app, zlib licence, vendored in the SDK) owns
// the window, input and sound; this class names our libraries, the app's files folder and the run's arguments, puts the
// touch controls over the game (TouchOverlay) and asks before Back leaves the game (ANDROID_PLAN F10; Jarrod
// 2026-10-28: "Quit after a confirm", back to the launcher).
package io.github.jarrodfeng.wwe2k14;

import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.net.Uri;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.Gravity;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.RelativeLayout;
import android.widget.TextView;
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

import org.json.JSONObject;

import org.libsdl.app.SDLActivity;

public class GameActivity extends SDLActivity {
    private static final String TAG = "wwe2k14";
    static final String EXTRA_ARGS = "wwe2k14.args", EXTRA_GAME_URI = "wwe2k14.game_uri", EXTRA_TOUCH = "wwe2k14.touch";
    // Session 95: the launcher started by this activity (the game process may still be going down: never bring it back), and
    // this activity started by the launcher only to bring a running game forward (no arguments: never boot from it).
    static final String EXTRA_FROM_GAME = "wwe2k14.from_game", EXTRA_BRING_BACK = "wwe2k14.bring_back";
    // The launcher's placeholder for the game file the game's process opens itself (mobile_settings.h kGameFdToken).
    private static final String GAME_FD_TOKEN = "@GAME_FD@";
    private static final String PRECOMPILE_ARG = "--wwe2k14_precompile=";
    private static final String DIAG_ARG = "--wwe2k14_diag=", DIAG_ARM_ARG = "--wwe2k14_diag_arm=";

    // Session 95 (the diagnostic run, src/diag.h): the game writes <run>/arm<ID>/state.json as its route goes. This activity
    // shows the stage as a line at the bottom and, when the route reaches "done" (or the game's watchdog wrote hang.txt), brings
    // the launcher back the way endPrecompile does, so the launcher can start the next arm. No marker file: the launcher reads
    // the arm's own files.
    private String diagRun, diagArm;
    private TextView diagText;
    private Handler diagHandler;
    private long diagStart, diagDoneSeen;
    private static final long DIAG_DEADLINE_MS = 12 * 60 * 1000;

    private TouchOverlay overlay;
    private OnBackInvokedCallback backCallback;
    private AlertDialog quitDialog;
    private boolean fromArgsFile;
    private BroadcastReceiver batteryReceiver;

    // Session 93 (M6 step 2, the shader translation store): the launcher starts the game once in its precompile mode
    // (--wwe2k14_precompile=FILE, src/precompile.h) before the first PLAY after an install, an update or a setting that
    // changes the store's key. This activity shows its progress ("Preparing shaders 120 / 462") from the progress file,
    // and when the game has written its last state (done / timeout) ends the process and brings the launcher back with a
    // marker file (<files>/precompile_return.txt: the state, or "cancelled"); the launcher then starts the real game.
    // The game itself does not quit in this mode on Android (precompile.h): an app shutdown is the old exit crash.
    private String precompileFile;
    private static final String BOOT_TRIAL_ARG = "--wwe2k14_boot_trial=";  // Session 95 (LADDER)
    private String bootTrial;
    private TextView precompileText;
    private Handler precompileHandler;
    private long precompileStart;
    private static final long PRECOMPILE_DEADLINE_MS = 15 * 60 * 1000;

    // Session 95 (src/android_lifecycle.h): every in-game exit lands in the launcher. The game writes logs/exit_request.txt, this
    // activity polls for it, always, in every kind of run.
    private Handler exitHandler;
    private static final long EXIT_POLL_MS = 400;

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
        // Session 95: Android rebuilding this activity (the process was killed in the background and the player came back, or a
        // bring-back found no game) would start a whole new game with the old arguments. Back to the launcher instead, which says
        // what ended the run. The process ends here, so SDL's onCreate never runs.
        if (!fromArgsFile && (savedInstanceState != null || getIntent().getBooleanExtra(EXTRA_BRING_BACK, false))) {
            Log.i(TAG, "rebuilt by Android: back to the launcher");
            blackBoxNote("Android rebuilt the game's activity after the run ended: back to the launcher, no new game");
            // This process ends by its own kill below: the launcher must not read that exit as the run's end.
            try {
                File logs = new File(files, "logs");
                if (logs.isDirectory() || logs.mkdirs())
                    try (FileOutputStream out = new FileOutputStream(new File(logs, "rebuilt_pid.txt"), true)) {
                        out.write((android.os.Process.myPid() + "\n").getBytes(StandardCharsets.UTF_8));
                    }
            } catch (Exception e) {
                Log.w(TAG, "rebuilt_pid.txt", e);
            }
            leaveForLauncher();
            return;
        }
        if (new File(new File(files, "logs"), "exit_request.txt").delete()) Log.i(TAG, "removed an old exit request");
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

        // (SDL calls getArguments() later, on its own thread: the precompile argument is read from the intent here.)
        if (!fromArgsFile) {
            String[] launched = getIntent().getStringArrayExtra(EXTRA_ARGS);
            if (launched != null)
                for (String a : launched)
                    if (a.startsWith(PRECOMPILE_ARG)) precompileFile = a.substring(PRECOMPILE_ARG.length());
                    else if (a.startsWith(BOOT_TRIAL_ARG)) bootTrial = a.substring(BOOT_TRIAL_ARG.length());
                    else if (a.startsWith(DIAG_ARG)) diagRun = a.substring(DIAG_ARG.length());
                    else if (a.startsWith(DIAG_ARM_ARG)) diagArm = a.substring(DIAG_ARM_ARG.length());
        }
        if (precompileFile != null && mLayout != null) startPrecompileWatch();
        if (diagRun != null && diagArm != null && mLayout != null) startDiagWatch();
        exitHandler = new Handler(Looper.getMainLooper());
        exitHandler.postDelayed(this::pollExitRequest, EXIT_POLL_MS);

        // Back asks first (Android 13+ with predictive back: this callback; older: onBackPressed below).
        if (Build.VERSION.SDK_INT >= 33) {
            backCallback = this::askQuit;
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback);
        }

        // Session 80: the battery's temperature for the performance overlay (src/perf_overlay.h). Android's battery
        // broadcast, which every app may receive: at once (it is sticky) and whenever it changes.
        batteryReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                int tenths = intent.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE);
                if (tenths == Integer.MIN_VALUE) return;
                try {
                    nativeBatteryTemperature(tenths);
                } catch (UnsatisfiedLinkError e) {
                    // (The game's library not loaded yet: the next broadcast brings it.)
                }
            }
        };
        registerReceiver(batteryReceiver, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
    }

    @Override
    protected void onDestroy() {
        if (precompileHandler != null) precompileHandler.removeCallbacksAndMessages(null);
        if (exitHandler != null) exitHandler.removeCallbacksAndMessages(null);
        if (batteryReceiver != null) {
            try {
                unregisterReceiver(batteryReceiver);
            } catch (IllegalArgumentException e) {
                // (Not registered.)
            }
        }
        if (overlay != null) overlay.release();
        if (Build.VERSION.SDK_INT >= 33 && backCallback != null)
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backCallback);
        super.onDestroy();
        // The native main ended without the game's own exit request (a startup failure, the guest's main thread ending):
        // SDL finishes this activity and the player would land on Home. The launcher instead.
        if (mSDLMainFinished) leaveForLauncher();
    }

    // Session 95 (src/android_lifecycle.h): SDL itself only pauses at onStop, so the shade and the quit dialog change nothing here.
    @Override
    protected void onPause() {
        appInFront(false);
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        appInFront(true);
    }

    private static void appInFront(boolean front) {
        try {
            nativeAppInFront(front);
        } catch (UnsatisfiedLinkError e) {
            // (The game's library not loaded yet: nothing is running to tell.)
        }
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
        if (precompileFile != null) writePrecompileReturn("cancelled");
        if (diagRun != null && diagArm != null) {  // Session 95 (review F14): the player's Back stops the diagnostic chain
            try (FileOutputStream o = new FileOutputStream(new File(new File(new File(diagRun), "arm" + diagArm), "cancel.txt"))) {
                o.write("the player quit to the launcher\n".getBytes(StandardCharsets.UTF_8));
            } catch (Exception e) {
                Log.w(TAG, "could not write cancel.txt: " + e);
            }
        }
        removeBootTrial();  // Session 95 (LADDER): a quit by the player is not a failed boot
        blackBoxNote("the player quit to the launcher");  // Session 73: a normal end (phone_check.h LastRun)
        leaveForLauncher();
    }

    // Every way out of the game: the launcher, marked as started from here, then this process ends at once.
    private void leaveForLauncher() {
        startActivity(new Intent(this, LauncherActivity.class).putExtra(EXTRA_FROM_GAME, true));
        finish();
        overridePendingTransition(0, 0);
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    // Session 95: see exitHandler. The file stays for the report; the next game start removes it.
    private void pollExitRequest() {
        if (new File(new File(filesDir(), "logs"), "exit_request.txt").isFile()) {
            Log.i(TAG, "the game asked for the launcher");
            if (precompileFile != null) writePrecompileReturn("cancelled");
            leaveForLauncher();
            return;
        }
        exitHandler.postDelayed(this::pollExitRequest, EXIT_POLL_MS);
    }

    // Session 95: the diagnostic run's stage line and its end (see the fields above).
    private void startDiagWatch() {
        diagText = new TextView(this);
        diagText.setTextColor(Color.WHITE);
        diagText.setBackgroundColor(Color.argb(160, 0, 0, 0));
        diagText.setTextSize(14);
        diagText.setPadding(24, 8, 24, 8);
        diagText.setText("Diagnostic run: starting");
        RelativeLayout.LayoutParams lp = new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.addRule(RelativeLayout.ALIGN_PARENT_TOP);
        lp.addRule(RelativeLayout.CENTER_HORIZONTAL);
        lp.topMargin = 12;
        mLayout.addView(diagText, lp);
        diagStart = System.currentTimeMillis();
        diagHandler = new Handler(Looper.getMainLooper());
        diagHandler.postDelayed(this::pollDiag, 500);
    }

    private void pollDiag() {
        File dir = new File(new File(diagRun), "arm" + diagArm);
        String stage = "starting";
        try (BufferedReader r = new BufferedReader(
                new InputStreamReader(new FileInputStream(new File(dir, "state.json")), StandardCharsets.UTF_8))) {
            StringBuilder sb = new StringBuilder();
            String l;
            while ((l = r.readLine()) != null) sb.append(l);
            JSONObject j = new JSONObject(sb.toString());
            stage = j.optString("stage", stage);
        } catch (Exception e) {
            // (Not written yet, or caught mid-write: the game renames a whole file into place.)
        }
        diagText.setText("Diagnostic run (arm " + diagArm + "): " + stage);
        long now = System.currentTimeMillis();
        boolean hang = new File(dir, "hang.txt").exists();
        if (stage.equals("done") && diagDoneSeen == 0) diagDoneSeen = now;
        boolean doneGrace = diagDoneSeen != 0 && now - diagDoneSeen > 4000;  // the script's quit and the log flush first
        boolean over = now - diagStart > DIAG_DEADLINE_MS;
        if (hang || doneGrace || over) {
            Log.i(TAG, "diagnostic arm ended: " + (hang ? "hang" : over ? "deadline" : "done"));
            blackBoxNote("the diagnostic arm ended: " + (hang ? "hang" : over ? "deadline" : "done") + " (quit to the launcher)");
            leaveForLauncher();
            return;
        }
        diagHandler.postDelayed(this::pollDiag, 400);
    }

    // Session 93: the precompile's progress line and its end (see the fields above).
    private void startPrecompileWatch() {
        precompileText = new TextView(this);
        precompileText.setTextColor(Color.WHITE);
        precompileText.setBackgroundColor(Color.argb(200, 0, 0, 0));
        precompileText.setTextSize(18);
        precompileText.setGravity(Gravity.CENTER);
        precompileText.setPadding(40, 24, 40, 24);
        precompileText.setText("Preparing shaders...");
        RelativeLayout.LayoutParams lp = new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.addRule(RelativeLayout.ALIGN_PARENT_BOTTOM);
        lp.addRule(RelativeLayout.CENTER_HORIZONTAL);
        lp.bottomMargin = 80;
        mLayout.addView(precompileText, lp);
        precompileStart = System.currentTimeMillis();
        precompileHandler = new Handler(Looper.getMainLooper());
        precompileHandler.postDelayed(this::pollPrecompile, 300);
    }

    private void pollPrecompile() {
        String state = "starting";
        String line = "Preparing shaders...";
        try (BufferedReader r = new BufferedReader(
                new InputStreamReader(new FileInputStream(new File(precompileFile)), StandardCharsets.UTF_8))) {
            StringBuilder sb = new StringBuilder();
            String l;
            while ((l = r.readLine()) != null) sb.append(l);
            JSONObject j = new JSONObject(sb.toString());
            state = j.optString("state", state);
            int m6Done = j.optInt("m6_done", 0), m6Total = j.optInt("m6_total", 0);
            int made = j.optInt("made", 0), total = j.optInt("total", 0);
            if (m6Total > 0 && m6Done < m6Total) line = "Preparing shaders " + m6Done + " / " + m6Total;
            else if (total > 0 && made < total) line = "Preparing pipelines " + made + " / " + total;
            else if (state.equals("done")) line = "Shaders ready";
        } catch (Exception e) {
            // (Not written yet, or caught mid-write: the game renames a whole file into place; the next poll reads it.)
        }
        // Session 95 (LADDER): leaving the app during a precompile ends it half done and the next PLAY starts it again.
        precompileText.setText(state.equals("done") ? line : line + "\nKeep this app open until it finishes");
        boolean over = System.currentTimeMillis() - precompileStart > PRECOMPILE_DEADLINE_MS;
        if (state.equals("done") || state.equals("timeout") || over) {
            Log.i(TAG, "precompile ended: " + (over ? "deadline" : state));
            endPrecompile(over ? "timeout" : state);
            return;
        }
        precompileHandler.postDelayed(this::pollPrecompile, 300);
    }

    private void writePrecompileReturn(String state) {
        try (FileOutputStream out = new FileOutputStream(new File(filesDir(), "precompile_return.txt"), false)) {
            out.write((state + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Exception e) {
            Log.w(TAG, "precompile return marker failed", e);
        }
    }

    // The launcher back (it starts the game), this process ended at once as quitToLauncher does.
    private void endPrecompile(String state) {
        blackBoxNote("the shader precompile ended: " + state + " (quit to the launcher)");  // (LastRun reads that as a normal end)
        writePrecompileReturn(state);
        leaveForLauncher();
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
            if (a.startsWith(PRECOMPILE_ARG)) precompileFile = a.substring(PRECOMPILE_ARG.length());
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

    // Session 80: the battery's temperature in tenths of a degree C (src/perf_overlay.h).
    private static native void nativeBatteryTemperature(int tenths);

    // Session 95: src/android_lifecycle.h.
    private static native void nativeAppInFront(boolean front);

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
    private void removeBootTrial() {
        if (bootTrial != null && new File(bootTrial).delete()) Log.i(TAG, "boot trial file removed (the player quit)");
    }

    private void blackBoxNote(String text) {
        File box = new File(new File(filesDir(), "logs"), "startup.txt");
        long before = box.lastModified();  // (the launcher keys a run on this time: a note added after the run must not make it look like another run)
        try (FileOutputStream out = new FileOutputStream(box, true)) {
            // Session 95 (LADDER): the same "seconds since the process started" column the native lines have, so the launcher can tell an
            // early quit (under ~120 s after the first frame) from a long play.
            double secs = (android.os.SystemClock.elapsedRealtime() - android.os.Process.getStartElapsedRealtime()) / 1000.0;
            out.write((String.format(java.util.Locale.US, "%8.3f  java: ", secs) + text + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Exception e) {
            Log.w(TAG, "black box note failed", e);
        }
        if (before > 0) box.setLastModified(before);
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
