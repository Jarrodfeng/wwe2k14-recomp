// wwe2k14 Android port (Session 71, ANDROID_PLAN F3 / F4 / F7): the launcher's activity, the app's entry point. The
// launcher itself is native (src/launcher_mobile, liblauncher.so: Dear ImGui on SDL, the PC launcher's look); this class
// does what only Android can: the file pickers (the game file, DLC, save import / export), the share sheet, the
// update check's web request, and starting the game. The native side calls the j* methods on SDL's thread and gets
// answers through nativeEvent (kinds = mobile_platform.h Event::Kind).
//
// The game runs in its own process (GameActivity, android:process=":game"): PLAY starts it and closes the launcher, so
// each game starts fresh and the launcher's memory is free for it.
package io.github.jarrodfeng.wwe2k14;

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
import java.util.Locale;

import org.libsdl.app.SDLActivity;

public class LauncherActivity extends SDLActivity {
    private static final String TAG = "wwe2k14";
    // mobile_platform.h Event::Kind
    static final int EV_GAME_PICKED = 0, EV_DLC_ADDED = 1, EV_SAVES_EXPORTED = 2, EV_SAVES_IMPORTED = 3, EV_UPDATE = 4,
            EV_BACK = 5, EV_MESSAGE = 6;
    private static final int REQ_GAME = 7101, REQ_DLC = 7102, REQ_EXPORT = 7103, REQ_IMPORT = 7104;

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
        super.onCreate(savedInstanceState);
        // Back = the launcher's own page back (the main page leaves the app). Android 13+ with predictive back
        // (the manifest opts in) sends it here; older versions send the Back key to SDL, which the native side reads.
        if (Build.VERSION.SDK_INT >= 33) {
            backCallback = () -> event(EV_BACK, true, null, null);
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback);
        }
    }

    @Override
    protected void onDestroy() {
        if (Build.VERSION.SDK_INT >= 33 && backCallback != null)
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backCallback);
        super.onDestroy();
        // A fresh process for the next launcher start (SDL's state is per process). The game, if starting, has its own.
        if (isFinishing()) android.os.Process.killProcess(android.os.Process.myPid());
    }

    private static void event(int kind, boolean ok, String a, String b) {
        try {
            nativeEvent(kind, ok, a, b);
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "launcher: native side not loaded", e);
        }
    }

    private File filesDir() {
        File f = getExternalFilesDir(null);
        return f != null ? f : getFilesDir();
    }

    // ------------------------------------------------------------------ called by the native launcher

    public String jFilesDir() { return filesDir().getAbsolutePath(); }

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
            return pfd != null ? pfd.detachFd() : -1;
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
            Intent send = new Intent(Intent.ACTION_SEND_MULTIPLE).setType("text/plain")
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
            startingGame = true;
            startActivity(i);
            finish();
        });
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
            default:
                break;
        }
    }

    private void copyDlc(ArrayList<Uri> uris, File dir) {
        dir.mkdirs();
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
