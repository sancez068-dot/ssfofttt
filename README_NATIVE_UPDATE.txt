# KM14 Native Update — V1.7

This project keeps the existing floating panel APK unchanged. The GitHub Actions workflow builds **only** the ARM64 native library update artifact; it does not publish a replacement panel APK.

## V1.7 changes

- Enemy-only targeting: live players on team 2/3 and on the opposite team from the local player.
- Line-of-sight gate restored using `EngineTraceClient003` and its `TraceRay` slot 4. Candidates blocked by a wall are rejected. A missing trace interface prevents aim-hook activation instead of silently allowing targets through walls.
- Aim speed now controls the response at every angle error, rather than only capping larger angular steps. Magnet controls response strength. Angular step is capped to avoid the old 25-degree write guard silently dropping high-speed updates.
- Recoil control remains off. Target point remains an approximate head-height point (`origin + 64`), not a verified model-bone position.
- Panel protocol remains `127.0.0.1:28765`; panel Java files and UI protocol are unchanged.

## Build and install flow

1. Keep the already installed `KM14-Control-Panel.apk`. **Do not reinstall or redownload it for native-only updates.**
2. Commit this project to the existing GitHub repository. Each push runs the `Build KM14 Native Update` workflow.
3. Download the `KM14-Native-Update` Actions artifact. It contains `liblogger.so`, its SHA-256 checksum, and this update note.
4. Stop the game and your existing loader, replace the old `liblogger.so` at the exact location your loader uses, then launch the game and load the new library. Do not load both versions in the same game process.
5. Open the already installed floating panel. It continues to send settings to the native library at `127.0.0.1:28765`.

The panel APK contains an older build of the library as a packaged resource, but your existing loader runs the separate `liblogger.so` in the game process. Replacing only the external `.so` is the correct update path for this architecture.

## Validation note

The C source passes local Clang syntax checks. A full Android NDK build or in-game test is not available in this environment. The visibility trace depends on this game build's `Ray_t` / `CGameTrace` ABI and must be confirmed from the runtime log. V1.7 writes `TRACE_RESULT` records including fraction, target hit, clear ray, and solid-start flags.
