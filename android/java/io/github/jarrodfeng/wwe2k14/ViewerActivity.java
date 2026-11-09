// wwe2k14 Android port (Session 98, Arena Viewer phone port stage C): the Arena Viewer (src/assets/view, libviewer.so) in its own
// process (android:process=":viewer"; SDL's state is per process and the launcher's own SDL lives in liblauncher.so). Started by the
// launcher's MODS page (ARENA VIEWER) with the viewer's arguments and the picked game file, which this process opens itself, as
// GameActivity does. Landscape, the whole screen past the camera cut-out. Back asks first (a look drag from the screen's edge is
// easily read as Back), then this process ends and the launcher below comes back.
package io.github.jarrodfeng.wwe2k14;

import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.WindowManager;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

import org.libsdl.app.SDLActivity;

public class ViewerActivity extends SDLActivity {
    private static final String TAG = "wwe2k14";
    static final String EXTRA_ARGS = "wwe2k14.viewer_args", EXTRA_GAME_URI = "wwe2k14.game_uri", EXTRA_LAUNCH_TOKEN = "wwe2k14.viewer_token";
    static final String LAUNCH_TOKEN_FILE = "viewer_launch.token";  // <files>/: written by LauncherActivity.jStartViewer
    private static final long ARGS_FILE_MAX_AGE_MS = 2 * 60 * 1000;
    // The launcher's placeholder for the game file this process opens itself (mobile_settings.h kGameFdToken).
    private static final String GAME_FD_TOKEN = "@GAME_FD@";

    private OnBackInvokedCallback backCallback;
    private AlertDialog quitDialog;

    // Session 98 (phone test: "the character importing appears greyed out"): IMPORT CHARACTER's file picker (mods_android.cpp). The
    // native side calls jPickCharacter on SDL's thread and waits; Android's picker (the Storage Access Framework) runs over this
    // activity, the picked file (a .pac or a .zip) is copied into the given folder on a worker thread, and nativeCharPicked answers.
    private static final int REQ_CHAR = 7201;
    private static final long CHAR_MAX_BYTES = 1L << 30;  // a zip of a few characters; char_mods.h caps one pac at 400 MB
    private static final int NAME_MAX_UTF8 = 200;         // bytes: the native side and the file system get this name as UTF-8
    private String pendingCharDir;
    private int pendingCharId;
    private volatile boolean charCopying;  // one copy at a time: a new pick while one runs is refused
    // id = the native side's request number (mods_android.cpp): an answer to an older request is ignored there.
    static native void nativeCharPicked(int id, String path, String error);

