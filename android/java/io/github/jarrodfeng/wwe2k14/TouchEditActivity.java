// wwe2k14 Android port (Session 74, players asked to move the touch controls): the launcher's MOVE THE CONTROLS screen.
// The touch controls full screen in landscape over a plain background, in TouchOverlay's edit mode; DONE saves
// touch_layout.txt (the game reads it at its start), CANCEL or Back leaves it as it was. Runs in the launcher's process
// (no game library here: the overlay is standalone). The window matches the game's: full screen past the camera
// cut-out, so the controls land where they will in a game.
package io.github.jarrodfeng.wwe2k14;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

public class TouchEditActivity extends Activity {
    // The launcher's Size and Opacity (int[2]), as the game gets them.
    static final String EXTRA_LOOK = "wwe2k14.touch_look";

    private TouchOverlay overlay;
    private OnBackInvokedCallback backCallback;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setAttributes(lp);

        int[] look = getIntent().getIntArrayExtra(EXTRA_LOOK);
        int size = look != null && look.length == 2 ? look[0] : 100, opacity = look != null && look.length == 2 ? look[1] : 70;
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.rgb(0x2A, 0x2D, 0x33));
        overlay = new TouchOverlay(this, TouchOverlay.MODE_ALWAYS, size, opacity, false, true);
        root.addView(overlay, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(root);
        hideSystemBars(root);
        overlay.startEdit(saved -> finish());

        if (Build.VERSION.SDK_INT >= 33) {
            backCallback = this::cancel;
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, backCallback);
        }
    }

    @SuppressWarnings("deprecation")
    private void hideSystemBars(View root) {
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            root.setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    private void cancel() {
        if (overlay != null && overlay.editing()) overlay.cancelEdit();  // its listener finishes
        else finish();
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        cancel();
    }

    @Override
    protected void onDestroy() {
        if (overlay != null) overlay.release();
        if (Build.VERSION.SDK_INT >= 33 && backCallback != null)
            getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backCallback);
        super.onDestroy();
    }
}
