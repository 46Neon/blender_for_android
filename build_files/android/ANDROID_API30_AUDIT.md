# Android 11 / API 30 adaptation status

This branch (`android-11-api-30`) lowers the port's configured native and APK
minimum from API 31 to API 30 for arm64-v8a. This is a source/build-configuration
adaptation, **not a compatibility certification**. No complete API-30 native
build, APK install, or physical-device run has been completed as part of this
change.

## Configuration alignment

- `env.sh`: default ABI arm64-v8a and native minimum `ANDROID_API=30`.
- NDK CMake platform and native dependency compilers consume `ANDROID_API`;
  `d8 --min-api` and the manual `aapt2 --min-sdk-version` consume the same value.
- Gradle's `minSdk` is 30. Compile SDK remains 35 and `targetSdk` remains 34;
  these are distinct from the minimum OS level and are not lowered just to
  install on Android 11.
- Fast deploy now defaults to DEX API 30, and the API-30 env default is used by
  the normal build, dependency build, APK packager, and toolchain smoke test.
- The NDK legacy `android.toolchain.cmake` deliberately reports
  `CMAKE_SYSTEM_VERSION=1` to inhibit CMake's built-in Android NDK handling.
  This is not the selected OS API: `ANDROID_PLATFORM=android-30`, the emitted
  compiler target `aarch64-none-linux-android30`, and API-30 sysroot establish
  the actual native minimum. See the [NDK toolchain source](https://android.googlesource.com/platform/ndk/+/master/build/cmake/android-legacy.toolchain.cmake).
- Default build output is isolated at `../blender_build_android_api30` (or
  `../blender_build_android_api${ANDROID_API}` when overridden), including the
  Android CMake platform fallback. The dependency prefix is marked with
  ABI/API/NDK identity. Existing unmarked or mismatched
  prefixes fail closed; no prior build output is removed. An explicit
  `BUILD_BASE` remains possible but must point to a compatible prefix.

## API-sensitive findings

- Android 11 is API 30 / `VERSION_CODES.R`. `BlenderActivity` uses
  `Environment.isExternalStorageManager()` and the all-files-access settings
  intents behind an API-R guard. The API-30 `StorageVolume.getDirectory()`
  method is used to resolve storage volumes. These APIs exist at the minimum,
  but the user-grant flow and actual file access still need Android 11 device
  tests.
- The manifest's `.blend` `pathSuffix` requires API 29 and is compatible with
  API 30.
- AAudio requires API 26, below this minimum.
- The OIDN patch substitutes Bionic pthread-affinity calls (unavailable until
  API 36) with `sched_*affinity`; glog's Android config avoids relying on
  `backtrace` declarations that require API 33. These are code-level mitigations,
  not proof that every dependency links or loads at API 30.
- The NDK minimum platform controls available native symbols. A library that
  references a symbol newer than API 30 may fail at load time even if the
  relevant code path is not used. The full transitive `.so` set and
  `libblender.so` must be audited after an API-30 build.
- Vulkan is a runtime requirement of this port although the manifest feature is
  optional for installation. Android/API level alone does not guarantee a
  compatible Vulkan driver. The target device's GPU/driver capability has not
  been established here. The `full` profile's resource use may also exceed what
  a lower/midrange phone can handle; `lite` should be evaluated separately.

## Focused CI verification (2026-10-17)

[GitHub Actions run 38993070899](https://github.com/46Neon/blender_for_android/actions/runs/38993070899) passed on this branch with NDK `28.2.13676358`, ABI `arm64-v8a`, and native minimum API 30:

- Native CMake/NDK smoke test and API-30 dependency builds for zlib, zstd, and libdeflate passed; the shared libraries were checked as ELF64/AArch64 and the dependency prefix marker matched ABI/API/NDK.
- oneTBB and host ISPC installed, and patched Open Image Denoise compiled and linked its static core and CPU-device archives. The archive checks found `sched_getaffinity`/`sched_setaffinity` and no undefined `pthread_getaffinity_np`/`pthread_setaffinity_np` references.
- Gradle assembled the API-30 **shell test APK** and confirmed `minSdkVersion=30`, `targetSdkVersion=34`.

This is focused component evidence only. The APK has no Blender native payload; the run did not compile `libblender.so`, package Blender, inspect the complete transitive library set, install an APK, or test in an emulator/physical device. It does not certify Blender/API-30 compatibility.

## Required verification before claiming API 30 support

1. Build all dependencies from scratch in the isolated API-30 `BUILD_BASE`;
   investigate and fix each compiler/linker failure rather than reusing API-31
   artifacts.
2. Build `libblender.so` and both supported packaging paths; inspect APK
   `minSdkVersion`=30, `targetSdkVersion`=34, ABI arm64-v8a, native ELF class and
   all linked/imported symbols against API-30 NDK stubs.
3. Install and launch on an Android 11/API 30 arm64 device; validate Vulkan
   surface creation/rendering, input/keyboard, suspend/resume, storage-access
   grant, opening/saving `.blend` files, and logs/crashes. Also test the Vulkan
   driver and memory/performance limits. An emulator or another Android version
   does not replace the target-device run.
4. Repeat with the actual Infinix Hot 2022. Its earlier successful launch was
   with a separate unofficial Blender 3.6.2 APK and does **not** validate this
   repository's Blender 5.3 build.

## Work performed for this audit

Static source review and consistency checks cover the Android environment,
NDK/dependency scripts, Gradle and manual APK package paths, fast deploy,
manifest/storage code, and Android build instructions. The focused CI evidence
above has passed, but no complete Blender build/APK, full transitive-symbol
audit, APK install, emulator test, or physical-device test has been completed.
API 30 compatibility therefore remains **unverified**.
