# seifg-android

Android Zygisk/LSPosed frontend for libseifg frame generation, Unity-scoped.
Seeded from hexstr's UnityFPSUnlocker (MIT).

Status: scaffolding only. Zygisk module structure + config loading.
libseifg and ShadowHook integration not wired yet.

Build (requires NDK r26+ with abseil prefab):

```
cmake -S seifg-android -B build-android -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release
cmake --build build-android
```
