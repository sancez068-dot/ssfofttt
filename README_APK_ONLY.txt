KM14 Panel V2.4 — APK-only GitHub Actions project

This archive contains the Android floating panel source and a GitHub Actions workflow at the repository root.
It builds only app-debug.apk. It does not compile, bundle, or require the native C source / .so library.
The workflow also deletes any leftover native source/library files under app/src/main before building and fails if the resulting APK contains any .so.

IMPORTANT: extract this ZIP into the ROOT of the GitHub repository, not into a subfolder. It is designed to overwrite the old root build.gradle/settings.gradle/app files and .github/workflows/build.yml.

Build:
- Push to main/master, or open GitHub → Actions → Build KM14 Panel APK Only → Run workflow.
- Download artifact: KM14-Panel-APK-Only
- APK: app/build/outputs/apk/debug/app-debug.apk

Native library is separate and must be compiled by you independently. The panel communicates with the existing local native control API at 127.0.0.1:28765.
