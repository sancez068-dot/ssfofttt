# KM 1.4 Control Panel — source package

This repository builds an Android control-panel APK and an ARM64 native shared library. The GitHub Actions workflow packages both outputs into `KM14-Control-Panel-Bundle.zip`.

## What is included

- `KM14-Control-Panel.apk`: panel UI with live toggles and sliders.
- `liblogger.so`: native module source build, compatible in name with the existing loader workflow.
- The panel sends settings over HTTP bound only to `127.0.0.1:28765`.

Default settings are aim enabled, radar enabled, speed `9.11`, magnet `0.70`, FOV `11.0°`. Recoil control is removed. Runtime ranges: speed `0.10..30.00`, magnet `0.00..1.00`, FOV `1.0..45.0°`.

## GitHub build

1. Extract this project ZIP.
2. Upload the **contents** of the extracted folder to a GitHub repository (the `.github/workflows/build.yml` path must be preserved).
3. Open **Actions → Build KM14 Control Panel + Native → Run workflow**, or push to `main`/`master`.
4. Download the artifact named `KM14-Control-Panel-Bundle`. It contains the APK and `liblogger.so` in a single ZIP.

The workflow uses JDK 17, Gradle 8.9, Android SDK 35, NDK `27.2.12479018`, and CMake `3.22.1`.

## Runtime connection

The panel app does **not** inject the native library into another process. Load `liblogger.so` into the game using the same loader you already use, then open the panel app. The native library starts the HTTP listener inside the process where it was loaded; the panel connects to `http://127.0.0.1:28765`.

API:

- `GET /state` — current settings.
- `GET /health` — listener health.
- `POST /config` — partial or full JSON update, e.g. `{"aim_enabled":true,"radar_enabled":true,"speed":9.11,"magnet":0.70,"fov":11.0}`.

The listener binds to loopback only, not to the Wi-Fi/LAN interface. If the panel reports no connection, check `logger.log` for `CONTROL_LISTEN_OK` or `CONTROL_BIND_FAIL` and make sure only one copy of the module is loaded.

## Visibility check

The aim code uses a fail-closed test: it accepts a target only when the trace reports that exact target entity as the hit. It no longer treats a near-1.0 trace fraction alone as proof of visibility. If the trace result layout/ABI differs in your game build, the safe failure mode is to skip targets; it may therefore need runtime verification for this exact binary.

## Important verification notes

This is a source package for GitHub Actions; the Android NDK build and live game behavior must be verified by the Actions artifact and runtime log. The target point remains `origin + viewOffset` as an approximation; a true head bone has not been confirmed. The `.so` is ARM64-only and uses offsets from the exact KM 1.4 binary previously analyzed. Do not use it with a different binary without revalidating offsets and hook signatures.
