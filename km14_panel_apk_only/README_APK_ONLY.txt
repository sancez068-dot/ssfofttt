KM14 Panel APK-only project (V2.4 UI)

This project builds only the Android panel APK. It does not compile or package a native .so.
The separate native source is provided outside this ZIP as km14_combined_v2_4.c.

GitHub Actions:
- Push to main/master or run the workflow manually.
- Download artifact: KM14-Panel-APK-Only
- APK path: app/build/outputs/apk/debug/app-debug.apk

The panel retains its existing configuration/protocol. Native library compilation is intentionally omitted so you can compile the .c source yourself.
