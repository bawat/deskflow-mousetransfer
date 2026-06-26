# Running deskflow-core on Windows 8.0 / 8.1

`deskflow-core.exe` from this fork (built with the normal toolchain: VS2022, Qt 6.8.1,
OpenSSL via vcpkg) **does** run on Windows 8.0 and 8.1 (x64) once two deployment problems
are handled. Neither requires changing the fork's source or the build.

This was diagnosed and verified end-to-end on a real Windows 8.0 box (6.2.9200): with the
two fixes below, `deskflow-core server` binds and listens, and `deskflow-core client` runs
its reconnect loop - both stay up. Windows 8.1 (6.3) is a strict superset of 8.0's
kernel32 export surface, so 8.0 success guarantees 8.1.

## The two problems

### 1. The Universal CRT is not present on a fresh Win8

On Windows 10 the UCRT (`ucrtbase.dll` + the `api-ms-win-crt-*` API sets) is part of the OS.
On Win8 it is not, so a first launch dies with `STATUS_DLL_NOT_FOUND` (0xC0000135). Fix:
deploy the UCRT **app-locally** (next to the exe). See the file list below.

### 2. Qt6Core imports a Windows-10-only function

After the UCRT is present the loader then fails with `STATUS_ENTRYPOINT_NOT_FOUND`
(0xC0000139). The cause - found by diffing every import of `deskflow-core.exe`,
`Qt6Core.dll`, `msvcp140.dll`, `vcruntime140*.dll`, `concrt140.dll`, and the redist
`ucrtbase.dll` against the real exports of an actual Win8.0 `kernel32`/`kernelbase`/etc. -
is a **single function**:

```
Qt6Core.dll  ->  kernel32!SetThreadDescription
```

`SetThreadDescription` only exists on Windows 10 1607+. Qt 6.5+ (we ship 6.8.1) links it
statically to name worker threads for debuggers (`qthread_win.cpp`). It is purely cosmetic.

Everything else is fine: `deskflow-core.exe` has **zero** Win8-missing imports of its own,
and the MSVC runtime + the 10.0.19041 redist UCRT are clean too. Even Qt's
`WaitOnAddress`/`WakeByAddress*` (the `api-ms-win-core-synch-l1-2-0` set) are present on
Win8.0.

**Fix:** `patch-qt6core-win8.ps1` rewrites that one import-by-name entry in a *copy* of the
shipped `Qt6Core.dll` to call `kernel32!GetThreadId` instead - same thread-HANDLE first
argument, return value ignored by Qt, harmless extra argument on x64. Thread naming silently
no-ops; nothing else changes (on any OS). `GetThreadId` exists on every Windows since Vista.

```
powershell -NoProfile -ExecutionPolicy Bypass -File patch-qt6core-win8.ps1 <bundle>\Qt6Core.dll
```

The script is idempotent and edits the DLL in place. Apply it to the bundled Qt6Core.dll as
a packaging step (e.g. in the wrapper's `package.ps1`, right after Qt6Core is staged).

## What to ship for a Win8 bundle (x64)

Next to `deskflow-core.exe`, in addition to the fork's own outputs
(`libssl-3-x64.dll`, `libcrypto-3-x64.dll`):

- **Qt:** `Qt6Core.dll` (PATCHED with this script).
- **MSVC runtime** (from `VC\Redist\MSVC\<ver>\x64\Microsoft.VC143.CRT\`):
  `msvcp140.dll`, `msvcp140_1.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`.
- **Universal CRT, app-local** (the entire `ucrt\DLLs\x64\` folder from a Windows SDK redist,
  e.g. `Windows Kits\10\Redist\10.0.19041.0\ucrt\DLLs\x64\`): `ucrtbase.dll` plus all the
  `api-ms-win-crt-*.dll` and `api-ms-win-core-*.dll` shims (~42 files). The 19041 redist UCRT
  is itself Win8-clean; the original downlevel UCRT (10.0.10240 from KB2999226) also works but
  is not required.

On Windows 10+ these app-local copies are simply shadowed by / equivalent to the OS ones, so
the same bundle keeps working on Win10/11 - the only universal side effect of the patch is the
loss of debugger thread names, which a headless core never needs.

## Alternative (no binary patch): build against Qt 6.2 LTS

Qt 6.2 LTS still officially supports Windows 8.1 and resolves `SetThreadDescription`
dynamically, so a `Qt6Core.dll` from Qt 6.2 has no Win10-only static import and needs no
patch. That means downloading Qt 6.2, pointing the fork's `CMAKE_PREFIX_PATH` at it, and
rebuilding (the headless core only uses the small, stable Qt6Core surface - QSettings, QString,
QFile, QCoreApplication - so it should compile). The binary patch above was chosen because it
is proven, self-contained, and needs no second Qt install.
