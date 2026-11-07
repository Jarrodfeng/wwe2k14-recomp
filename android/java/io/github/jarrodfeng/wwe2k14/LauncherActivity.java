// wwe2k14 Android port (Session 71, ANDROID_PLAN F3 / F4 / F7): the launcher's activity, the app's entry point. The
// launcher itself is native (src/launcher_mobile, liblauncher.so: Dear ImGui on SDL, the PC launcher's look); this class
// does what only Android can: the file pickers (the game file, DLC, save import / export), the share sheet, the
// update check's web request, and starting the game. The native side calls the j* methods on SDL's thread and gets
// answers through nativeEvent (kinds = mobile_platform.h Event::Kind).
//
// The game runs in its own process (GameActivity, android:process=":game"): PLAY starts it and closes the launcher, so
// each game starts fresh and the launcher's memory is free for it.
package io.github.jarrodfeng.wwe2k14;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.ClipData;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.util.Log;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

import org.libsdl.app.SDLActivity;

public class LauncherActivity extends SDLActivity {
    private static final String TAG = "wwe2k14";
    // mobile_platform.h Event::Kind
    static final int EV_GAME_PICKED = 0, EV_DLC_ADDED = 1, EV_SAVES_EXPORTED = 2, EV_SAVES_IMPORTED = 3, EV_UPDATE = 4,
            EV_BACK = 5, EV_MESSAGE = 6, EV_DRIVER_IMPORTED = 7, EV_UPDATE_PROGRESS = 8, EV_UPDATE_DOWNLOADED = 9;
    private static final int REQ_GAME = 7101, REQ_DLC = 7102, REQ_EXPORT = 7103, REQ_IMPORT = 7104, REQ_DRIVER = 7105;

    static native void nativeEvent(int kind, boolean ok, String a, String b);

    private String pendingDir;  // the DLC / save folder of the picker that is open
    private boolean startingGame;
    private OnBackInvokedCallback backCallback;

    @Override
    protected String[] getLibraries() {
        return new String[] {"c++_shared", "launcher"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Session 95: the icon tapped while a game runs brings that game forward; a second launcher here would only start a second
        // game over it. The process ends here, so SDL's onCreate never runs.
        if (!getIntent().getBooleanExtra(GameActivity.EXTRA_FROM_GAME, false) && gameProcessAlive()) {
            Log.i(TAG, "the game is running: bringing it forward");
            startActivity(new Intent(this, GameActivity.class).putExtra(GameActivity.EXTRA_BRING_BACK, true));
            finish();
            overridePendingTransition(0, 0);
            android.os.Process.killProcess(android.os.Process.myPid());
            return;
        }
        writeLastGameExit();  // Session 73: before the native launcher starts (it reads the file at its start)
        writeLastLauncherExit();  // Session 95 (LADDER): the launcher's own process
        if (savedInstanceState != null) pendingDir = savedInstanceState.getString("pendingDir");  // Session 95 (LADDER)
        super.onCreate(savedInstanceState);
        // Back = the launcher's own page back (the main page leaves the app). Android 13+ with predictive back
        // (the manifest opts in) sends it here; older versions send the Back key to SDL, which the native side reads.
        if (Build.VERSION.SDK_INT >= 33) {
            backCallback = () -> event(EV_BACK, true, null, null);
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback);
        }
    }

