# KM14 Floating Overlay — GitHub Actions project

This project builds an Android APK with a **movable, collapsible floating overlay** and the ARM64 native module `liblogger.so` in one ZIP artifact. The launcher activity is only used to request Android's “Display over other apps” permission; the actual controls appear as a floating window above the game.

## Aim-assist fix V1.3 (trace slot correction)

The supplied runtime log showed that the CreateMove hook was active and target scanning sometimes found candidates, but every logged trace returned `hit=NULL` and `fraction=1.0`. That meant the strict visibility test rejected all candidates. The previous build called `EngineTraceClient003` vtable slot 4 as if it were `TraceRay`; in the conventional Source `IEngineTrace` order, slot 4 is `ClipRayToCollideable` and `TraceRay` is slot 5. V1.3 calls slot 5 and logs both slot addresses. The trace mask is the standard Source `MASK_SHOT` value `0x46004003`.

The visibility guard remains strict: a candidate is accepted only if the trace's hit entity equals that candidate. There is no fraction-only fallback, so the patch does not intentionally weaken wall blocking. Recoil control stays off. Defaults: speed `9.11`, magnet `0.70`, FOV `11°`.

New diagnostic values: `INTERFACES_OK` prints `traceSlot4_clipRayToCollideable`, `traceSlot5_traceRay`, and `traceRaySlot=5`; `TRACE_RESULT` includes `trace_slot=5`. If the runtime interface differs from the conventional Source layout, the next log will make that discrepancy visible. This was not tested live in-game.

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
