package com.xcode.km14panel;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.provider.Settings;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

/** Movable, collapsible panel overlay. Control API is loopback-only. */
public final class OverlayService extends Service {
    private static final String BASE_URL = "http://127.0.0.1:28765";
    private static final String CHANNEL_ID = "km14_floating_panel";
    private static final int NOTIFICATION_ID = 28765;

    private static final int BG = Color.rgb(14, 17, 24);
    private static final int PANEL = Color.rgb(25, 30, 41);
    private static final int PANEL_ALT = Color.rgb(32, 39, 52);
    private static final int TEXT = Color.rgb(238, 242, 250);
    private static final int MUTED = Color.rgb(157, 169, 188);
    private static final int ACCENT = Color.rgb(79, 157, 255);
    private static final int GREEN = Color.rgb(105, 225, 160);
    private static final int ORANGE = Color.rgb(255, 178, 111);

    private final Handler main = new Handler(Looper.getMainLooper());
    private final ScheduledExecutorService io = Executors.newSingleThreadScheduledExecutor();
    private ScheduledFuture<?> pendingPush;
    private ScheduledFuture<?> pollTask;
    private WindowManager wm;
    private WindowManager.LayoutParams params;
    private View activeWindow;
    private View expandedWindow;
    private View collapsedWindow;
    private boolean collapsed;
    private boolean attached;
    private boolean applyingRemote;
    private boolean adjusting;
    private long lastLocalEdit;

    private Switch aimSwitch;
    private Switch radarSwitch;
    private SeekBar speedSeek;
    private SeekBar magnetSeek;
    private SeekBar fovSeek;
    private TextView speedValue;
    private TextView magnetValue;
    private TextView fovValue;
    private TextView statusText;

