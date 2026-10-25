package com.xcode.km14panel;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/** Launcher activity: requests overlay permission and starts the floating panel. */
public final class MainActivity extends Activity {
    private static final int REQUEST_OVERLAY = 28765;
    private boolean openedSettings;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (hasOverlayPermission()) {
            startFloatingPanel();
        } else {
            showPermissionScreen();
        }
    }

    private boolean hasOverlayPermission() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.M || Settings.canDrawOverlays(this);
    }

    private void showPermissionScreen() {
        int bg = Color.rgb(14, 17, 24);
        int textColor = Color.rgb(238, 242, 250);
        int muted = Color.rgb(157, 169, 188);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setPadding(dp(24), dp(24), dp(24), dp(24));
        root.setBackgroundColor(bg);

        TextView title = new TextView(this);
        title.setText("KM14 Floating Panel");
        title.setTextColor(textColor);
        title.setTextSize(25);
        title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        root.addView(title, new LinearLayout.LayoutParams(-1, -2));

        TextView description = new TextView(this);
        description.setText("Для отображения панели поверх игры разреши «Показывать поверх других приложений». После выдачи разрешения панель откроется автоматически.");
        description.setTextColor(muted);
        description.setTextSize(15);
        description.setLineSpacing(dp(3), 1.0f);
        LinearLayout.LayoutParams descLp = new LinearLayout.LayoutParams(-1, -2);
        descLp.topMargin = dp(14);
        root.addView(description, descLp);

        Button openSettings = new Button(this);
        openSettings.setText("Разрешить оверлей");
        openSettings.setTextColor(Color.WHITE);
        openSettings.setAllCaps(false);
        GradientDrawable buttonBg = new GradientDrawable();
        buttonBg.setColor(Color.rgb(45, 112, 200));
        buttonBg.setCornerRadius(dp(12));
        openSettings.setBackground(buttonBg);
        LinearLayout.LayoutParams btnLp = new LinearLayout.LayoutParams(-1, dp(52));
        btnLp.topMargin = dp(24);
        root.addView(openSettings, btnLp);

        openSettings.setOnClickListener(v -> requestOverlayPermission());
        setContentView(root);
    }

    private void requestOverlayPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            openedSettings = true;
            Intent intent = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivityForResult(intent, REQUEST_OVERLAY);
        } else {
            startFloatingPanel();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_OVERLAY) {
            openedSettings = false;
            if (hasOverlayPermission()) startFloatingPanel();
            else showPermissionScreen();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (openedSettings && hasOverlayPermission()) {
            openedSettings = false;
            startFloatingPanel();
        }
    }

    private void startFloatingPanel() {
        Intent service = new Intent(this, OverlayService.class);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(service);
        else startService(service);
        finish();
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
