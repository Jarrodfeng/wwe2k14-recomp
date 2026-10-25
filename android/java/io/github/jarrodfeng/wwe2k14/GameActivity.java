// wwe2k14 Android port (Session 63, ANDROID_PLAN.md 4.2): the game's activity. SDL's activity (org.libsdl.app, zlib
// licence, vendored in the SDK) owns the window, input and sound; this class only names our libraries, the app's
// files folder and the run's arguments.
package io.github.jarrodfeng.wwe2k14;

import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.WindowManager;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

import org.libsdl.app.SDLActivity;

public class GameActivity extends SDLActivity {
    private static final String TAG = "wwe2k14";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // The app's own folder (Android/data/<package>/files): the native side makes it the working folder and HOME
        // (logs, user data, files the PC build keeps next to its exe). Set before SDL starts the native thread.
        File files = getExternalFilesDir(null);
        if (files == null) files = getFilesDir();
        try {
            Os.setenv("WWE2K14_FILES_DIR", files.getAbsolutePath(), true);
        } catch (ErrnoException e) {
            Log.e(TAG, "setenv WWE2K14_FILES_DIR failed", e);
        }
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    // SDL lives inside librexruntime.so (no libSDL3.so); libmain.so (last) holds SDL_main.
    @Override
    protected String[] getLibraries() {
        return new String[] {"c++_shared", "rexruntime", "main"};
    }

    // A test run's arguments: <files>/args.txt, one argument per line (tools/android/run.ps1 writes it). Read once and
    // deleted, so a normal start afterwards does not reuse test arguments. Empty lines and lines starting with # skip.
    @Override
    protected String[] getArguments() {
        File files = getExternalFilesDir(null);
        File args = files != null ? new File(files, "args.txt") : null;
        List<String> list = new ArrayList<>();
        if (args != null && args.isFile()) {
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
            Log.i(TAG, "arguments from args.txt: " + list);
        }
        return list.toArray(new String[0]);
    }
}