    @Override
    public void onCreate() {
        super.onCreate();
        wm = (WindowManager) getSystemService(WINDOW_SERVICE);
        ensureNotificationChannel();
        startForeground(NOTIFICATION_ID, buildNotification());
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            stopSelf();
            return;
        }
        buildWindows();
        showExpanded();
        pollTask = io.scheduleAtFixedRate(this::pollState, 0, 1400, TimeUnit.MILLISECONDS);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (collapsed || (!attached && expandedWindow != null)) showExpanded();
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    private void ensureNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(CHANNEL_ID, "KM14 floating panel",
                    NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("Keeps the floating control panel available");
            NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
            nm.createNotificationChannel(channel);
        }
    }

    private Notification buildNotification() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            return new Notification.Builder(this, CHANNEL_ID)
                    .setSmallIcon(android.R.drawable.ic_menu_manage)
                    .setContentTitle("KM14 Floating Panel")
                    .setContentText("Плавающая панель работает")
                    .setOngoing(true)
                    .build();
        }
        return new Notification.Builder(this)
                .setSmallIcon(android.R.drawable.ic_menu_manage)
                .setContentTitle("KM14 Floating Panel")
                .setContentText("Плавающая панель работает")
                .setOngoing(true)
                .build();
    }

    private void buildWindows() {
        expandedWindow = buildExpanded();
        collapsedWindow = buildCollapsed();
        params = new WindowManager.LayoutParams(
                panelWidth(), WindowManager.LayoutParams.WRAP_CONTENT,
                overlayType(),
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.TRANSLUCENT);
        params.gravity = Gravity.TOP | Gravity.START;
        params.x = dp(12);
        params.y = dp(36);
        params.setTitle("KM14 Floating Panel");
        attachDrag((View) expandedWindow.getTag());
        attachDrag((View) collapsedWindow.getTag());
    }

    private int overlayType() {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                : WindowManager.LayoutParams.TYPE_PHONE;
    }

    private int panelWidth() {
        android.util.DisplayMetrics metrics = new android.util.DisplayMetrics();
        wm.getDefaultDisplay().getMetrics(metrics);
        return Math.min(dp(302), metrics.widthPixels - dp(16));
    }

    private View buildExpanded() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(12), dp(10), dp(12), dp(10));
        root.setBackground(rounded(BG, 18, Color.rgb(55, 70, 91)));
        root.setElevation(dp(12));

        LinearLayout header = new LinearLayout(this);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setPadding(dp(4), dp(2), dp(0), dp(8));
        LinearLayout titleArea = new LinearLayout(this);
        titleArea.setOrientation(LinearLayout.VERTICAL);
        TextView title = text("KM14  /  CONTROL", 16, true, TEXT);
        TextView sub = text("FLOATING OVERLAY · ARM64", 9, true, MUTED);
        titleArea.addView(title, new LinearLayout.LayoutParams(-1, -2));
        titleArea.addView(sub, new LinearLayout.LayoutParams(-1, -2));
        title.setOnTouchListener(new DragTouchListener());
        sub.setOnTouchListener(new DragTouchListener());
        header.addView(titleArea, new LinearLayout.LayoutParams(0, -2, 1));
        header.addView(headerButton("−", "Свернуть", v -> showCollapsed()), new LinearLayout.LayoutParams(dp(38), dp(34)));
        header.addView(headerButton("×", "Закрыть панель", v -> stopSelf()), new LinearLayout.LayoutParams(dp(38), dp(34)));
        root.addView(header, new LinearLayout.LayoutParams(-1, -2));

        statusText = text("НАТИВКА: проверка соединения…", 11, true, ORANGE);
        statusText.setSingleLine(true);
        statusText.setPadding(dp(10), dp(8), dp(10), dp(8));
        statusText.setBackground(rounded(PANEL_ALT, 10, Color.TRANSPARENT));
        root.addView(statusText, margins(0, 0, 0, 10));

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(false);
        scroll.setVerticalScrollBarEnabled(false);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(content, new ScrollView.LayoutParams(-1, -2));

        LinearLayout toggles = section();
        toggles.addView(sectionTitle("МОДУЛИ"), margins(0, 0, 0, 4));
        aimSwitch = createSwitch();
        radarSwitch = createSwitch();
        toggles.addView(switchRow("Aim Assist", aimSwitch));
        toggles.addView(switchRow("Radar", radarSwitch));
        content.addView(toggles, margins(0, 0, 0, 9));

        LinearLayout settings = section();
        settings.addView(sectionTitle("НАСТРОЙКИ НАВЕДЕНИЯ"), margins(0, 0, 0, 8));
        speedSeek = addSlider(settings, "Скорость", "9.11", 2990, 901, 0);
        magnetSeek = addSlider(settings, "Магнит", "0.70", 100, 70, 1);
        fovSeek = addSlider(settings, "FOV", "11.0°", 440, 100, 2);
        content.addView(settings, new LinearLayout.LayoutParams(-1, -2));

        TextView footer = text("Перетаскивай за заголовок. Изменения применяются сразу при подключённой нативке.", 10, false, MUTED);
        footer.setLineSpacing(dp(1), 1.0f);
        footer.setPadding(dp(4), dp(8), dp(4), dp(2));
        content.addView(footer, new LinearLayout.LayoutParams(-1, -2));

        android.util.DisplayMetrics display = new android.util.DisplayMetrics();
        wm.getDefaultDisplay().getMetrics(display);
        int screenHeightDp = Math.round(display.heightPixels / getResources().getDisplayMetrics().density);
        int scrollHeightDp = Math.max(150, Math.min(420, screenHeightDp - 170));
        root.addView(scroll, new LinearLayout.LayoutParams(-1, dp(scrollHeightDp)));
        titleArea.setOnTouchListener(new DragTouchListener());
        root.setTag(titleArea);
        wireListeners();
        return root;
    }

    private View buildCollapsed() {
        LinearLayout pill = new LinearLayout(this);
        pill.setGravity(Gravity.CENTER);
        pill.setOrientation(LinearLayout.HORIZONTAL);
        pill.setPadding(dp(10), dp(7), dp(10), dp(7));
        pill.setBackground(rounded(Color.rgb(24, 32, 47), 18, ACCENT));
        pill.setElevation(dp(12));
        TextView dot = text("●", 12, true, GREEN);
        TextView label = text(" KM14", 14, true, TEXT);
        pill.addView(dot, new LinearLayout.LayoutParams(-2, -2));
        pill.addView(label, new LinearLayout.LayoutParams(-2, -2));
        pill.setOnClickListener(v -> showExpanded());
        pill.setTag(pill);
        return pill;
    }

    private void wireListeners() {
        aimSwitch.setOnCheckedChangeListener((button, checked) -> {
            if (!applyingRemote) edited();
        });
        radarSwitch.setOnCheckedChangeListener((button, checked) -> {
            if (!applyingRemote) edited();
        });
    }

    private Switch createSwitch() {
        Switch sw = new Switch(this);
        sw.setTextColor(TEXT);
        sw.setTextSize(14);
        sw.setShowText(false);
        sw.setThumbTintList(new ColorStateList(
                new int[][]{new int[]{android.R.attr.state_checked}, new int[]{}},
                new int[]{ACCENT, Color.rgb(120, 130, 146)}));
        sw.setTrackTintList(new ColorStateList(
                new int[][]{new int[]{android.R.attr.state_checked}, new int[]{}},
                new int[]{Color.rgb(45, 103, 163), Color.rgb(66, 75, 90)}));
        return sw;
    }

    private LinearLayout switchRow(String label, Switch sw) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        TextView name = text(label, 14, false, TEXT);
        row.addView(name, new LinearLayout.LayoutParams(0, dp(34), 1));
        row.addView(sw, new LinearLayout.LayoutParams(dp(54), dp(38)));
        return row;
    }

    private SeekBar addSlider(LinearLayout parent, String label, String initial, int max, int progress, int type) {
        LinearLayout block = new LinearLayout(this);
        block.setOrientation(LinearLayout.VERTICAL);
        LinearLayout heading = new LinearLayout(this);
        heading.setGravity(Gravity.CENTER_VERTICAL);
        TextView name = text(label, 13, false, MUTED);
        TextView value = text(initial, 14, true, TEXT);
        heading.addView(name, new LinearLayout.LayoutParams(0, -2, 1));
        heading.addView(value, new LinearLayout.LayoutParams(-2, -2));
        block.addView(heading, new LinearLayout.LayoutParams(-1, -2));
        SeekBar seek = new SeekBar(this);
        seek.setMax(max);
        seek.setProgress(progress);
        seek.setPadding(dp(2), 0, dp(2), 0);
        seek.setProgressTintList(ColorStateList.valueOf(ACCENT));
        seek.setThumbTintList(ColorStateList.valueOf(ACCENT));
        seek.setProgressBackgroundTintList(ColorStateList.valueOf(Color.rgb(58, 68, 84)));
        block.addView(seek, new LinearLayout.LayoutParams(-1, dp(28)));
        LinearLayout.LayoutParams lp = margins(0, 0, 0, 4);
        parent.addView(block, lp);
        if (type == 0) speedValue = value;
        else if (type == 1) magnetValue = value;
        else fovValue = value;
        seek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int p, boolean fromUser) {
                if (type == 0) value.setText(String.format(Locale.US, "%.2f", 0.10f + p * 0.01f));
                else if (type == 1) value.setText(String.format(Locale.US, "%.2f", p / 100.0f));
                else value.setText(String.format(Locale.US, "%.1f°", 1.0f + p * 0.1f));
                if (fromUser && !applyingRemote) editedDebounced();
            }
            @Override public void onStartTrackingTouch(SeekBar bar) { adjusting = true; }
            @Override public void onStopTrackingTouch(SeekBar bar) { adjusting = false; edited(); }
        });
        return seek;
    }

    private LinearLayout section() {
        LinearLayout section = new LinearLayout(this);
        section.setOrientation(LinearLayout.VERTICAL);
        section.setPadding(dp(11), dp(9), dp(11), dp(8));
        section.setBackground(rounded(PANEL, 13, Color.TRANSPARENT));
        return section;
    }

    private TextView sectionTitle(String label) {
        return text(label, 10, true, ACCENT);
    }

    private TextView text(String value, int size, boolean bold, int color) {
        TextView tv = new TextView(this);
        tv.setText(value);
        tv.setTextSize(size);
        tv.setTextColor(color);
        if (bold) tv.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return tv;
    }

    private View headerButton(String value, String description, View.OnClickListener listener) {
        TextView button = text(value, 21, false, TEXT);
        button.setGravity(Gravity.CENTER);
        button.setContentDescription(description);
        button.setBackground(rounded(PANEL_ALT, 10, Color.TRANSPARENT));
        button.setOnClickListener(listener);
        return button;
    }

    private GradientDrawable rounded(int color, int radius, int strokeColor) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(radius));
        if (strokeColor != Color.TRANSPARENT) d.setStroke(dp(1), strokeColor);
        return d;
    }

    private LinearLayout.LayoutParams margins(int left, int top, int right, int bottom) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-1, -2);
        p.setMargins(dp(left), dp(top), dp(right), dp(bottom));
        return p;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void showExpanded() {
        if (wm == null || expandedWindow == null) return;
        swapWindow(expandedWindow, panelWidth(), WindowManager.LayoutParams.WRAP_CONTENT);
        collapsed = false;
    }

    private void showCollapsed() {
        if (wm == null || collapsedWindow == null) return;
        swapWindow(collapsedWindow, dp(90), dp(48));
        collapsed = true;
    }

    private void swapWindow(View next, int width, int height) {
        try {
            if (activeWindow != null && attached) wm.removeView(activeWindow);
            params.width = width;
            params.height = height;
            activeWindow = next;
            wm.addView(activeWindow, params);
            attached = true;
        } catch (Exception e) {
            attached = false;
            stopSelf();
        }
    }

    private void attachDrag(View handle) {
        handle.setOnTouchListener(new DragTouchListener());
    }

    private final class DragTouchListener implements View.OnTouchListener {
        private int startX, startY;
        private float touchX, touchY;
        private boolean moving;

        @Override public boolean onTouch(View v, MotionEvent event) {
            if (params == null || !attached) return false;
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    startX = params.x;
                    startY = params.y;
                    touchX = event.getRawX();
                    touchY = event.getRawY();
                    moving = false;
                    return true;
                case MotionEvent.ACTION_MOVE:
                    float dx = event.getRawX() - touchX;
                    float dy = event.getRawY() - touchY;
                    if (Math.abs(dx) + Math.abs(dy) > dp(4)) moving = true;
                    if (moving && activeWindow != null) {
                        params.x = startX + Math.round(dx);
                        params.y = startY + Math.round(dy);
                        try { wm.updateViewLayout(activeWindow, params); } catch (Exception ignored) { }
                    }
                    return true;
                case MotionEvent.ACTION_UP:
                    if (!moving && activeWindow == collapsedWindow) showExpanded();
                    return true;
                case MotionEvent.ACTION_CANCEL:
                    return true;
                default:
                    return true;
            }
        }
    }

    private JSONObject configFromUi() {
        JSONObject body = new JSONObject();
        try {
            body.put("aim_enabled", aimSwitch.isChecked());
            body.put("radar_enabled", radarSwitch.isChecked());
            body.put("speed", 0.10f + speedSeek.getProgress() * 0.01f);
            body.put("magnet", magnetSeek.getProgress() / 100.0f);
            body.put("fov", 1.0f + fovSeek.getProgress() * 0.1f);
        } catch (Exception ignored) { }
        return body;
    }

    private void edited() {
        if (applyingRemote || aimSwitch == null) return;
        lastLocalEdit = System.currentTimeMillis();
        schedulePush(0);
    }

    private void editedDebounced() {
        if (applyingRemote || aimSwitch == null) return;
        lastLocalEdit = System.currentTimeMillis();
        schedulePush(130);
    }

    private void schedulePush(long delay) {
        final JSONObject snapshot = configFromUi();
        synchronized (this) {
            if (pendingPush != null) pendingPush.cancel(false);
            pendingPush = io.schedule(() -> postConfig(snapshot), delay, TimeUnit.MILLISECONDS);
        }
    }

    private void postConfig(JSONObject body) {
        try {
            JSONObject response = request("POST", "/config", body);
            if (response != null && response.optBoolean("ok", false)) {
                main.post(() -> setStatus("● НАТИВКА ПОДКЛЮЧЕНА · " + response.optInt("port", 28765), true));
            } else {
                main.post(() -> setStatus("● НЕТ ОТВЕТА · ПОРТ 28765", false));
            }
        } catch (Exception e) {
            main.post(() -> setStatus("● НАТИВКА НЕ НАЙДЕНА · ЗАГРУЗИ .SO", false));
        }
    }

    private void pollState() {
        try {
            JSONObject state = request("GET", "/state", null);
            if (state == null || !state.optBoolean("ok", false)) throw new IllegalStateException("state");
            main.post(() -> applyState(state));
        } catch (Exception e) {
            main.post(() -> setStatus("● НАТИВКА НЕ НАЙДЕНА · ПОРТ 28765", false));
        }
    }

    private void applyState(JSONObject state) {
        setStatus("● НАТИВКА ПОДКЛЮЧЕНА · " + state.optInt("port", 28765), true);
        if (applyingRemote || aimSwitch == null || adjusting || System.currentTimeMillis() - lastLocalEdit < 2200) return;
        applyingRemote = true;
        try {
            aimSwitch.setChecked(state.optBoolean("aim_enabled", true));
            radarSwitch.setChecked(state.optBoolean("radar_enabled", true));
            float speed = (float) state.optDouble("speed", 9.11);
            float magnet = (float) state.optDouble("magnet", 0.70);
            float fov = (float) state.optDouble("fov", 11.0);
            speedSeek.setProgress(clamp(Math.round((speed - 0.10f) * 100.0f), 0, 2990));
            magnetSeek.setProgress(clamp(Math.round(magnet * 100.0f), 0, 100));
            fovSeek.setProgress(clamp(Math.round((fov - 1.0f) * 10.0f), 0, 440));
        } finally {
            applyingRemote = false;
        }
    }

    private void setStatus(String value, boolean connected) {
        if (statusText == null) return;
        statusText.setText(value);
        statusText.setTextColor(connected ? GREEN : ORANGE);
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
            InputStream stream = code >= 200 && code < 400 ? conn.getInputStream() : conn.getErrorStream();
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

    private int clamp(int value, int min, int max) {
        return Math.max(min, Math.min(max, value));
    }

    @Override
    public void onDestroy() {
        synchronized (this) {
            if (pendingPush != null) pendingPush.cancel(false);
        }
        if (pollTask != null) pollTask.cancel(true);
        io.shutdownNow();
        try {
            if (activeWindow != null && attached) wm.removeView(activeWindow);
        } catch (Exception ignored) { }
        attached = false;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) stopForeground(true);
        else stopForeground(false);
        super.onDestroy();
    }
}
