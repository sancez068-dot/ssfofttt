# KM14 Floating Overlay — GitHub Actions project

This project builds an Android APK with a **movable, collapsible floating overlay** and the ARM64 native module `liblogger.so` in one ZIP artifact. The launcher activity is only used to request Android's “Display over other apps” permission; the actual controls appear as a floating window above the game.

## Aim-assist fix V1.2 (diagnostic build)

This update removes a faulty gate that only ran the aim routine when the original `CreateMove` returned nonzero. The routine now runs for every hooked call with a non-null command, while preserving the original return value. Recoil compensation remains disabled. The visibility rule is intentionally strict: a target is accepted only when the trace result's hit entity equals the candidate entity; a trace fraction alone never enables aiming.

New log records in `logger.log`:

- `CREATE_MOVE_CALL` — proves the hook is called and shows the original return value, command pointer, interface readiness and in-game state.
- `TRACE_RESULT` — first eight trace results, including fraction and whether the hit pointer equals the candidate. If these always show `hit_equals_target=0`, the trace ABI/layout or endpoint needs correction; do not weaken the wall check based on fraction alone.
- `ASSIST_SKIP` — throttled reason when the routine exits early (not in game, bad local player state, missing trace interface, etc.).
- `FRAME` / `TARGET_SCAN` — scan counts, selected player and changed command angles.

Expected defaults remain speed `9.11`, magnet `0.70`, FOV `11°`; recoil control is off. This patch is source-checked with Clang but still requires the Android GitHub Actions build and an in-game log to confirm the actual runtime path.

## Overlay behavior

- Draggable by the title area; the compact pill is also draggable.
- `−` collapses the panel into a small `KM14` pill. Tap the pill to expand it.
- `×` stops the overlay service; reopen the app to start it again.
- Aim Assist and Radar switches, and Speed / Magnet / FOV sliders.
- Sends changes immediately to `http://127.0.0.1:28765/config` and reads the live state from `/state`.
- The overlay service is a foreground service so Android is less likely to stop it while the game is active.

## Included outputs

The workflow packages:

- `KM14-Control-Panel.apk` — floating overlay APK.
- `liblogger.so` — ARM64 native library built into the APK.
- `README.md` — this file.

The ZIP artifact is named `KM14-Control-Panel-Bundle.zip`.

## Build with GitHub Actions

1. Extract this ZIP locally/Termux.
2. Upload the **contents** of the extracted project directory to the repository root. Preserve `.github/workflows/build.yml`.
3. Open **Actions → Build KM14 Control Panel + Native → Run workflow** (or push to `main` / `master`).
4. Download the `KM14-Control-Panel-Bundle` artifact.

The workflow uses JDK 17, Gradle 8.9, Android SDK 35, NDK `27.2.12479018`, and CMake `3.22.1`.

## First launch

1. Install `KM14-Control-Panel.apk`.
2. Open it once and allow **Display over other apps** in Android settings.
3. The Activity closes and the floating overlay appears. You can move it by dragging the title, collapse it with `−`, and reopen it by tapping the `KM14` pill.
4. Load `liblogger.so` in the game's process with the loader already used for KM 1.4. The panel's status should change to `NATIVE CONNECTED` when the native listener is ready on `127.0.0.1:28765`.

The APK does **not** inject the `.so` into the game. The library must already be loaded in the target process by the user's existing loader.

## Native control API

- `GET /state`
- `GET /health`
- `POST /config` with JSON fields `aim_enabled`, `radar_enabled`, `speed`, `magnet`, `fov`.

The native listener is designed to bind only to loopback. The APK uses the same device's `127.0.0.1` endpoint. If the status remains disconnected, check `logger.log` for `CONTROL_LISTEN_OK` / `CONTROL_BIND_FAIL` and ensure only one copy of the native module is loaded.

## Verification note

This ZIP is prepared for the repository's GitHub Actions build. A full Android SDK/NDK build and live overlay test have not been run in this environment. Exact KM 1.4 binary offsets remain version-specific.
