// wwe2k14 Android port (Session 101, lane 5; docs\systems\case_zip.md): the bug button. A small corner button over the game: one tap writes a
// case zip (the last seconds of frames, the picture, where the game was, timing, settings, log) into the app's files folder cases\ and, when the
// player typed a WebDAV address in the dialog below, uploads it. Long-press opens the dialog: WebDAV address / user / password (typed here by
// the player, kept in this app's private preferences, never in code or in the repo; https only) and "hide the button" (it then fades out; three
// quick taps still write one case). Off by default (S101 lane 11): GameActivity attaches the button only when the launcher passed
// --wwe2k14_case=true, which is also what turns the native ring on. The native side (src\case\case_jni.h) takes the tap on the game's own frame thread, so the game never waits.
package io.github.jarrodfeng.wwe2k14;

import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.Log;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;

final class CaseButton {
    private static final String TAG = "wwe2k14.case";
    static final String PREFS = "case_upload";  // url, user, password (typed by the player), hidden

    // Native (src\case\case_jni.h in the game's library): asks the frame thread for a case zip.
    private static native void nativeCaseTap();

    private final Context context;
    private final File cases;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private TextView view;
    private long lastTap;
    private int taps;

    private CaseButton(Context c, File filesDir) {
        context = c;
        cases = new File(filesDir, "cases");
    }

    /** Adds the button to the game's layout. Call once from GameActivity.onCreate after the layout exists. */
    static void attach(Context c, ViewGroup layout, File filesDir) {
        CaseButton b = new CaseButton(c, filesDir);
        b.build(layout);
        CaseUpload.startPending(c, b.cases);  // zips an earlier run left behind
    }

    private SharedPreferences prefs() {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    private void build(ViewGroup layout) {
        float d = context.getResources().getDisplayMetrics().density;
        view = new TextView(context);
        view.setText("!");
        view.setTextColor(Color.WHITE);
        view.setTextSize(16);
        view.setGravity(Gravity.CENTER);
        GradientDrawable bg = new GradientDrawable();
        bg.setShape(GradientDrawable.OVAL);
        bg.setColor(0x99000000);
        bg.setStroke((int) (1.5f * d), 0xCCFFFFFF);
        view.setBackground(bg);
        int size = (int) (40 * d);
        // (SDL's layout is a RelativeLayout: top-left corner)
        android.widget.RelativeLayout.LayoutParams lp = new android.widget.RelativeLayout.LayoutParams(size, size);
        lp.addRule(android.widget.RelativeLayout.ALIGN_PARENT_TOP);
        lp.addRule(android.widget.RelativeLayout.ALIGN_PARENT_LEFT);
        lp.setMargins((int) (8 * d), (int) (8 * d), 0, 0);
        view.setLayoutParams(lp);
        applyHidden();
        view.setOnClickListener(v -> onTap());
        view.setOnLongClickListener(v -> {
            showDialog();
            return true;
        });
        layout.addView(view);
    }

    private boolean hidden() {
        return prefs().getBoolean("hidden", false);
    }

    private void applyHidden() {
        view.setAlpha(hidden() ? 0.04f : 1f);  // still there: three quick taps write a case
    }

    private void onTap() {
        long now = android.os.SystemClock.uptimeMillis();
        if (hidden()) {  // hidden: only three quick taps count, and they write ONE case (the count starts again)
            taps = (now - lastTap > 600) ? 1 : taps + 1;
            lastTap = now;
            if (taps < 3) return;
            taps = 0;
        }
        lastTap = now;
        try {
            nativeCaseTap();
            Toast.makeText(context, "Writing a case zip...", Toast.LENGTH_SHORT).show();
            CaseUpload.watch(context, cases, ui);  // uploads it as soon as it appears
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "the native case tap is not in this build", e);
            Toast.makeText(context, "This build has no case zip", Toast.LENGTH_SHORT).show();
        }
    }

    private void showDialog() {
        float d = context.getResources().getDisplayMetrics().density;
        SharedPreferences p = prefs();
        LinearLayout box = new LinearLayout(context);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding((int) (20 * d), (int) (8 * d), (int) (20 * d), 0);
        TextView note = new TextView(context);
        note.setText("A case zip is written when you tap the corner button. Zips stay in the app's files folder (cases). To upload them, "
                + "type your WebDAV address, user and password; they are kept only on this phone.");
        box.addView(note);
        EditText url = field("https://.../folder", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI, p.getString("url", ""));
        EditText user = field("user", InputType.TYPE_CLASS_TEXT, p.getString("user", ""));
        EditText pass = field("password", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD, p.getString("password", ""));
        box.addView(url);
        box.addView(user);
        box.addView(pass);
        CheckBox hide = new CheckBox(context);
        hide.setText("Hide the button (three quick taps still write a case)");
        hide.setChecked(hidden());
        box.addView(hide);
        AlertDialog dialog = new AlertDialog.Builder(context)
                .setTitle("Report button")
                .setView(box)
                .setPositiveButton("Save", null)  // (the click is set below: a bad address keeps the dialog open)
                .setNegativeButton("Cancel", null)
                .create();
        dialog.show();
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            String address = url.getText().toString().trim();
            // S101 lane 11: the password goes over Basic auth: https only (an empty address = no upload, zips stay on the phone).
            if (!address.isEmpty() && !address.toLowerCase(java.util.Locale.ROOT).startsWith("https://")) {
                Toast.makeText(context, "The upload address must start with https://", Toast.LENGTH_LONG).show();
                return;
            }
            p.edit().putString("url", address).putString("user", user.getText().toString())
                    .putString("password", pass.getText().toString()).putBoolean("hidden", hide.isChecked()).apply();
            applyHidden();
            CaseUpload.startPending(context, cases);
            dialog.dismiss();
        });
    }

    private EditText field(String hint, int type, String value) {
        EditText e = new EditText(context);
        e.setHint(hint);
        e.setInputType(type);
        e.setSingleLine(true);
        e.setText(value);
        return e;
    }
}
