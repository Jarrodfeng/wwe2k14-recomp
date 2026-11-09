// wwe2k14 Android port, Session 98 (Jarrod: "building tron previews definitely needs a progress bar, ideally also being able to be
// done as a background process with a notification that lists the progress"): the tron export's foreground service.
//
// Why a service and not a headless export: the export (src/tron_export.h) runs on the game's guest thread inside one VdSwap, after
// the game has booted to guest frame 900 - the boot needs SDL's activity, its window and the Vulkan surface (SDLActivity starts the
// native thread; the guest presents frames until then). Once the export has started it presents nothing: the Bink decode, the
// encoder and the file writes need no surface, so the app can go to the background. What stops it there is Android, not the game:
// a background process with no foreground service is "cached" - frozen by the cached-apps freezer and first in line for the
// low-memory killer. This service, in the game's own process (":game"), keeps that process at foreground-service priority for the
// export, and shows its progress as a notification. It does not run the export itself.
//
// It reads <trons>/progress.json (the game writes it, renamed into place) once a second. The end comes from GameActivity (the game's
// exit request): finish() posts the result as its own notification (not tied to this service, so it stays after the process ends),
// then the process ends. Tapping either notification opens the launcher (LauncherActivity: during the export proper it opens the
// launcher, which shows the progress; during the boot it brings the game forward, which the boot needs).
package io.github.jarrodfeng.wwe2k14;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

import org.json.JSONObject;

public class TronExportService extends Service {
    private static final String TAG = "wwe2k14";
    static final String EXTRA_DIR = "wwe2k14.trons_dir";
    private static final String CHANNEL_PROGRESS = "tron_export", CHANNEL_RESULT = "tron_export_result";
    private static final int ID_PROGRESS = 9801, ID_RESULT = 9802;

    private static TronExportService running;  // (one export at a time; the game's process)
    private Handler handler;
    private String dir;

    /** progress.json, read the forgiving way (missing / caught mid-write = null). */
    static final class Progress {
        String state = "starting", phase = "", item = "", message = "";
        int done, total, ok, itemFrame, itemFrames;
        double mb, elapsed = -1, eta = -1;

        static Progress read(String dir) {
            if (dir == null) return null;
            try {
                byte[] raw = java.nio.file.Files.readAllBytes(new File(dir, "progress.json").toPath());
                JSONObject j = new JSONObject(new String(raw, StandardCharsets.UTF_8));
                Progress p = new Progress();
                p.state = j.optString("state", "starting");
                p.phase = j.optString("phase", "");
                p.item = j.optString("item", "");
                p.message = j.optString("message", "");
                p.done = j.optInt("done", 0);
                p.total = j.optInt("total", 0);
                p.ok = j.optInt("ok", 0);
                p.itemFrame = j.optInt("item_frame", 0);
                p.itemFrames = j.optInt("item_frames", 0);
                p.mb = j.optDouble("mb", 0);
                p.elapsed = j.optDouble("elapsed_s", -1);
                p.eta = j.optDouble("eta_s", -1);
                return p;
            } catch (Exception e) {
                return null;
            }
        }

        boolean exporting() { return state.equals("running"); }
        boolean finished() { return state.equals("done") || state.equals("failed"); }

        /** 0..1000 over the whole run: finished items plus the share of the current one. */
        int permille() {
            if (total <= 0) return 0;
            double part = itemFrames > 0 ? Math.min(1.0, (double) itemFrame / itemFrames) : 0;
            return (int) Math.min(1000, Math.round((done + (done < total ? part : 0)) * 1000.0 / total));
        }

        String counts() { return Math.min(done + 1, total) + " / " + total; }

        static String duration(double s) {
            if (s < 0) return "";
            long m = Math.round(s / 60.0);
            return m < 1 ? "under a minute" : m == 1 ? "about 1 minute" : "about " + m + " minutes";
        }

        String line() {
            if (!exporting()) return "Starting the game";
            String what = phase.isEmpty() ? item : phase + ": " + item;
            return counts() + "  " + what;
        }

        String detail() {
            StringBuilder b = new StringBuilder(String.format(Locale.US, "%.0f MB written", mb));
            if (eta >= 0) b.append(", ").append(duration(eta)).append(" left");
            return b.toString();
        }
    }