    // Session 95 (LADDER): the folder a picker is for (DLC, saves, drivers) is kept when Android recreates this activity while the
    // picker is open (rotation, memory pressure); it used to come back null and the copy crashed or went nowhere.
    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        if (pendingDir != null) out.putString("pendingDir", pendingDir);
    }

    @Override
    protected void onDestroy() {
        if (Build.VERSION.SDK_INT >= 33 && backCallback != null)
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backCallback);
        super.onDestroy();
        // A fresh process for the next launcher start (SDL's state is per process). The game, if starting, has its own.
        if (isFinishing()) android.os.Process.killProcess(android.os.Process.myPid());
    }

    // Session 95 (LADDER): how Android ended the launcher's own process the last time (writeLastGameExit reads only ":game"). A launcher
    // that died of a crash or low memory left nothing behind. logs/last_launcher_exit.txt goes into the shared zip.
    private void writeLastLauncherExit() {
        if (Build.VERSION.SDK_INT < 30) return;
        try {
            ActivityManager am = getSystemService(ActivityManager.class);
            List<ApplicationExitInfo> exits = am.getHistoricalProcessExitReasons(getPackageName(), 0, 8);
            for (ApplicationExitInfo e : exits) {
                if (e.getProcessName() == null || !e.getProcessName().equals(getPackageName())) continue;
                int r = e.getReason();
                // (a normal end is the launcher killing its own process after a PLAY: EXIT_SELF / SIGNALED 9 / USER_REQUESTED: not recorded)
                boolean bad = r == ApplicationExitInfo.REASON_CRASH || r == ApplicationExitInfo.REASON_CRASH_NATIVE
                        || r == ApplicationExitInfo.REASON_LOW_MEMORY || r == ApplicationExitInfo.REASON_ANR
                        || r == ApplicationExitInfo.REASON_INITIALIZATION_FAILURE;
                if (!bad) return;
                File logs = new File(filesDir(), "logs");
                if (!logs.isDirectory() && !logs.mkdirs()) return;
                String text = "time: " + new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date(e.getTimestamp()))
                        + "\nreason_code: " + r + "\nstatus: " + e.getStatus() + "\ndescription: " + e.getDescription()
                        + "\nimportance: " + e.getImportance() + "\n";
                try (OutputStream out = new FileOutputStream(new File(logs, "last_launcher_exit.txt"))) {
                    out.write(text.getBytes(StandardCharsets.UTF_8));
                }
                return;
            }
        } catch (Exception e) {
            Log.w(TAG, "could not read the launcher's last exit", e);
        }
    }

    private boolean gameProcessAlive() {
        ActivityManager am = getSystemService(ActivityManager.class);
        List<ActivityManager.RunningAppProcessInfo> running = am != null ? am.getRunningAppProcesses() : null;
        if (running == null) return false;
        for (ActivityManager.RunningAppProcessInfo p : running)
            if (p.processName != null && p.processName.endsWith(":game")) return true;
        return false;
    }

    private static void event(int kind, boolean ok, String a, String b) {
        try {
            nativeEvent(kind, ok, a, b);
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "launcher: native side not loaded", e);
        }
    }

    // Session 73: how Android says the last game process (":game") ended - a native crash, a low-memory kill, not
    // responding, killed... (ApplicationExitInfo, Android 11+) - into logs/last_game_exit.txt for the native launcher
    // (phone_check.h LastRun) and the shared logs; a native crash's tombstone / an ANR's traces as
    // logs/last_game_exit_trace.bin. Each exit written once (its time kept in logs/last_game_exit.seen).
    private void writeLastGameExit() {
        if (Build.VERSION.SDK_INT < 30) return;
        try {
            ActivityManager am = getSystemService(ActivityManager.class);
            List<ApplicationExitInfo> exits = am.getHistoricalProcessExitReasons(getPackageName(), 0, 8);
            ApplicationExitInfo game = null;
            java.util.Set<String> rebuilt = new java.util.HashSet<>();
            try {
                File rp = new File(new File(filesDir(), "logs"), "rebuilt_pid.txt");
                if (rp.isFile())
                    for (String line : new String(java.nio.file.Files.readAllBytes(rp.toPath()), StandardCharsets.UTF_8).split("\\s+"))
                        if (!line.isEmpty()) rebuilt.add(line);
            } catch (Exception ignored) {
            }
            for (ApplicationExitInfo e : exits) {
                if (rebuilt.contains(Integer.toString(e.getPid()))) continue;  // the launcher-bound self-kill of a rebuilt activity
                if (e.getProcessName() != null && e.getProcessName().endsWith(":game")) {
                    game = e;
                    break;
                }
            }
            if (game == null) return;
            File logs = new File(filesDir(), "logs");
            if (!logs.isDirectory() && !logs.mkdirs()) return;
            File seen = new File(logs, "last_game_exit.seen");
            String stamp = Long.toString(game.getTimestamp());
            if (seen.isFile() && stamp.equals(new String(java.nio.file.Files.readAllBytes(seen.toPath()),
                    StandardCharsets.UTF_8).trim())) {
                return;
            }
            String[] reasons = {"UNKNOWN", "EXIT_SELF", "SIGNALED", "LOW_MEMORY", "CRASH", "CRASH_NATIVE", "ANR",
                    "INITIALIZATION_FAILURE", "PERMISSION_CHANGE", "EXCESSIVE_RESOURCE_USAGE", "USER_REQUESTED",
                    "USER_STOPPED", "DEPENDENCY_DIED", "OTHER", "FREEZER", "PACKAGE_STATE_CHANGE", "PACKAGE_UPDATED"};
            int r = game.getReason();
            String text = "time: " + new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date(game.getTimestamp()))
                    + "\nreason: " + (r >= 0 && r < reasons.length ? reasons[r] : "REASON_" + r)
                    + "\nstatus: " + game.getStatus()
                    + "\ndescription: " + game.getDescription()
                    + "\nimportance: " + game.getImportance()
                    + "\npss_kb: " + game.getPss() + "\nrss_kb: " + game.getRss() + "\n";
            try (OutputStream out = new FileOutputStream(new File(logs, "last_game_exit.txt"))) {
                out.write(text.getBytes(StandardCharsets.UTF_8));
            }
            File trace = new File(logs, "last_game_exit_trace.bin");
            if (trace.isFile() && !trace.delete()) Log.w(TAG, "could not delete " + trace);
            try (InputStream in = game.getTraceInputStream()) {
                if (in != null) {
                    try (OutputStream out = new FileOutputStream(trace)) {
                        byte[] buffer = new byte[65536];
                        int n;
                        while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
                    }
                }
            }
            try (OutputStream out = new FileOutputStream(seen)) {
                out.write(stamp.getBytes(StandardCharsets.UTF_8));
            }
            Log.i(TAG, "last game exit: " + text.replace("\n", "; "));
        } catch (Exception e) {
            Log.w(TAG, "could not read the last game exit", e);
        }
    }

    private File filesDir() {
        File f = getExternalFilesDir(null);
        return f != null ? f : getFilesDir();
    }

    // ------------------------------------------------------------------ called by the native launcher

    public String jFilesDir() { return filesDir().getAbsolutePath(); }

    // Session 72 part 8: the player's own drivers, in internal storage (DriverImport: external storage is noexec).
    public String jDriversDir() { return new File(getFilesDir(), "drivers").getAbsolutePath(); }

    public void jImportDriver(String dir) {
        runOnUiThread(() -> {
            pendingDir = dir;
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
            startActivityForResult(i, REQ_DRIVER);
        });
    }

    public int[] jScreenSize() {
        android.graphics.Rect r = Build.VERSION.SDK_INT >= 30 ? getWindowManager().getMaximumWindowMetrics().getBounds() : null;
        if (r != null) return new int[] {r.width(), r.height()};
        android.util.DisplayMetrics m = new android.util.DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(m);
        return new int[] {m.widthPixels, m.heightPixels};
    }

    public void jPickGame() {
        runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            startActivityForResult(i, REQ_GAME);
        });
    }

    /** The picked game file opened for reading; the caller owns the descriptor. -1 when it can't be opened. */
    public int jOpenFd(String uri) {
        try {
            ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
            if (pfd == null) return -1;
            // Session 95 (LADDER): the game reads the file in place by position. A provider that streams (some cloud folders) gives a
            // descriptor that cannot seek: say so now instead of failing later with a half-read disc.
            try {
                android.system.Os.lseek(pfd.getFileDescriptor(), 0, android.system.OsConstants.SEEK_END);
                android.system.Os.lseek(pfd.getFileDescriptor(), 0, android.system.OsConstants.SEEK_SET);
            } catch (android.system.ErrnoException e) {
                Log.w(TAG, "launcher: " + uri + " cannot seek (" + e.getMessage() + "): not usable in place");
                pfd.close();
                return -2;
            }
            return pfd.detachFd();
        } catch (Exception e) {
            Log.w(TAG, "launcher: cannot open " + uri, e);
            return -1;
        }
    }

    public void jAddDlc(String dir) {
        runOnUiThread(() -> {
            pendingDir = dir;
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                    .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            startActivityForResult(i, REQ_DLC);
        });
    }

    public void jShare(String[] paths) {
        runOnUiThread(() -> {
            ArrayList<Uri> uris = new ArrayList<>();
            ClipData clip = null;
            for (String p : paths) {
                File f = new File(p);
                if (!f.isFile()) continue;
                try {
                    Uri u = FileShareProvider.uriFor(filesDir(), f);
                    uris.add(u);
                    if (clip == null) clip = ClipData.newRawUri(f.getName(), u);
                    else clip.addItem(new ClipData.Item(u));
                } catch (Exception e) {
                    Log.w(TAG, "launcher: cannot share " + p, e);
                }
            }
            if (uris.isEmpty()) {
                event(EV_MESSAGE, false, "No log to share yet", null);
                return;
            }
            boolean zip = uris.size() == 1 && uris.get(0).getPath() != null && uris.get(0).getPath().endsWith(".zip");
            Intent send = new Intent(Intent.ACTION_SEND_MULTIPLE).setType(zip ? "application/zip" : "text/plain")
                    .putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris)
                    .putExtra(Intent.EXTRA_SUBJECT, "WWE 2K14 log")
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            send.setClipData(clip);
            startActivity(Intent.createChooser(send, "Share the log"));
        });
    }

    public void jExportSaves(String dir) {
        runOnUiThread(() -> {
            pendingDir = dir;
            String name = "wwe2k14_save_" + new SimpleDateFormat("yyyyMMdd_HHmm", Locale.US).format(new Date()) + ".zip";
            Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                    .setType("application/zip").putExtra(Intent.EXTRA_TITLE, name);
            startActivityForResult(i, REQ_EXPORT);
        });
    }

    public void jImportSaves(String dir) {
        runOnUiThread(() -> {
            pendingDir = dir;
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
            startActivityForResult(i, REQ_IMPORT);
        });
    }

    /** GET the address (GitHub's latest release) on a thread; the body and the HTTP status go back as EV_UPDATE. */
    public void jCheckUpdate(String address) {
        new Thread(() -> {
            String body = "";
            int status = 0;
            HttpURLConnection c = null;
            try {
                // Session 81 test hook, development builds only (android:debuggable): files/update_test.json is a
                // release list as GitHub would answer, so the update screens can be tried without a real release.
                File test = new File(getExternalFilesDir(null), "update_test.json");
                if (isDebuggableBuild() && test.isFile()) {
                    byte[] raw = java.nio.file.Files.readAllBytes(test.toPath());
                    event(EV_UPDATE, true, new String(raw, StandardCharsets.UTF_8), "200");
                    return;
                }
                c = (HttpURLConnection) new URL(address).openConnection();
                c.setConnectTimeout(6000);
                c.setReadTimeout(6000);
                c.setRequestProperty("Accept", "application/vnd.github+json");
                c.setRequestProperty("X-GitHub-Api-Version", "2022-11-28");
                c.setRequestProperty("User-Agent", "wwe2k14-launcher-android");
                status = c.getResponseCode();
                if (status == 200) {
                    try (InputStream in = c.getInputStream()) {
                        ByteArrayOutputStream out = new ByteArrayOutputStream();
                        byte[] buf = new byte[16384];
                        for (int n; (n = in.read(buf)) > 0 && out.size() < (4 << 20); ) out.write(buf, 0, n);
                        body = new String(out.toByteArray(), StandardCharsets.UTF_8);
                    }
                }
            } catch (Exception e) {
                Log.w(TAG, "launcher: update check failed", e);
            } finally {
                if (c != null) c.disconnect();
            }
            event(EV_UPDATE, status == 200, body, Integer.toString(status));
        }, "wwe2k14-update").start();
    }

    // Session 81 (Jarrod): the launcher installs its own updates. The player presses UPDATE NOW; the APK is downloaded
    // to Android/data/<package>/files/update/, checked against GitHub's SHA-256 and then handed to Android's own
    // installer (one tap on its "Update this app?" screen; the first time the player also allows installs from this
    // app). Updates only install over the same signing key. Any failure is an EV_UPDATE_DOWNLOADED with the reason.
    private static final String UPDATE_PREFIX = "https://github.com/Jarrodfeng/wwe2k14-recomp/releases/download/";

    private boolean isDebuggableBuild() {
        return (getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0;
    }

    private File updateDir() {
        File base = getExternalFilesDir(null);
        return base == null ? null : new File(base, "update");
    }

    /** arg = "ADDRESS SHA256". Progress goes back as EV_UPDATE_PROGRESS ("done total"). */
    public void jDownloadUpdate(String arg) {
        new Thread(() -> {
            String err = null;
            File dir = updateDir();
            File part = dir == null ? null : new File(dir, "update.apk.part"), apk = dir == null ? null : new File(dir, "update.apk");
            HttpURLConnection c = null;
            try {
                String[] p = arg.split(" ");
                // (a development build also takes file:/// addresses: a test APK pushed into the app's own folder)
                if (dir == null || p.length != 2 || !(p[0].startsWith(UPDATE_PREFIX) || (isDebuggableBuild() && p[0].startsWith("file:///")))) throw new java.io.IOException("unexpected download address");
                dir.mkdirs();
                if (apk.exists()) apk.delete();
                InputStream src;
                long total;
                if (p[0].startsWith("file:///")) {  // development builds: a file in the app's own folder (test hook)
                    File f = new File(p[0].substring(7));
                    src = new java.io.FileInputStream(f);
                    total = f.length();
                } else {
                    c = (HttpURLConnection) new URL(p[0]).openConnection();
                    c.setConnectTimeout(10000);
                    c.setReadTimeout(20000);
                    c.setRequestProperty("User-Agent", "wwe2k14-launcher-android");
                    int status = c.getResponseCode();
                    if (status != 200) throw new java.io.IOException("GitHub answered " + status);
                    total = c.getContentLengthLong();
                    src = c.getInputStream();
                }
                long done = 0, last = 0;
                java.security.MessageDigest md = java.security.MessageDigest.getInstance("SHA-256");
                try (InputStream in = src; OutputStream out = new FileOutputStream(part)) {
                    byte[] buf = new byte[65536];
                    for (int n; (n = in.read(buf)) > 0; ) {
                        out.write(buf, 0, n);
                        md.update(buf, 0, n);
                        done += n;
                        long now = System.currentTimeMillis();
                        if (now - last > 300) {
                            last = now;
                            event(EV_UPDATE_PROGRESS, true, done + " " + total, null);
                        }
                    }
                }
                StringBuilder hex = new StringBuilder();
                for (byte b : md.digest()) hex.append(String.format(Locale.ROOT, "%02x", b));
                if (!hex.toString().equalsIgnoreCase(p[1]))
                    throw new java.io.IOException("the download did not match GitHub's checksum; nothing was changed");
                if (!part.renameTo(apk)) throw new java.io.IOException("the download could not be saved");
            } catch (Exception e) {
                Log.w(TAG, "launcher: update download failed", e);
                err = (e.getMessage() == null ? e.toString() : e.getMessage());
                if (part != null) part.delete();
            } finally {
                if (c != null) c.disconnect();
            }
            event(EV_UPDATE_DOWNLOADED, err == null, err == null ? "" : err, null);
        }, "wwe2k14-update-dl").start();
    }

    /** Hands the downloaded APK to Android's installer. */
    public void jInstallUpdate() {
        runOnUiThread(() -> {
            try {
                File dir = updateDir();
                File apk = dir == null ? null : new File(dir, "update.apk");
                if (apk == null || !apk.isFile()) {
                    event(EV_MESSAGE, false, "The update file is gone: download it again", null);
                    return;
                }
                if (Build.VERSION.SDK_INT >= 26 && !getPackageManager().canRequestPackageInstalls()) {
                    event(EV_MESSAGE, true, "Allow installs from this app, then come back and press INSTALL again", null);
                    startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                            Uri.parse("package:" + getPackageName())));
                    return;
                }
                Uri u = FileShareProvider.uriFor(getExternalFilesDir(null), apk);
                startActivity(new Intent(Intent.ACTION_VIEW).setDataAndType(u, "application/vnd.android.package-archive")
                        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK));
            } catch (Exception e) {
                Log.w(TAG, "launcher: update install failed", e);
                event(EV_MESSAGE, false, "The update could not be started: " + e.getMessage(), null);
            }
        });
    }

    /** The app is up to date (an update was installed or none was needed): the downloaded file is not needed any more. */
    public void jClearUpdate() {
        new Thread(() -> {
            File dir = updateDir();
            File[] files = dir == null ? null : dir.listFiles();
            if (files != null) for (File f : files) f.delete();
            if (dir != null) dir.delete();
        }, "wwe2k14-update-clean").start();
    }

    public void jOpenUrl(String url) {
        if (url == null || !url.startsWith("https://github.com/")) return;  // only ever GitHub pages
        runOnUiThread(() -> {
            try {
                startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
            } catch (Exception e) {
                Log.w(TAG, "launcher: no browser for " + url, e);
            }
        });
    }

    /** PLAY: the game in its own process with these arguments, then the launcher closes. */
    public void jStartGame(String[] args, String gameUri, int[] touch) {
        runOnUiThread(() -> {
            Intent i = new Intent(this, GameActivity.class)
                    .putExtra(GameActivity.EXTRA_ARGS, args)
                    .putExtra(GameActivity.EXTRA_GAME_URI, gameUri)
                    .putExtra(GameActivity.EXTRA_TOUCH, touch);
            // A fresh activity with these arguments, never Android's saved record of an earlier run (a killed :game).
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startingGame = true;
            startActivity(i);
            finish();
        });
    }

    /** Session 74: MOVE THE CONTROLS (TouchEditActivity) with the launcher's Size and Opacity. */
    public void jEditTouch(int[] look) {
        runOnUiThread(() -> startActivity(new Intent(this, TouchEditActivity.class)
                .putExtra(TouchEditActivity.EXTRA_LOOK, look)));
    }

    public void jQuit() {
        runOnUiThread(this::finish);
    }

    // ------------------------------------------------------------------ picker answers

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) {
        }
        String s = uri.getLastPathSegment();
        return s != null ? s : "file";
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);  // SDL's own file dialogs
        final boolean ok = resultCode == RESULT_OK && data != null;
        switch (requestCode) {
            case REQ_GAME: {
                if (!ok || data.getData() == null) { event(EV_GAME_PICKED, false, null, null); return; }
                Uri uri = data.getData();
                try {
                    getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (SecurityException e) {
                    Log.w(TAG, "launcher: no lasting permission for " + uri, e);
                    // Session 95 (LADDER): without lasting permission the file works now and fails at the next start: say so.
                    event(EV_MESSAGE, true, "Android would not keep access to that file. Choose it again from the phone's own storage (not a cloud or app folder).", null);
                    return;  // not accepted: it would work now and fail at the next start (and the message must stay on screen)
                }
                event(EV_GAME_PICKED, true, uri.toString(), displayName(uri));
                return;
            }
            case REQ_DLC: {
                if (!ok) return;
                ArrayList<Uri> uris = new ArrayList<>();
                if (data.getClipData() != null)
                    for (int k = 0; k < data.getClipData().getItemCount(); k++) uris.add(data.getClipData().getItemAt(k).getUri());
                else if (data.getData() != null) uris.add(data.getData());
                final String dir = pendingDir;
                event(EV_MESSAGE, true, "Copying " + uris.size() + " file(s)...", null);
                new Thread(() -> copyDlc(uris, new File(dir)), "wwe2k14-dlc").start();
                return;
            }
            case REQ_EXPORT: {
                if (!ok || data.getData() == null) return;
                final Uri uri = data.getData();
                final File dir = new File(pendingDir);
                new Thread(() -> {
                    try (OutputStream out = getContentResolver().openOutputStream(uri, "w")) {
                        int n = SaveZip.export(dir, out);
                        event(EV_SAVES_EXPORTED, true, "Save exported (" + n + " files)", null);
                    } catch (Exception e) {
                        Log.w(TAG, "launcher: export failed", e);
                        event(EV_SAVES_EXPORTED, false, "Export failed: " + e.getMessage(), null);
                    }
                }, "wwe2k14-export").start();
                return;
            }
            case REQ_IMPORT: {
                if (!ok || data.getData() == null) return;
                final Uri uri = data.getData();
                final File dir = new File(pendingDir);
                new Thread(() -> {
                    try (InputStream in = getContentResolver().openInputStream(uri)) {
                        int n = SaveZip.importTo(in, dir);
                        event(EV_SAVES_IMPORTED, true, "Save imported (" + n + " files); the old one is kept as a backup", null);
                    } catch (Exception e) {
                        Log.w(TAG, "launcher: import failed", e);
                        event(EV_SAVES_IMPORTED, false, "Import failed: " + e.getMessage(), null);
                    }
                }, "wwe2k14-import").start();
                return;
            }
            case REQ_DRIVER: {
                if (!ok || data.getData() == null) return;
                final Uri uri = data.getData();
                final File dir = new File(pendingDir);
                final String name = displayName(uri);
                event(EV_MESSAGE, true, "Adding " + name + "...", null);
                new Thread(() -> {
                    DriverImport.Result r;
                    try (InputStream in = getContentResolver().openInputStream(uri)) {
                        if (in == null) throw new java.io.IOException("cannot read");
                        r = DriverImport.run(in, name, dir);
                    } catch (Exception e) {
                        Log.w(TAG, "launcher: driver import failed", e);
                        r = new DriverImport.Result();
                        r.message = "Could not read " + name + ": " + e.getMessage();
                    }
                    event(EV_DRIVER_IMPORTED, r.ok, r.message, r.folder);
                }, "wwe2k14-driver").start();
                return;
            }
            default:
                break;
        }
    }

    private long sizeOf(Uri u) {
        try (Cursor c = getContentResolver().query(u, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getLong(0);
        } catch (Exception ignored) {
        }
        return 0;
    }

    private void copyDlc(ArrayList<Uri> uris, File dir) {
        dir.mkdirs();
        // Session 95 (LADDER): the free space is checked before the copy starts, so a full phone says so instead of leaving half a file.
        long need = 0;
        for (Uri u : uris) need += sizeOf(u);
        if (need > 0 && dir.getUsableSpace() < need + (64L << 20)) {
            event(EV_DLC_ADDED, false, "Not enough free storage for these files: they need " + (need >> 20) + " MB and the phone has "
                    + (dir.getUsableSpace() >> 20) + " MB free. Free some space and try again.", null);
            return;
        }
        int copied = 0;
        long bytes = 0;
        String failed = null;
        for (Uri u : uris) {
            String name = displayName(u).replaceAll("[\\\\/:*?\"<>|]", "_");
            File out = new File(dir, name);
            try (InputStream in = getContentResolver().openInputStream(u); OutputStream o = new FileOutputStream(out)) {
                if (in == null) throw new java.io.IOException("cannot read");
                byte[] buf = new byte[1 << 20];
                for (int n; (n = in.read(buf)) > 0; ) { o.write(buf, 0, n); bytes += n; }
                copied++;
            } catch (Exception e) {
                Log.w(TAG, "launcher: DLC copy failed for " + u, e);
                out.delete();
                failed = name;
            }
        }
        String msg = "Copied " + copied + " of " + uris.size() + " file(s), " + (bytes >> 20) + " MB"
                + (failed != null ? " (" + failed + " failed)" : "");
        event(EV_DLC_ADDED, failed == null, msg, null);
    }
}
