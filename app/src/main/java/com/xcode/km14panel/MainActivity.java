package com.xcode.km14panel;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

public final class MainActivity extends Activity {
    private static final String BASE_URL = "http://127.0.0.1:28765";
    private static final int BG = Color.rgb(14, 17, 24);
    private static final int PANEL = Color.rgb(25, 30, 41);
    private static final int TEXT = Color.rgb(238, 242, 250);
    private static final int MUTED = Color.rgb(157, 169, 188);
    private static final int ACCENT = Color.rgb(96, 171, 255);

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final ScheduledExecutorService io = Executors.newSingleThreadScheduledExecutor();
    private ScheduledFuture<?> pendingPush;
    private CheckBox aimEnabled;
    private CheckBox radarEnabled;
    private SeekBar speedSeek;
    private SeekBar magnetSeek;
    private SeekBar fovSeek;
    private TextView speedValue;
    private TextView magnetValue;
    private TextView fovValue;
    private TextView status;
    private boolean applyingRemote;
    private boolean dragging;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);
        getWindow().getDecorView().setSystemUiVisibility(0);

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(BG);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(18), dp(18), dp(18), dp(24));
        scroll.addView(root, new ScrollView.LayoutParams(-1, -2));

        TextView title = text("KM 1.4 Control", 26, true, TEXT);
        root.addView(title, margin(0, 4, 0, 4));
        TextView subtitle = text("Runtime settings · localhost:28765", 13, false, MUTED);
        root.addView(subtitle, margin(0, 0, 0, 14));

        status = text("Проверка соединения с нативкой…", 13, true, MUTED);
        status.setPadding(dp(12), dp(12), dp(12), dp(12));
        status.setBackground(rounded(PANEL, 12));
        root.addView(status, margin(0, 0, 0, 16));

        LinearLayout toggles = card(root);
        TextView togglesTitle = text("Модули", 17, true, TEXT);
        toggles.addView(togglesTitle, margin(0, 0, 0, 8));
        aimEnabled = new CheckBox(this);
        aimEnabled.setText("Aim Assist");
        aimEnabled.setTextColor(TEXT);
        aimEnabled.setTextSize(16);
        aimEnabled.setButtonTintList(android.content.res.ColorStateList.valueOf(ACCENT));
        toggles.addView(aimEnabled, margin(0, 0, 0, 4));
        radarEnabled = new CheckBox(this);
        radarEnabled.setText("Radar");
        radarEnabled.setTextColor(TEXT);
        radarEnabled.setTextSize(16);
        radarEnabled.setButtonTintList(android.content.res.ColorStateList.valueOf(ACCENT));
        toggles.addView(radarEnabled, margin(0, 0, 0, 0));

        LinearLayout tuning = card(root);
        tuning.addView(text("Настройки наведения", 17, true, TEXT), margin(0, 0, 0, 12));
        speedValue = addSlider(tuning, "Скорость", "9.11", 2990, 901, value -> {
            float v = 0.10f + value * 0.01f;
            speedValue.setText(String.format(java.util.Locale.US, "%.2f", v));
        });
        speedSeek = (SeekBar) speedValue.getTag();
        magnetValue = addSlider(tuning, "Магнит", "0.70", 100, 70, value -> {
            float v = value / 100.0f;
            magnetValue.setText(String.format(java.util.Locale.US, "%.2f", v));
        });
        magnetSeek = (SeekBar) magnetValue.getTag();
        fovValue = addSlider(tuning, "FOV (градусы)", "11.0°", 440, 100, value -> {
            float v = 1.0f + value * 0.1f;
            fovValue.setText(String.format(java.util.Locale.US, "%.1f°", v));
        });
        fovSeek = (SeekBar) fovValue.getTag();

        TextView note = text("Отдача отключена. Наведение учитывает только трассу, где движок сообщает, что первым попал именно целевой игрок. Если проверка трассы не подтверждена, цель пропускается.", 12, false, MUTED);
        note.setLineSpacing(dp(2), 1.0f);
        root.addView(note, margin(dp(2), 2, dp(2), 12));
        TextView portNote = text("Панель подключается к 127.0.0.1 внутри того же устройства. Нативная библиотека должна уже быть загружена в процесс игры.", 12, false, MUTED);
        portNote.setLineSpacing(dp(2), 1.0f);
        root.addView(portNote, margin(dp(2), 0, dp(2), 12));

        aimEnabled.setOnCheckedChangeListener((button, checked) -> {
            if (!applyingRemote) schedulePush(0);
        });
        radarEnabled.setOnCheckedChangeListener((button, checked) -> {
            if (!applyingRemote) schedulePush(0);
        });
        setContentView(scroll);
        io.scheduleAtFixedRate(this::pollState, 0, 1500, TimeUnit.MILLISECONDS);
    }

    private interface SliderFormatter { void format(int progress); }

    private TextView addSlider(LinearLayout parent, String label, String initial, int max, int progress,
                               SliderFormatter formatter) {
        LinearLayout heading = new LinearLayout(this);
        heading.setOrientation(LinearLayout.HORIZONTAL);
        heading.setGravity(Gravity.CENTER_VERTICAL);
        TextView name = text(label, 14, false, MUTED);
        TextView value = text(initial, 15, true, TEXT);
        heading.addView(name, new LinearLayout.LayoutParams(0, -2, 1));
        heading.addView(value, new LinearLayout.LayoutParams(-2, -2));
        parent.addView(heading, margin(0, 0, 0, 0));
        SeekBar seek = new SeekBar(this);
        seek.setMax(max);
        seek.setProgress(progress);
        seek.setProgressTintList(android.content.res.ColorStateList.valueOf(ACCENT));
        seek.setThumbTintList(android.content.res.ColorStateList.valueOf(ACCENT));
        seek.setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(Color.rgb(61, 70, 86)));
        parent.addView(seek, margin(0, 0, 0, 14));
        value.setTag(seek);
        seek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int p, boolean fromUser) {
                formatter.format(p);
                if (fromUser && !applyingRemote) schedulePush(120);
            }
            @Override public void onStartTrackingTouch(SeekBar bar) { dragging = true; }
            @Override public void onStopTrackingTouch(SeekBar bar) { dragging = false; schedulePush(0); }
        });
        return value;
    }

    private LinearLayout card(LinearLayout root) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(14), dp(14), dp(14), dp(12));
        box.setBackground(rounded(PANEL, 14));
        root.addView(box, margin(0, 0, 0, 14));
        return box;
    }

    private TextView text(String value, int size, boolean bold, int color) {
        TextView tv = new TextView(this);
        tv.setText(value);
        tv.setTextSize(size);
        tv.setTextColor(color);
        if (bold) tv.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return tv;
    }

    private GradientDrawable rounded(int color, int radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(radius));
        return d;
    }

    private LinearLayout.LayoutParams margin(int left, int top, int right, int bottom) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-1, -2);
        p.setMargins(left, top, right, bottom);
        return p;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private JSONObject configFromUi() {
        JSONObject json = new JSONObject();
        try {
            json.put("aim_enabled", aimEnabled.isChecked());
            json.put("radar_enabled", radarEnabled.isChecked());
            json.put("speed", 0.10f + speedSeek.getProgress() * 0.01f);
            json.put("magnet", magnetSeek.getProgress() / 100.0f);
            json.put("fov", 1.0f + fovSeek.getProgress() * 0.1f);
        } catch (Exception ignored) { }
        return json;
    }

    private void schedulePush(long delayMs) {
        if (applyingRemote || aimEnabled == null) return;
        final JSONObject snapshot = configFromUi();
        synchronized (this) {
            if (pendingPush != null) pendingPush.cancel(false);
            pendingPush = io.schedule(() -> postConfig(snapshot), delayMs, TimeUnit.MILLISECONDS);
        }
    }

    private void postConfig(JSONObject body) {
        try {
            JSONObject response = request("POST", "/config", body);
            if (response != null && response.optBoolean("ok", false)) {
                mainHandler.post(() -> setStatus("Подключено · изменения применены", true));
            } else {
                mainHandler.post(() -> setStatus("Нет ответа от нативки · порт 28765", false));
            }
        } catch (Exception ex) {
            mainHandler.post(() -> setStatus("Нативка не подключена · загрузите .so в игру", false));
        }
    }

    private void pollState() {
        try {
            JSONObject state = request("GET", "/state", null);
            if (state == null || !state.optBoolean("ok", false)) throw new IllegalStateException("bad state");
            mainHandler.post(() -> applyState(state));
        } catch (Exception ex) {
            mainHandler.post(() -> setStatus("Нет соединения · загрузите нативку в процесс игры", false));
        }
    }

    private void applyState(JSONObject s) {
        setStatus("Подключено · порт " + s.optInt("port", 28765), true);
        if (dragging || aimEnabled == null) return;
        applyingRemote = true;
        aimEnabled.setChecked(s.optBoolean("aim_enabled", true));
        radarEnabled.setChecked(s.optBoolean("radar_enabled", true));
        float speed = (float) s.optDouble("speed", 9.11);
        float magnet = (float) s.optDouble("magnet", 0.70);
        float fov = (float) s.optDouble("fov", 11.0);
        speedSeek.setProgress(clamp(Math.round((speed - 0.10f) * 100.0f), 0, 2990));
        magnetSeek.setProgress(clamp(Math.round(magnet * 100.0f), 0, 100));
        fovSeek.setProgress(clamp(Math.round((fov - 1.0f) * 10.0f), 0, 440));
        speedValue.setText(String.format(java.util.Locale.US, "%.2f", 0.10f + speedSeek.getProgress() * 0.01f));
        magnetValue.setText(String.format(java.util.Locale.US, "%.2f", magnetSeek.getProgress() / 100.0f));
        fovValue.setText(String.format(java.util.Locale.US, "%.1f°", 1.0f + fovSeek.getProgress() * 0.1f));
        applyingRemote = false;
    }

    private int clamp(int v, int min, int max) { return Math.max(min, Math.min(max, v)); }

    private void setStatus(String value, boolean connected) {
        if (status == null) return;
        status.setText(value);
        status.setTextColor(connected ? Color.rgb(112, 220, 160) : Color.rgb(255, 172, 116));
    }

    private JSONObject request(String method, String path, JSONObject body) throws Exception {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(BASE_URL + path).openConnection();
            conn.setRequestMethod(method);
            conn.setConnectTimeout(800);
            conn.setReadTimeout(1000);
            conn.setUseCaches(false);
            conn.setRequestProperty("Accept", "application/json");
            if (body != null) {
                byte[] bytes = body.toString().getBytes(StandardCharsets.UTF_8);
                conn.setDoOutput(true);
                conn.setRequestProperty("Content-Type", "application/json; charset=utf-8");
                conn.setFixedLengthStreamingMode(bytes.length);
                try (OutputStream out = conn.getOutputStream()) { out.write(bytes); }
            }
            int code = conn.getResponseCode();
            InputStream stream = (code >= 200 && code < 400) ? conn.getInputStream() : conn.getErrorStream();
            if (stream == null) return null;
            StringBuilder result = new StringBuilder();
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(stream, StandardCharsets.UTF_8))) {
                String line;
                while ((line = reader.readLine()) != null) result.append(line);
            }
            return new JSONObject(result.toString());
        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    @Override protected void onDestroy() {
        synchronized (this) {
            if (pendingPush != null) pendingPush.cancel(false);
        }
        io.shutdownNow();
        super.onDestroy();
    }
}
