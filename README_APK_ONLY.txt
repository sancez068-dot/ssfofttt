KM14 Panel V2.5 — APK-only project

This archive builds only the Android panel APK from GitHub Actions. It intentionally contains no native C/C++ source and no .so libraries.

Panel groups:
A — Aim Assist: enable, speed, magnet, FOV. The native source applies a fail-closed firearm-only check; unknown weapon classes stop aiming.
E — ESP / Visuals: enemy boxes, local screen-space line to the closest projected opponent, and local-only shot-trail lines. These use the separate GET /visuals endpoint in km14_combined_v2_5.c.
R — Radar: radar/spotted-flag toggle.
U — Interface: hides the collapsed panel badge entirely (not a faded icon); its 90x48 touch area remains active, so tap where the badge used to be to reopen.

Build:
1. Extract this ZIP directly into the repository root (not a subfolder), allowing it to replace app/, build.gradle, settings.gradle, gradle.properties and .github/workflows/build.yml.
2. Push to main/master or run Actions -> Build KM14 Panel APK Only.
3. Download artifact KM14-Panel-APK-Only; it contains app-debug.apk.

Native source is separate: compile km14_combined_v2_5.c to the game-process .so yourself. The panel alone does not contain or load a native library; it sends config to the existing local API on 127.0.0.1:28765.

Important validation caveat:
- The C source passes a local Clang syntax-only check.
- The Android APK was not built here and no live game test was performed.
- m_hActiveWeapon offset 0x1268 and WorldToScreenMatrix/viewport usage were identified from static audit notes, but active-weapon decoding and screen projection still require runtime confirmation against the exact KM1.4 binaries. The weapon gate fails closed for unknown/unreadable classes.
- World-to-screen values can be shifted if the game viewport and Android overlay viewport differ; confirm in-game before relying on the visual features.
