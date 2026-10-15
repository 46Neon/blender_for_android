---
name: android-crash-debug
description: Find the cause of a native crash, freeze or misbehaviour in the Blender Android app. Use whenever the app dies on the device, or when a change "has no effect" and the reason is not obvious from the code.
---

# Debugging a crash on the device

The whole method is one rule: **measure, do not reason**. Every time a crash here
was diagnosed by reading code, the answer was wrong; every time it was
instrumented, the answer arrived in one build. The sections below are ordered the
way a session should actually run.

---

## 1. The crash log is already on the device

Do not ask anyone to reproduce it while you watch. Android keeps native crashes
in a dedicated buffer that survives the process:

```bash
adb -s "$SERIAL" logcat -d -b crash
```

That gives the signal, the fault address, the registers and a backtrace already
symbolized for exported functions. `logcat -d` alone will **not** show it — the
crash buffer is separate.

Read these three lines first:

```
signal 11 (SIGSEGV), code 2 (SEGV_ACCERR), fault addr 0x... (write)
Process uptime: 10s
#01 pc ... libblender.so (blender::ui::text_clip_middle_ex(...)+412)
```

- **`(write)` vs `(read)`** narrows it to a store or a load.
- **`Process uptime`** says whether it died during startup or during use. Two
  crashes in a row at the same short uptime means it relaunched into a bad state
  and is looping — the state is the bug, not the action.
- **A bare address at `#00` with a named `#01`** means `#00` is a static or
  inlined function called from `#01`. The named frame is where to start reading,
  not where the fault is.

## 2. Do not build a theory on the registers

An ARM64 dump prints `x0`–`x28`, and it is tempting to map them onto the
arguments of the named frame. **Do not.** In optimized code they belong to
whatever the compiler last put there, in a frame that may be inlined.

This cost two wrong hypotheses in one session: `x2 = 0xfffffffffffffffa` was read
as "a memmove got size −6", which produced a plausible and completely wrong story
about an unsigned underflow defeating an overflow guard. The real bad value was a
*destination offset*, in a different branch, and the size was 4 the whole time.

Registers are a hint about *which* value is wrong. They are not evidence about
*which line*.

## 3. Reproduce it yourself

Injected input is faster than a round trip to the user, and it makes the
reproduction repeatable:

```bash
adb -s "$SERIAL" shell input tap 324 1290
adb -s "$SERIAL" shell input swipe 150 600 150 300 700     # x1 y1 x2 y2 ms
adb -s "$SERIAL" shell input stylus swipe 150 600 150 300 700
```

Take a screenshot first and read the coordinates off it — crop rather than scale
so the pixels are the real ones:

```bash
adb -s "$SERIAL" exec-out screencap -p > shot.png
ffmpeg -i shot.png -vf "crop=800:420:0:130" -y crop.png   # coords stay real
```

**Orientation is a variable, and it is settable.** One crash here only ever
happened in portrait, because a narrow panel reached a case a wide one never
does. Landscape testing said "cannot reproduce" for an hour.

```bash
adb -s "$SERIAL" shell settings get system accelerometer_rotation   # save first
adb -s "$SERIAL" shell settings get system user_rotation
adb -s "$SERIAL" shell settings put system accelerometer_rotation 0
adb -s "$SERIAL" shell settings put system user_rotation 0          # 0 = portrait
```

Restore both when finished — it is the user's phone.

**"Cannot reproduce" is a result, not a failure.** It says the trigger is a
condition you have not set yet: orientation, a restored session, a preference,
a window size. Report it and hunt the difference rather than repeating the run.

## 4. Instrument every branch at once

Blender's CLOG cannot be enabled on Android (no argv), so log directly:

```cpp
#ifdef __ANDROID__
#  include <android/log.h>
#  define BLDBG(...) __android_log_print(ANDROID_LOG_INFO, "BLDBG", __VA_ARGS__)
#else
#  define BLDBG(...) ((void)0)
#endif
```

```bash
adb -s "$SERIAL" logcat -c && ...reproduce... && adb -s "$SERIAL" logcat -d -s BLDBG
```

**Cover the whole function, not the branch you suspect.** Instrumenting one
branch produced zero output and burned a build cycle — which did prove the branch
was never taken, but a full trace would have proved that *and* shown the real
path in the same run. Log at entry, at every branch, and just before every write.

Print the actual values, including the string being worked on. The line that
solved the crash was:

```
R: okw=48.00 sepw=16.00 l_end=4 max_len=550 str='    Brush'
R: trimmed l_end=1038974096, writing 4 bytes at +1038974096
```

`l_end` going from 4 to 1038974096 across one statement *is* the diagnosis. No
amount of reading the function produced it.

## 5. Confirm the instrumentation actually shipped

Before concluding anything from an empty log:

```bash
strings -a .../lib/arm64-v8a/libblender.so | grep -c "your format string"
```

A `1` means it is in the binary and the silence is real. A `0` means the build
did not include it and the conclusion would have been false. This check is two
seconds and has already prevented one wrong turn.

C++ instrumentation lives in `libblender.so`, so the default fast build path
carries it. Python-side logging does **not** — that needs `--repackage`; see the
`android-build` skill.

## 6. Clean up before committing

Restore the instrumented file from a copy taken before patching, rather than
unpicking edits:

```bash
cp source/.../file.cc /root/file.cc.orig     # before instrumenting
cp /root/file.cc.orig source/.../file.cc     # after
grep -c "BLDBG" source/.../file.cc           # 0
```

Then apply the real fix to the clean file, rebuild, and verify on the device
before committing — always.

---

## Traps that cost time in this project

- **Git Bash eats variables in `wsl.exe bash -lc '...'`.** `$ZIP` expanded to
  nothing and `unzip -l ""` reported the payload as empty, which nearly became
  "the templates are not packaged". Write anything non-trivial to a script file
  and run that. The same applies to `for f in ...` loops and heredocs.
- **Exit codes are lost through `wsl.exe`.** `$?` is 0 even when the command
  failed. Grep the output for a success marker instead.
- **A background task stuck at 0.0% CPU is blocked on stdin**, usually a `grep`
  that lost its filename to trap one. It is not doing work; kill it.
- **Check what is on screen before driving the device.** The phone is the user's,
  and it has had a render in progress and a personal app in the foreground at
  different points. `adb shell dumpsys window | grep mCurrentFocus`.

## What a good report looks like

State separately: what the evidence shows, what it does not, and which of your
earlier statements it overturns. In this project the useful pattern has been

> reproduced / not reproduced, the captured numbers, the line they implicate,
> and what is still unknown

rather than a confident narrative that has to be walked back next turn.