    // Session 98 (Superstar Importer): kind 0 = one character file, 1 = several (EXTRA_ALLOW_MULTIPLE; the answer is the copied paths
    // joined by '\n'), 2 = a PNG / JPG picture. Same copy rules for all (size cap, first bytes checked, a safe name).
    private int pendingKind;
    public void jPickCharacter(String destDir, int id) { jPickFiles(destDir, id, 0); }
    public void jPickFiles(String destDir, int id, int kind) {
        runOnUiThread(() -> {
            if (charCopying) {
                nativeCharPicked(id, "", "The last file is still being copied: try again in a moment");
                return;
            }
            pendingCharDir = destDir;
            pendingCharId = id;
            pendingKind = kind;
            try {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType(kind == 2 ? "image/*" : "*/*");
                if (kind == 1) i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
                startActivityForResult(i, REQ_CHAR);
            } catch (Exception e) {
                Log.w(TAG, "viewer: no file picker", e);
                nativeCharPicked(id, "", "This phone has no file picker for that");
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);  // SDL's own file dialogs
        if (requestCode != REQ_CHAR) return;
        final int id = pendingCharId, kind = pendingKind;
        final List<Uri> uris = new ArrayList<>();
        if (resultCode == RESULT_OK && data != null) {
            if (data.getClipData() != null)
                for (int k = 0; k < data.getClipData().getItemCount() && k < 200; k++) uris.add(data.getClipData().getItemAt(k).getUri());
            else if (data.getData() != null) uris.add(data.getData());
        }
        if (uris.isEmpty()) {
            nativeCharPicked(id, "", "");
            return;
        }
        if (charCopying) {
            nativeCharPicked(id, "", "The last file is still being copied: try again in a moment");
            return;
        }
        final String dir = pendingCharDir;
        charCopying = true;
        new Thread(() -> {
            try {
                if (uris.size() == 1 && kind != 1) {
                    copyPicked(uris.get(0), dir, id, kind);
                    return;
                }
                // several: each copied (a refused one is named in the error line, the rest still come back)
                StringBuilder paths = new StringBuilder(), errs = new StringBuilder();
                for (Uri u : uris) {
                    String[] r = copyOne(u, dir, id, kind);
                    if (!r[0].isEmpty()) paths.append(paths.length() > 0 ? "\n" : "").append(r[0]);
                    if (!r[1].isEmpty()) errs.append(errs.length() > 0 ? "; " : "").append(r[1]);
                }
                nativeCharPicked(id, paths.toString(), errs.toString());
            } finally {
                charCopying = false;
            }
        }, "wwe2k14-charpick").start();
    }

    private static boolean isPng(byte[] h) { return (h[0] & 0xFF) == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G'; }
    private static boolean isJpg(byte[] h) { return (h[0] & 0xFF) == 0xFF && (h[1] & 0xFF) == 0xD8 && (h[2] & 0xFF) == 0xFF; }
    private void copyPicked(Uri uri, String dir, int id, int kind) {
        String[] r = copyOne(uri, dir, id, kind);
        nativeCharPicked(id, r[0], r[1]);
    }

    private static boolean isZip(byte[] h) { return h[0] == 'P' && h[1] == 'K' && h[2] == 3 && h[3] == 4; }
    private static boolean isPac(byte[] h) { return h[0] == 'E' && h[1] == 'P' && (h[2] == 'A' && h[3] == 'C' || h[2] == 'K' && h[3] == '8'); }
    // A game pac still in its compressed wrapper: let it through, the native import names the problem (char_mods.h CheckHead).
    private static boolean isGamePac(byte[] h) { return (h[0] & 0xFF) == 0x0F && (h[1] & 0xFF) == 0xF5 && (h[2] & 0xFF) == 0x12 && (h[3] & 0xFF) == 0xED; }

    // The picked file -> <dir>/<its name>, with .pac / .zip from its first bytes when the name has neither. Its size (when the
    // provider says) and its first bytes are checked before anything is copied.
    // Returns {path, error} (one of them empty).
    private int copySeq;
    private String[] copyOne(Uri uri, String dir, int id, int kind) {
        String name = displayName(uri);
        File tmp = new File(dir, "picked_" + id + "_" + (copySeq++) + ".part");
        try {
            long size = providerSize(uri);
            if (size > CHAR_MAX_BYTES) return new String[] {"", name + " is too big (" + (size >> 20) + " MB; a character zip or pac is far smaller)"};
            byte[] head = new byte[4];
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                if (in == null) throw new java.io.IOException("cannot read it");
                int got = 0, n;
                while (got < 4 && (n = in.read(head, got, 4 - got)) > 0) got += n;
                if (kind == 2) {
                    if (got < 4 || !(isPng(head) || isJpg(head))) return new String[] {"", name + " is not a PNG or JPG picture"};
                } else if (got < 4 || !(isZip(head) || isPac(head) || isGamePac(head))) {
                    return new String[] {"", name + " is not a character .pac or a .zip that holds one"};
                }
            }
            if (kind == 2) {
                String lower = name.toLowerCase(Locale.ROOT);
                name = isPng(head) ? (lower.endsWith(".png") ? name : name + ".png") : (lower.endsWith(".jpg") || lower.endsWith(".jpeg") ? name : name + ".jpg");
                name = safeName(name.replace(".pac", "_pac").replace(".zip", "_zip"), "");
            } else name = safeName(name, isZip(head) ? ".zip" : ".pac");
            final String shown = name;
            runOnUiThread(() -> android.widget.Toast.makeText(this, "Copying " + shown + (size > 0 ? " (" + Math.max(1, size >> 20) + " MB)" : "") + "...",
                    android.widget.Toast.LENGTH_SHORT).show());
            new File(dir).mkdirs();
            long total = 0;
            try (InputStream in = getContentResolver().openInputStream(uri);
                 OutputStream out = new FileOutputStream(tmp)) {
                if (in == null) throw new java.io.IOException("cannot read it");
                byte[] buf = new byte[1 << 16];
                int n;
                while ((n = in.read(buf)) > 0) {
                    total += n;
                    if (total > CHAR_MAX_BYTES) throw new java.io.IOException("the file is too big (over 1 GB)");
                    out.write(buf, 0, n);
                }
            }
            File fin = new File(dir, name);
            if (kind == 1)  // (several picked: two files with one name both stay)
                for (int k = 2; fin.exists(); k++) {
                    int dot = name.lastIndexOf('.');
                    fin = new File(dir, (dot > 0 ? name.substring(0, dot) : name) + " (" + k + ")" + (dot > 0 ? name.substring(dot) : ""));
                }
            fin.delete();
            if (!tmp.renameTo(fin)) throw new java.io.IOException("could not finish the copy");
            Log.i(TAG, "viewer: picked file " + fin + " (" + total + " bytes)");
            return new String[] {fin.getAbsolutePath(), ""};
        } catch (Exception e) {
            Log.w(TAG, "viewer: pick copy failed", e);
            tmp.delete();
            return new String[] {"", "Could not read " + name + ": " + e.getMessage()};
        }
    }

    private long providerSize(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getLong(0);
        } catch (Exception ignored) {
        }
        return -1;
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                String s = c.getString(0);
                if (s != null && !s.isEmpty()) return s;
            }
        } catch (Exception ignored) {
        }
        String s = uri.getLastPathSegment();
        return s != null ? s : "character";
    }

    // Only the file's own name: no folders, no control characters, no lone surrogates, no leading dots, ending in `ext` when it has
    // neither .pac nor .zip, at most NAME_MAX_UTF8 bytes of UTF-8 (char_mods.h SafeName cleans it again).
    private static String safeName(String s, String ext) {
        int cut = Math.max(s.lastIndexOf('/'), s.lastIndexOf('\\'));
        if (cut >= 0) s = s.substring(cut + 1);
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            // (surrogates - emoji and other characters past U+FFFF - become '_': JNI hands strings to the native side as modified
            // UTF-8, which writes them as two 3-byte halves that are not valid UTF-8; a pair becomes one '_')
            if (Character.isHighSurrogate(ch) && i + 1 < s.length() && Character.isLowSurrogate(s.charAt(i + 1))) i++;
            boolean bad = ch < 0x20 || ch == 0x7F || Character.isSurrogate(ch) || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|';
            b.append(bad ? '_' : ch);
        }
        String o = b.toString().replaceAll("^[. ]+", "");
        String lower = o.toLowerCase(Locale.ROOT);
        String stem = o;
        if (lower.endsWith(".pac") || lower.endsWith(".zip")) {
            ext = o.substring(o.length() - 4);
            stem = o.substring(0, o.length() - 4);
        }
        while (!stem.isEmpty() && (stem + ext).getBytes(StandardCharsets.UTF_8).length > NAME_MAX_UTF8) stem = stem.substring(0, stem.length() - 1);
        if (stem.trim().isEmpty() || stem.startsWith("picked_")) stem = "character";
        return stem + ext;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // The app's files folder for the native side (platform.h LocalDataDir: logs/viewer.log, the tron stills, saved camera paths).
        try {
            Os.setenv("WWE2K14_FILES_DIR", filesDir().getAbsolutePath(), true);
        } catch (ErrnoException e) {
            Log.e(TAG, "setenv WWE2K14_FILES_DIR failed", e);
        }
        // Android rebuilt this activity after its process was killed in the background: the viewer keeps no state worth a restart
        // with old arguments. Back to the launcher instead (SDL's onCreate never runs).
        if (savedInstanceState != null) {
            Log.i(TAG, "viewer: rebuilt by Android, back to the launcher");
            leave();
            return;
        }
        super.onCreate(savedInstanceState);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setAttributes(lp);
        if (Build.VERSION.SDK_INT >= 33) {
            backCallback = this::askQuit;
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback);
        }
    }

    @Override
    protected void onDestroy() {
        if (Build.VERSION.SDK_INT >= 33 && backCallback != null)
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backCallback);
        super.onDestroy();
        // The viewer's own quit (Esc, the quit command) or a start failure: a fresh process next time.
        if (isFinishing()) android.os.Process.killProcess(android.os.Process.myPid());
    }

    // SDL asks for an orientation from the viewer's window when it is made; the viewer is landscape only.
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        askQuit();
    }

    private void askQuit() {
        if (quitDialog != null && quitDialog.isShowing()) return;
        quitDialog = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Leave the Arena Viewer?")
                .setPositiveButton("Leave", (d, w) -> leave())
                .setNegativeButton("Stay", null)
                .create();
        quitDialog.show();
    }

    // The launcher (below in the same task) comes back; this process ends at once (no native shutdown to wait for).
    private void leave() {
        Log.i(TAG, "viewer: back to the launcher");
        finish();
        overridePendingTransition(0, 0);
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    private File filesDir() {
        File files = getExternalFilesDir(null);
        return files != null ? files : getFilesDir();
    }

    // SDL is linked into libviewer.so (no libSDL3.so); the last library holds SDL_main.
    @Override
    protected String[] getLibraries() {
        return new String[] {"c++_shared", "viewer"};
    }

    // Session 98 stage C review: this activity is exported (adb starts it), so another app could start it with its own extras. The
    // launcher's extras count only with the one-time token the launcher wrote into the app's files folder (no other app can read or
    // write it); the token file is deleted here, so it works once.
    private boolean fromOurLauncher() {
        String given = getIntent().getStringExtra(EXTRA_LAUNCH_TOKEN);
        File f = new File(filesDir(), LAUNCH_TOKEN_FILE);
        String want = null;
        try {
            if (f.isFile()) want = new String(java.nio.file.Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).trim();
        } catch (Exception e) {
            Log.w(TAG, "viewer: cannot read the launch token", e);
        }
        if (f.isFile() && !f.delete()) Log.w(TAG, "viewer: could not delete " + f);
        return given != null && want != null && !want.isEmpty() && want.equals(given);
    }

    // The launcher's arguments (EXTRA_ARGS with its token), with its game-file placeholder replaced by the file this process opens. Else
    // a test run's: <files>/viewer_args.txt, one per line (tools/android/run.ps1 -Viewer writes it), when written in the last 2 minutes;
    // read once and deleted (a launcher start deletes a leftover one too).
    @Override
    protected String[] getArguments() {
        File args = new File(filesDir(), "viewer_args.txt");
        List<String> list = new ArrayList<>();
        final boolean launcher = fromOurLauncher();
        if (launcher || (args.isFile() && System.currentTimeMillis() - args.lastModified() > ARGS_FILE_MAX_AGE_MS)) {
            if (args.isFile()) {
                Log.i(TAG, "viewer: ignoring " + (launcher ? "a leftover " : "a stale ") + args);
                if (!args.delete()) Log.w(TAG, "could not delete " + args);
            }
        } else if (args.isFile()) {
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(args), StandardCharsets.UTF_8))) {
                String line;
                while ((line = r.readLine()) != null) {
                    line = line.trim();
                    if (!line.isEmpty() && !line.startsWith("#")) list.add(line);
                }
            } catch (Exception e) {
                Log.e(TAG, "reading " + args + " failed", e);
            }
            if (!args.delete()) Log.w(TAG, "could not delete " + args);
            Log.i(TAG, "viewer arguments from viewer_args.txt: " + list);
            return list.toArray(new String[0]);
        }
        String[] given = launcher ? getIntent().getStringArrayExtra(EXTRA_ARGS) : null;
        if (given == null) {
            if (getIntent().hasExtra(EXTRA_ARGS)) Log.w(TAG, "viewer: arguments without the launcher's token ignored");
            return new String[0];
        }
        String fd = null;
        for (String a : given) {
            if (a.contains(GAME_FD_TOKEN)) {
                if (fd == null) fd = openGameFile(getIntent().getStringExtra(EXTRA_GAME_URI));
                if (fd == null) continue;  // the viewer then says it has no game files
                a = a.replace(GAME_FD_TOKEN, fd);
            }
            list.add(a);
        }
        Log.i(TAG, "viewer arguments from the launcher: " + list);
        return list.toArray(new String[0]);
    }

    // The picked game file, opened by this process (the app keeps lasting read access to it): /proc/self/fd/N.
    private String openGameFile(String uri) {
        if (uri == null) return null;
        try {
            ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
            if (pfd == null) return null;
            return "/proc/self/fd/" + pfd.detachFd();
        } catch (Exception e) {
            Log.e(TAG, "viewer: cannot open the game file " + uri, e);
            return null;
        }
    }
}
