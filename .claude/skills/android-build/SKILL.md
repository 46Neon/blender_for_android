---
name: android-build
description: Build, package, deploy and debug the Blender Android APK. Use for any task that compiles this port, installs it on the device, or investigates why a change did not take effect on the phone.
---

# Building Blender for Android

The port lives in WSL and is driven from a Windows session. Nearly every hour lost
on this project has gone to one of four traps, all of which are silent — the build
reports success and the device runs the old code. They are listed first for that
reason.

## The four traps

### 1. A change under `scripts/`, `assets/` or `release/datafiles` needs `--repackage`

`build.py` has three paths and picks the cheapest one that looks sufficient:

| Path | When it is chosen | What it updates |
| --- | --- | --- |
| fast | default, once a build tree and stage exist | **`libblender.so` only** |
| compile + repackage | `--repackage`, `--clean`, `--validation`, `--debuggable`, or no stage yet | native libs + runtime payload |
| full | no `build.ninja` yet | configure, compile, package |

The fast path swaps the shared library into the staged APK and reuses the runtime
payload untouched. So a C++ edit lands with a plain `build.py full`, but a Python
script, keymap, datafile, asset or manifest edit **does not**, and nothing warns
you.

```bash
python3 build_files/android/build.py full --repackage
```

### 1b. A version or DNA change needs `--reconfigure`

The fast path swaps `libblender.so` and nothing else — including the **host code
generators**. `makesdna` and `makesrna` keep whatever build they had, so a change
to `BLENDER_FILE_SUBVERSION` or to a DNA header leaves the compiled binary and
the generated DNA/RNA disagreeing.

The result is not a compile error. The app installs, the activity starts, and the
process dies before `android_main` logs its first line — no native crash, no Java
exception, nothing in `logcat -b crash`. It looks like the APK is broken.

```bash
python3 build_files/android/build.py full --reconfigure
```

Confirm the tools actually moved:

```bash
ls -l --time-style=+%H:%M "$BUILD_BASE/build_host_tools_full/bin/"   # makesdna, makesrna
```

When a build stops launching with no diagnostic at all, install the previous
archived APK first. If that one runs, the regression is in the change, not the
device, and this is the first thing to check.

### 2. The device unpacks the runtime once

`BlenderActivity.extractRuntimeIfNeeded()` writes `.installed-<version>-<revision>`
into `<filesDir>/blender/5.3` and returns early whenever that marker exists.

The revision now comes from `blender_runtime.rev`, a hash of the payload archive
that `package.sh` writes at build time, so a changed payload invalidates the marker
on its own. Before that it was a hand-edited Java constant, and forgetting to bump
it left edited Python stranded inside the APK while the phone kept running a copy
unpacked weeks earlier — the failure looks exactly like "my code has no effect".

If you ever suspect the device is running a stale payload, clear it:

```bash
adb shell pm clear org.blender.blender
```

That also erases user preferences, so ask before doing it. User config lives at
`<filesDir>/config/5.3`, outside the payload root.

### 3. `BUILD_BASE` is not in the environment

The dependency prefix and build trees live at `/root/blender_android_deps`, but
that is only exported by `/root/run_apk_build.sh`, not by the login shell.
`build.py` now autodetects it (looking for `lib/android_arm64`), exports it so the
shell scripts agree, and aborts immediately with a clear message if it is wrong —
it used to guess a nonexistent path and fail two minutes later inside cmake.

Override with `BUILD_BASE=...` or `--build-base DIR` when needed.

### 4. Exit codes are lost through `wsl.exe`

```bash
wsl.exe -d ubuntu-26.04 bash -lc 'python3 -c "import sys; sys.exit(1)"; echo $?'   # prints 0
```

**Never trust `$?` or the harness's reported exit code for anything run through
`wsl.exe`.** Grep the log for a success marker instead:

```bash
grep -E "archive\]|failed|error:" /root/apk_build.log | tail -5
```

## Environment

| Thing | Value |
| --- | --- |
| Working copy | `/root/blender-android-fast` (WSL `ubuntu-26.04`) |
| From Windows | `MSYS_NO_PATHCONV=1 wsl.exe -d ubuntu-26.04 bash -lc '...'` |
| Build base | `/root/blender_android_deps` |
| SDK / JDK / NDK | `build_files/android/env.sh` owns these; do not hardcode |
| Host compiler | `ANDROID_HOST_CC=gcc-15 ANDROID_HOST_CXX=g++-15` |
| APK archive | `/root/blender_android_deps/apk/blender-full-latest.apk` |
| adb | `C:/Users/vfxon/AppData/Local/Android/Sdk/platform-tools/adb.exe` |
| Package / activity | `org.blender.blender` / `.BlenderActivity` |

