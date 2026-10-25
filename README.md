# KM14 V2.4 — unlimited target distance + FOV 1–360

## Changes
- No local-player-to-target distance cap is applied. The remaining `bone_offset_len` range check validates the head bone's offset from the target origin only; it is not a range limit.
- Native `/config` accepts FOV from 1 to 360 degrees.
- Panel FOV slider source now covers 1.0–360.0 degrees at 0.1-degree steps.
- Existing native-only workflow remains the default. The panel APK is built only if the workflow is manually dispatched with `build_panel=true`.
- Aim visibility checks and performance logic are otherwise preserved from V2.3.

## Build / deploy
1. Push this project to GitHub. Normal push builds only `KM14-Native-Update.zip`.
2. Because the slider range is part of the Android panel UI, run Actions -> Build KM14 Native Update -> Run workflow and set `build_panel=true` once. Download `KM14-Panel-APK-FOV360` and install that APK.
3. Future native changes can continue using the normal native-only artifact without rebuilding the panel.

## Validation
Source changes are syntactic; no live game test was performed here. FOV 360 means the angular filter admits all directions, but visibility checks still reject occluded targets.
