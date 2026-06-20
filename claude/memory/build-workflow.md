---
name: build-workflow
description: How the home-accounting desktop project is edited locally and built/tested on remote appbuild
metadata: 
  node_type: memory
  type: project
  originSessionId: 5778f742-bb34-4c48-81d2-da3f41687685
---

Source of truth for desktop C++ is `/workspace/home-accounting/desktop`. The build/test host is the remote `appbuild` (ssh alias, project at `/home/builder/project/home-accounting`).

**Sync code to appbuild** (no `rsync` on remote — use tar):
`cd desktop && tar czf - src tools CMakeLists.txt | ssh appbuild 'cd /home/builder/project/home-accounting/desktop && tar xzf -'`

⚠️ **Match the remote `cd` to the tar's relative paths, or files land in a stray dir.** A tar made from `.../android` with entries `app/...` must extract under `.../home-accounting/android` (NOT `.../home-accounting`, which creates a stray `app/` and the real `android/app` keeps stale code — builds/tests then silently pass on the OLD code). After a migration also `rm` deleted files on the remote (tar extract never deletes) and verify with md5 before trusting any test result.

**Build/test on appbuild:**
`ssh appbuild 'cd .../desktop && cmake -S . -B build && cmake --build build -j4 --target <tgt>'`
Targets: `home-accounting` (app), `guitest` (headless UI/sync-dialog harness, run with `QT_QPA_PLATFORM=offscreen`), `syncv2test` (model+sync unit tests, no Qt).

The user sometimes edits files directly on appbuild (e.g. Store.cpp `ensureIdentity`). Before editing, diff local vs appbuild and adopt their version as baseline.

**Android** builds on `androidbuild2` (`/home/builder/project/home-accounting/android`, system `gradle` 9.5.1, no wrapper, proxy configured). Push with the same tar+ssh. Unit tests: `gradle :app:testDebugUnitTest` (JVM, src/test, no emulator); also `:app:assembleDebug` / `:app:assembleRelease`. Testing is now unit-test based (no Appium/emulator). Notes: `Crypto` must be per-Store (keystore passed in), not an `object` singleton, or two Stores in one test JVM share one TLS identity. The test task sets `LC_ALL=C.UTF-8` so the JVM encodes Cyrillic **filenames** (DB dir «Основная») as UTF-8 — otherwise the build host's locale mangles them to "????????" (file *contents* are always written UTF-8 explicitly, so only names are affected). Gradle caches test results: use `--rerun-tasks` to force a re-run.

**Cross-platform compat tests** (desktop↔Android, format + sync-exchange framing): desktop tool `xcompattest produce|verify <dir>` (CMake target) and Android `XCompatTest` (env-gated: `XC_MODE=produce|verify XC_DIR=<dir>`, auto-skips without `XC_MODE`). Each platform *produces* a fixture (DB dir + wire-framed `exchange.bin`); the other *verifies* it asserts the same canonical scenario. Orchestrate by tar-piping the fixture between the two hosts through the local shell, then run the other side's verify. Don't install anything on build hosts.