`MSYS_NO_PATHCONV=1` matters: Git Bash rewrites anything that looks like a Unix
path, so `/root/...` becomes `C:/Program Files/Git/root/...` without it.

## Building

```bash
MSYS_NO_PATHCONV=1 wsl.exe -d ubuntu-26.04 bash -lc 'cd /root/blender-android-fast && python3 build_files/android/build.py full > /root/apk_build.log 2>&1; grep -E "archive\]|failed|error:" /root/apk_build.log | tail -5'
```

Run it in the background — a full build is 20+ minutes, an incremental C++ change
about 5. Useful flags: `--repackage`, `--clean`, `--install`, `--run`,
`--keep N` (archived APKs to retain), `--no-archive`, `--build-base DIR`,
`-s SERIAL`.

Every build is archived as `blender-full-<date>-<time>.apk` with
`blender-full-latest.apk` hard-linked to the newest.

## Verifying the build contains your change

Do not assume. Check the object file was actually recompiled:

```bash
find /root/blender_android_deps/build_android_full -name 'YOURFILE.cc.o' -newermt '-10 minutes'
```

For a payload change, look inside the packaged archive:

```bash
unzip -p /root/blender_android_deps/android_apk_stage_full/assets/blender_runtime.zip \
  scripts/path/to/file.py | grep 'your change'
```

## Deploying

```bash
MSYS_NO_PATHCONV=1 "C:/Users/vfxon/AppData/Local/Android/Sdk/platform-tools/adb.exe" install -r "//wsl.localhost/ubuntu-26.04/root/blender_android_deps/apk/blender-full-latest.apk"
```

adb reads the APK straight off the WSL share; no copy to Windows is needed. Then:

```bash
adb shell am start -n org.blender.blender/.BlenderActivity
```

Confirm it survived startup rather than assuming — a Vulkan or payload failure
exits within seconds:

```bash
adb shell 'for i in 1 2 3 4 5 6; do sleep 2; pidof org.blender.blender; done'
```

## Debugging on the device

For a crash, a freeze or anything needing a trace, use the **`android-crash-debug`**
skill — it covers the crash buffer, input injection, orientation as a variable,
and how to instrument without wasting a build. The essentials:

**Inject input instead of asking the user to try it.** This turns a guess into a
measurement and costs nothing:

```bash
adb shell input tap 460 460
adb shell input swipe 1010 1700 1010 1250 800     # x1 y1 x2 y2 duration_ms
```

Injected events arrive as real finger touches, so they exercise the same GHOST
path the user does.

**Temporary native logging** beats reading code when a hypothesis has already been
wrong once. Blender's CLOG cannot be enabled on Android (no argv), so log directly:

```cpp
#ifdef __ANDROID__
#  include <android/log.h>
#  define BLDBG(...) __android_log_print(ANDROID_LOG_INFO, "BLDBG", __VA_ARGS__)
#endif
```

```bash
adb logcat -c && adb logcat -d -s BLDBG | head -60
```

Remove the instrumentation before committing.

**Screenshots**: `adb exec-out screencap -p > shot.png`. Check what is in the
foreground first — `adb shell dumpsys window | grep mCurrentFocus` — because the
phone is the user's personal device and may not be showing Blender.

## Committing

**Commit from WSL git, never from Windows git.** `NUL/` is a tracked directory
holding git-lfs hooks; Windows cannot represent that name and reports the files as
deleted, so a Windows commit silently removes them.

```bash
MSYS_NO_PATHCONV=1 wsl.exe -d ubuntu-26.04 bash -lc 'cd /root/blender-android-fast && git add -A && git commit -m "..."'
```

The working copy is configured with `core.fileMode=false` (the tree was seeded
from a Windows mount that reports mode 777) and `core.autocrlf=input`.

## "It built, installed, and nothing changed"

Work down this list before re-reading any source. Each row has actually happened.

| Symptom | Check |
| --- | --- |
| A Python/keymap/asset edit has no effect, C++ edits do | Trap 1 — the fast path never repackaged. Rebuild with `--repackage`. |
| Even a repackaged payload has no effect | Trap 2 — the device kept the old extraction. Confirm `[apk] runtime revision:` changed in the log. |
| cmake complains about a missing `LIBDIR` | Trap 3 — `BUILD_BASE`. Should now abort early with a clear message instead. |
| The build "succeeded" but produced nothing | Trap 4 — the exit code lied. Grep the log. |
| An event-handling change seems inert | Instrument. The UI layer has several unreachable-looking paths: `handle_button_event` deals with `MOUSEMOVE` itself in `WAIT_RELEASE` and only calls `do_button` from its `default:` case. |

## Habits that pay

- Verify on the device before committing. That has been the rule all along.
- One hypothesis at a time, and instrument rather than guess after the first miss.
- When a change "has no effect", check traps 1 and 2 *before* re-reading the code.