    // ------------------------------------------------------------------ from GameActivity

    /** Starts the service (the game's activity is in front, so Android allows it). */
    static void start(Context c, String trons) {
        try {
            Intent i = new Intent(c, TronExportService.class).putExtra(EXTRA_DIR, trons);
            if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i);
            else c.startService(i);
        } catch (Exception e) {
            Log.w(TAG, "tron export: the foreground service could not start", e);
        }
    }

    /** The run is over (the game's exit request, or the player quit): the result notification, then the service stops. */
    static void finish(Context c, String trons, String quitReason) {
        Progress p = Progress.read(trons);
        String title, text;
        int icon;
        if (quitReason != null) {
            title = "Tron previews stopped";
            text = quitReason + (p != null && p.total > 0 ? " (" + p.done + " of " + p.total + " done). Build them again to finish." : ".");
            icon = android.R.drawable.stat_notify_error;
        } else if (p != null && p.state.equals("done")) {
            title = "Tron previews ready";
            text = p.ok + " of " + p.total + " videos and pictures made" + (p.elapsed > 0 ? " in " + Progress.duration(p.elapsed).replace("about ", "") : "")
                    + String.format(Locale.US, ", %.0f MB", p.mb);
            icon = android.R.drawable.stat_sys_download_done;
        } else if (p != null && p.state.equals("failed")) {
            title = "Tron previews failed";
            text = "None could be made (" + p.message + "). Open the launcher and use SHARE for the game's log.";
            icon = android.R.drawable.stat_notify_error;
        } else {
            title = "Tron previews stopped";
            text = "The game ended before the export finished" + (p != null && p.total > 0 ? " (" + p.done + " of " + p.total + " done)" : "")
                    + ". Build them again to finish.";
            icon = android.R.drawable.stat_notify_error;
        }
        try {
            NotificationManager nm = c.getSystemService(NotificationManager.class);
            if (nm != null) {
                makeChannels(nm);
                Notification n = builder(c, CHANNEL_RESULT).setSmallIcon(icon).setContentTitle(title).setContentText(text)
                        .setStyle(new Notification.BigTextStyle().bigText(text)).setAutoCancel(true).build();
                nm.notify(ID_RESULT, n);
            }
        } catch (Exception e) {
            Log.w(TAG, "tron export: result notification failed", e);
        }
        Log.i(TAG, "tron export: " + title + ": " + text);
        TronExportService s = running;
        if (s != null) s.stopNow();
    }

    // ------------------------------------------------------------------ the service

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && intent.getStringExtra(EXTRA_DIR) != null) dir = intent.getStringExtra(EXTRA_DIR);
        running = this;
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) makeChannels(nm);
        Notification n = progressNotification(Progress.read(dir));
        // Review: with the type first; if Android refuses the typed start, once more without a type. Never stopSelf before a
        // startForeground succeeded (stopping a startForegroundService service that never went to the foreground is itself an error
        // on some Android versions); when both fail the service only keeps polling, and the export runs while the game is in front.
        boolean foreground = false;
        try {
            if (Build.VERSION.SDK_INT >= 35)
                startForeground(ID_PROGRESS, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROCESSING);
            else if (Build.VERSION.SDK_INT >= 29)
                startForeground(ID_PROGRESS, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
            else
                startForeground(ID_PROGRESS, n);
            foreground = true;
        } catch (Exception e) {
            Log.w(TAG, "tron export: typed startForeground refused, trying without a type", e);
        }
        if (!foreground) {
            try {
                startForeground(ID_PROGRESS, n);
            } catch (Exception e) {
                Log.w(TAG, "tron export: startForeground refused", e);
            }
        }
        if (handler == null) {
            handler = new Handler(Looper.getMainLooper());
            handler.postDelayed(this::poll, 1000);
        }
        return START_NOT_STICKY;  // (a killed game process ends the export: never restart the service on its own)
    }

    // Review (MUST): a foreground-service process outlives its task. With GameActivity gone (the app swiped away from recents) nothing
    // would end this process after the export, and the launcher would see a game alive forever. So the service ends the run itself
    // when the export is over (progress done / failed, or the game's exit request) and GameActivity has not done it within ~3 s
    // (its own poll runs every 400 ms and normally wins, bringing the launcher back).
    private long overSince;

    private boolean exitRequested() {
        File files = getExternalFilesDir(null);
        if (files == null) files = getFilesDir();
        return new File(new File(files, "logs"), "exit_request.txt").isFile();
    }

    private void endProcess(String quitReason) {
        Log.i(TAG, "tron export: the service ends the game's process (" + (quitReason != null ? quitReason : "export over") + ")");
        finish(this, dir, quitReason);
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    // Review: swiped away from recents. During the boot the export cannot go on without the game's screen: end it (the result
    // notification says so). Past the boot it keeps going; poll() ends the process when it is done.
    @Override
    public void onTaskRemoved(Intent rootIntent) {
        Progress p = Progress.read(dir);
        if (p == null || !(p.exporting() || p.finished())) endProcess("The app was closed before the export started");
        else Log.i(TAG, "tron export: the app's task was removed; the export keeps going");
    }

    private void poll() {
        Progress p = Progress.read(dir);
        if ((p != null && p.finished()) || exitRequested()) {
            long now = System.currentTimeMillis();
            if (overSince == 0) overSince = now;
            else if (now - overSince > 3000) {
                endProcess(null);
                return;
            }
        }
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) {
            try {
                nm.notify(ID_PROGRESS, progressNotification(p));
            } catch (Exception e) {
                Log.w(TAG, "tron export: notification update failed", e);
            }
        }
        handler.postDelayed(this::poll, 1000);
    }

    // Android 15+: the foreground-service time limit for dataSync / mediaProcessing (6 h a day) ran out. The export never takes that
    // long; stop cleanly rather than let Android end the process with an error.
    @Override
    public void onTimeout(int startId, int fgsType) {
        Log.w(TAG, "tron export: Android's foreground-service time limit reached");
        stopNow();
    }

    private void stopNow() {
        if (handler != null) handler.removeCallbacksAndMessages(null);
        running = null;
        if (Build.VERSION.SDK_INT >= 33) stopForeground(STOP_FOREGROUND_REMOVE);
        else stopForeground(true);
        stopSelf();
    }

    @Override
    public void onDestroy() {
        if (handler != null) handler.removeCallbacksAndMessages(null);
        if (running == this) running = null;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    private Notification progressNotification(Progress p) {
        Notification.Builder b = builder(this, CHANNEL_PROGRESS).setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle("Building tron previews").setOngoing(true).setOnlyAlertOnce(true);
        if (p != null && p.finished()) {
            b.setContentText("Finishing: " + p.message).setProgress(1000, 1000, false);
        } else if (p == null || !p.exporting()) {
            b.setContentText("Starting the game. Keep it on screen until the export starts.").setProgress(0, 0, true);
        } else {
            b.setContentText(p.line()).setSubText(p.detail()).setProgress(1000, p.permille(), false)
                    .setStyle(new Notification.BigTextStyle().bigText(p.line() + "\n" + p.detail() + "\nYou can leave the app; this keeps going."));
        }
        if (Build.VERSION.SDK_INT >= 31) b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
        return b.build();
    }

    private static Notification.Builder builder(Context c, String channel) {
        Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(c, channel) : new Notification.Builder(c);
        Intent open = new Intent(c, LauncherActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        b.setContentIntent(PendingIntent.getActivity(c, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT));
        return b;
    }

    private static void makeChannels(NotificationManager nm) {
        if (Build.VERSION.SDK_INT < 26) return;
        if (nm.getNotificationChannel(CHANNEL_PROGRESS) == null) {
            NotificationChannel ch = new NotificationChannel(CHANNEL_PROGRESS, "Tron previews: progress", NotificationManager.IMPORTANCE_LOW);
            ch.setDescription("While BUILD TRON PREVIEWS runs");
            ch.setShowBadge(false);
            nm.createNotificationChannel(ch);
        }
        if (nm.getNotificationChannel(CHANNEL_RESULT) == null) {
            NotificationChannel ch = new NotificationChannel(CHANNEL_RESULT, "Tron previews: finished", NotificationManager.IMPORTANCE_DEFAULT);
            ch.setDescription("When BUILD TRON PREVIEWS ends");
            nm.createNotificationChannel(ch);
        }
    }
}
