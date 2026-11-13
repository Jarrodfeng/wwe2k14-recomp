// wwe2k14 Android port (Session 101, lane 5): uploads case zips from the app's files folder cases\ to the WebDAV address the player typed into
// the report-button dialog (CaseButton). One HTTP PUT per zip with Basic auth, streamed (a zip can be hundreds of MB); a zip that went up is
// marked with <name>.uploaded beside it and never sent twice. Nothing is uploaded until the player has typed an address; the credentials live in
// this app's private preferences only. The fallback when there is no address: adb pull of cases\ (tools\android\pull_cases.ps1).
package io.github.jarrodfeng.wwe2k14;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Handler;
import android.util.Base64;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;

final class CaseUpload {
    private static final String TAG = "wwe2k14.case";
    private static final AtomicBoolean running = new AtomicBoolean(false);

    private CaseUpload() {}

    /** Uploads every finished zip that has not gone up yet (a worker thread; returns at once). */
    static void startPending(Context c, File cases) {
        SharedPreferences p = c.getSharedPreferences(CaseButton.PREFS, Context.MODE_PRIVATE);
        final String url = p.getString("url", "").trim(), user = p.getString("user", ""), pass = p.getString("password", "");
        // S101 lane 11: https only (the dialog refuses anything else; a stale preference is refused here too: Basic auth over plain http leaks the password)
        if (url.isEmpty() || !url.toLowerCase(java.util.Locale.ROOT).startsWith("https://") || !running.compareAndSet(false, true)) return;
        new Thread(() -> {
            try {
                // A zip written while an upload is running would be missed by one pass: loop until a pass uploads nothing. A zip that failed is
                // not retried in this run (the next start tries again), so a dead server cannot spin this loop.
                java.util.Set<String> failed = new java.util.HashSet<>();
                boolean any;
                do {
                    any = false;
                    File[] zips = cases.listFiles((dir, name) -> name.endsWith(".zip"));
                    if (zips == null) return;
                    java.util.Arrays.sort(zips);
                    for (File z : zips) {
                        File marker = new File(z.getPath() + ".uploaded");
                        if (marker.exists() || failed.contains(z.getName())) continue;
                        try {
                            put(url, user, pass, z);
                            marker.createNewFile();
                            any = true;
                            Log.i(TAG, "uploaded " + z.getName() + " (" + z.length() + " bytes)");
                        } catch (Exception e) {
                            failed.add(z.getName());
                            Log.w(TAG, "upload of " + z.getName() + " failed: " + e);  // (never the password; the next start tries again)
                        }
                    }
                } while (any);
            } finally {
                running.set(false);
            }
        }, "case-upload").start();
    }

    /** After a tap: polls for the new zip for two minutes, then uploads. */
    static void watch(Context c, File cases, Handler ui) {
        final long until = android.os.SystemClock.uptimeMillis() + 120_000;
        final long before = newest(cases);
        Runnable poll = new Runnable() {
            @Override
            public void run() {
                if (newest(cases) > before) {
                    startPending(c, cases);
                } else if (android.os.SystemClock.uptimeMillis() < until) {
                    ui.postDelayed(this, 2000);
                }
            }
        };
        ui.postDelayed(poll, 2000);
    }

    private static long newest(File cases) {
        File[] zips = cases.listFiles((dir, name) -> name.endsWith(".zip"));  // (a zip is renamed from .part when finished)
        long t = 0;
        if (zips != null)
            for (File z : zips) t = Math.max(t, z.lastModified());
        return t;
    }

    private static void put(String base, String user, String pass, File zip) throws Exception {
        String target = base.endsWith("/") ? base + zip.getName() : base + "/" + zip.getName();
        HttpURLConnection h = (HttpURLConnection) new URL(target).openConnection();
        h.setRequestMethod("PUT");
        h.setDoOutput(true);
        h.setConnectTimeout(15_000);
        h.setReadTimeout(60_000);
        h.setFixedLengthStreamingMode(zip.length());
        if (!user.isEmpty())
            h.setRequestProperty("Authorization", "Basic " + Base64.encodeToString((user + ":" + pass).getBytes(StandardCharsets.UTF_8), Base64.NO_WRAP));
        try (InputStream in = new FileInputStream(zip); OutputStream out = h.getOutputStream()) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
        int code = h.getResponseCode();
        h.disconnect();
        if (code < 200 || code >= 300) throw new java.io.IOException("HTTP " + code);
    }
}
