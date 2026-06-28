# Modifications in this fork

This is a fork of [Deskflow](https://github.com/deskflow/deskflow) at the
`v1.26.0` release. It is redistributed under the same licence as upstream,
**GPL-2.0-only WITH LicenseRef-OpenSSL-Exception** (see `LICENSE` and the
`LICENSES/` directory, both unchanged from upstream).

The complete corresponding source for any binary built from this fork is this
repository at the committed revision used for that build.

## Changes from upstream v1.26.0

### Local-inject passthrough (2026-06-15)

**File:** `src/lib/platform/MSWindowsHook.cpp`

Added a general facility for delivering a *deliberately injected* input event to
the **local** machine's OS without Deskflow relaying it to the active client.

- A new constant `kDeskflowLocalInjectSignature = 0x0DF10CA1`.
- In the low-level mouse hook `mouseLLHook`, an injected event
  (`LLMHF_INJECTED`) whose `MSLLHOOKSTRUCT.dwExtraInfo` equals that signature is
  passed straight through with `CallNextHookEx` — so it reaches the local OS and
  is **not** eaten/relayed, even on the primary while relaying to a client.

Stock Deskflow never uses this signature, so all existing behaviour is
unchanged. The facility lets an external automation process (which tags its
`SendInput` with the signature) inject local input on the server that Deskflow
would otherwise forward to the active client.

This is used by the (separate, independently-licensed) "Merged" wrapper to end a
file drag-and-drop on the *source* screen after the cursor has crossed to the
other screen, without the synthetic button-up being forwarded to that screen.
The wrapper communicates with this core only as a child process (command-line
arguments, a config file, and standard streams) and as OS-level input events; it
does not link against or include any Deskflow source.

### Clipboard coexistence with out-of-band file sync (2026-06-25)

**Files:** `src/lib/platform/MSWindowsScreen.cpp`, `src/lib/server/Server.cpp`

The "Merged"/MouseTransfer wrapper syncs **files** across machines out-of-band
(over SMB, placing CF_HDROP on the peer's clipboard itself). Deskflow's built-in
clipboard sync only represents **text/HTML/bitmap** — it cannot represent a file
(CF_HDROP) copy, which marshals to *zero formats*. Left unchanged, Deskflow's
clipboard-ownership protocol destroyed file copies three ways; these changes make
the two clipboard mechanisms coexist. Text/HTML/bitmap sync is unchanged.

1. **`MSWindowsScreen::setClipboard(id, nullptr)` is now a no-op.** Deskflow
   blanked the local clipboard here to "assert ownership" (so it could serve a
   remote owner's data via delayed render). That empty destroyed a user's
   freshly-copied files when a remote clipboard *grab* arrived. The real remote
   data still arrives via the `src != nullptr` path, so nothing is lost; we just
   never blank the local clipboard purely to assert ownership.

2. **`Server::onScreenSwitch` skips pushing a clipboard with no formats.** A
   zero-format clipboard marshals to exactly **4 bytes** (a `uint32` format-count
   of 0), *not* an empty string — so the test is `marshalled.size() <= 4`.
   Pushing such a clipboard onto the newly-active screen would empty its clipboard
   and wipe a file the wrapper had just synced there.

3. **`Server::onClipboardChanged` clears (does not propagate) a zero-format
   clipboard.** Same `size() <= 4` test. The stored copy is set to the empty value
   (rather than left stale) so a later screen-switch does not re-push *stale text*
   over a peer's freshly-synced file; the push to the active screen is skipped.

The core never needs to *see* the file formats (the LocalSystem core reads
CF_HDROP as absent anyway); all three changes act on Deskflow's own
text/HTML/bitmap marshalling, so they are robust to that blindness.

### Cursor jump: centred switch + a switch-request file poll (2026-06-26)

**Files:** `src/lib/server/Server.{h,cpp}`

Two related changes powering the MouseTransfer wrapper's "jump cursor to this computer"
feature (move the shared cursor onto a chosen screen from a tray item / hotkey):

1. **Centred switch.** A new `Server::switchToScreenByName(name)` drops the cursor in the
   **centre** of the target screen rather than at its last-known cursor position (stock
   `jumpToScreen` -> `getJumpCursorPos`, which lands at whatever edge it last left from, or
   top-left if never visited). `handleSwitchToScreenEvent` (the explicit `switchToScreen`
   input-filter action) now calls it. Without this, a jump onto a client landed at the far
   edge/top-left and a jump back to the primary landed at the **seam edge** (looking like
   nothing moved).

2. **Switch-request file poll.** A 150 ms periodic timer polls a wrapper-written file
   (`$MOUSETRANSFER_SWITCHFILE`, else `switchreq` relative to the core's CWD — the launcher
   sets the core's CWD to its bundle dir). The wrapper writes `"<screen> <nonce>"`; on a
   content change the server `switchToScreenByName`s to it. This is the RELIABLE trigger:
   an injected `switchToScreen` hotkey is **not processed by the input filter while the
   cursor is on a client** (verified — the injected key never reaches the filter in relay
   mode), so it cannot pull the cursor back to the server. The file poll calls the switch
   directly on the server's event thread, so it works in both directions.

Scope is narrow: the centred switch only affects **explicit** `switchToScreen` actions and
the file poll (ordinary edge crossings and `switchInDirection` are untouched), and the file
poll is inert unless the wrapper writes the file. Stock Deskflow never reaches either.

### Live layout reload: a reload-request file poll (2026-06-26)

**Files:** `src/lib/server/Server.{h,cpp}`

Powers the MouseTransfer wrapper's **configurable transition-area** feature (drag the
slice of a screen edge that crosses to a given peer, and where it lands on that peer; see
the wrapper's `TRANSITION-AREAS-DESIGN.md`). Editing a transition range rewrites the
external server layout, and this lets the **running** server pick that up with **no
restart** so the mapping is testable while you drag it (clients stay connected).

A second periodic timer (200 ms), modelled on the switch-request poll above, watches a
wrapper-written nonce file (`$MOUSETRANSFER_RELOADFILE`, else `layoutreload` relative to
the core's CWD). On a content change it raises `EventTypes::ServerAppReloadConfig` — the
**same** event the SIGHUP handler raises — which re-reads the external layout file and
hot-applies it via the existing `ServerApp::reloadConfig()` -> `Server::setConfig()` path.
No new apply logic: `Config::read()` does `*this = tmp` (a clean full replace of the live
config the crossing logic reads), and `setConfig()` reconfigures the primary screen and
re-sends options to connected clients.

Scope is narrow and inert unless the wrapper writes the file; stock Deskflow never does, so
`m_reloadReqLast` stays seeded and `checkReloadRequest()` returns early every tick.

### Respect an OS cursor-clip as a lock-to-screen (2026-06-25)

**Files:** `src/lib/server/Server.cpp`, `src/lib/server/PrimaryClient.{h,cpp}`,
`src/lib/deskflow/Screen.{h,cpp}`, `src/lib/deskflow/IPlatformScreen.h`,
`src/lib/platform/MSWindowsScreen.{h,cpp}`

Lets an external process pin the cursor to the current Deskflow screen by simply
confining the OS cursor with `ClipCursor`. The MouseTransfer wrapper uses this for its
"lock cursor to screen while a focused app is fullscreen" feature. Without it the
wrapper's `ClipCursor` cannot stop a switch: Deskflow detects the seam crossing from the
**absolute** cursor position its low-level hook reports (and that hook clamps any
out-of-screen position back onto the screen *edge*, inside the 1px jump zone), and when
`disableLockToScreen = true` (the wrapper's seamless-drag mode) every native lock path is
short-circuited off.

- A new platform query `isCursorClippedToSubRegion()` is added to `IPlatformScreen` with
  a **default `return false`** (so non-Windows screens need no change) and overridden in
  `MSWindowsScreen` to return true when `GetClipCursor` reports the cursor confined to a
  *proper sub-region* of the virtual desktop (1px slack, so an unconfined clip — which
  equals the full virtual screen — never counts). It is forwarded through
  `deskflow::Screen` and `PrimaryClient` exactly like the existing `isLockedToScreen()`.
- `Server::isLockedToScreen()` consults it **first** (before the `disableLockToScreen`
  short-circuit) and returns true when a sub-region clip is in effect. Because the whole
  switch machinery already gates on `isLockedToScreen()` (via `isSwitchOkay()`), this
  suppresses screen switches in **both** directions — cursor-on-primary→secondary *and*
  cursor-on-secondary→primary — so it works whether the fullscreen machine is the server
  or (told over the wrapper's control channel to set its own clip) the server acting for
  a fullscreen client.

It is only consulted when a switch is being considered (near an edge), not on every mouse
event, so it adds nothing to the input hot path. Stock Deskflow sets no such clip, so
`GetClipCursor` equals the full virtual screen and this is always a no-op. The wrapper
communicates purely via OS state (`ClipCursor`); it does not link against or modify any
Deskflow interface.

### Windows 8.0 / 8.1 compatibility (2026-06-26)

**No source change to the fork.** This is a *deployment* note: the `deskflow-core.exe` this
fork builds (VS2022 + Qt 6.8.1 + OpenSSL/vcpkg) runs on Windows 8.0 and 8.1 (x64) once two
runtime-dependency problems are handled. Verified end-to-end on a real Windows 8.0 box
(6.2.9200): server binds + listens and client runs its reconnect loop, both stay up.

The wall was found by diffing every import of `deskflow-core.exe`, `Qt6Core.dll`,
`msvcp140*.dll`, `vcruntime140*.dll`, `concrt140.dll`, and the redist `ucrtbase.dll` against
the *actual* exports of a live Win8.0 `kernel32`/`kernelbase`/`user32`/etc. The core itself,
the MSVC runtime, and the 10.0.19041 redist UCRT all have **zero** Win8-missing imports. The
sole blocker is a single function:

- `Qt6Core.dll` statically imports `kernel32!SetThreadDescription` (Windows 10 1607+ only; Qt
  6.5+ uses it to name worker threads for debuggers, purely cosmetic). On Win8 the loader
  aborts with `STATUS_ENTRYPOINT_NOT_FOUND` (0xC0000139) before `main()`.
- Plus the Universal CRT is absent on a fresh Win8 (it is built into Win10), giving the earlier
  `STATUS_DLL_NOT_FOUND` (0xC0000135) until the UCRT is deployed app-locally.

Fix (no rebuild): `win8/patch-qt6core-win8.ps1` rewrites that one import-by-name entry in a
*copy* of the shipped `Qt6Core.dll` to call `kernel32!GetThreadId` instead (same thread-HANDLE
first arg, return ignored by Qt, harmless on x64 - thread naming silently no-ops), and ship the
UCRT + MSVC runtime app-locally. Full instructions and the exact file list are in
`win8/README.md`. The clean no-patch alternative is to build the core against Qt 6.2 LTS (the
last Qt that supports 8.1 and resolves `SetThreadDescription` dynamically).

### Left-button-up stdout marker for cross-machine drop (2026-06-27)

`Server::onMouseUp(ButtonID)` (`src/lib/server/Server.cpp`) now emits one extra log line,
`LOG_INFO("mousetransfer lbutton up")`, when the released button is `kButtonLeft`. **Additive
logging only — no behaviour change** (the relay path is untouched).

Why: the MouseTransfer wrapper completes a cross-machine Explorer-window drag-and-drop on the
user's mouse release. On a slow cross-client path (e.g. a Windows 8 laptop two seams away) the
release was unreliable to detect: the receiver's button STATE stays up for the whole ghost ride,
Deskflow's relayed up EVENT to that client is occasionally dropped, and on the SERVER both the
wrapper's own low-level hook and `GetAsyncKeyState` are suppressed by the relay while the cursor is
on a client. The server's core, however, is the input source and always sees the real release here.
Surfacing it on stdout (which the wrapper already tails as a black box) gives the wrapper a reliable
release signal; the server then drives the drop to the active-screen peer over its own control
channel. Verified live .12/.18/.65: the `.12→.65` cross-client drop lands on the first release.

### Re-sync key state on screen transition (fix chronic stuck-AltGr / stuck modifier) (2026-06-28)

`MSWindowsScreen::enter()` and `::leave()` (`src/lib/platform/MSWindowsScreen.cpp`) now call
`m_keyState->updateKeyState()` in the **primary** branch (on `leave()` just before
`saveModifiers()`, so the saved + relayed state is corrected; on `enter()` after clearing the
primary key-down list). `updateKeyState()` rebuilds the internal modifier model
(`m_activeModifiers` / `m_mask`) from `pollPressedKeys()` + `pollActiveModifiers()` (OS ground
truth) and **injects no key events** — it only corrects Deskflow's tracking.

Why: the long-standing Synergy/Barrier/Deskflow "keyboard switches / accented characters after
crossing the seam" bug (Deskflow #6599 / #8864 / #9011). Deskflow's internal modifier state can
latch a modifier whose release didn't unwind cleanly — classically AltGr (modelled as Ctrl+Alt),
where Windows fabricates a phantom Left-Ctrl (scancode `0x21D`) that leaves Ctrl "down" internally
while the OS reports it up. With AltGr believed-held, the primary maps **every** captured key
through the AltGr 3rd level, so the secondary reproduces accented vowels (a→á …; consonants/digits
have no AltGr glyph so they pass through). The desync is invisible to the OS (`GetAsyncKeyState`
reads clean), so it can only be fixed inside the core. Confirmed live 2026-06-28 by a client-side
low-level keyboard-hook capture showing every relayed vowel injected wrapped in Left-Ctrl+Left-Alt
while consonants were plain, with the OS modifier state clean on all machines. This mirrors
input-leap's flush-on-transition fix (PR #1972); it self-heals at the next seam crossing.
**Behaviour change is limited to clearing a stale internal modifier on a screen switch** — a
genuinely-held key is re-captured from the OS poll, so legitimate cross-seam modifier use is
unaffected.

**Follow-up (gate-free timer resync, for the Ctrl+Alt+Del / UAC case):** the screen-transition
resync only fires on a seam crossing. The most common *trigger* for the stuck modifier is
**Ctrl+Alt+Del** (the Secure Attention Sequence): Windows routes the Ctrl+Alt key-UP to the Winlogon
secure desktop, which the hook cannot see, so the modifier latches with no crossing involved.
Upstream's own desk-switch resync (PR #9069, `MSWindowsDesks::checkDesk`) is supposed to catch this
but **cannot on the elevated (UIAccess) core**: `OpenInputDesktop()` returns NULL on/after the secure
desktop, so `getDesktopName()` returns `""`, the desktop name never changes, and the
`name != m_activeDeskName` gate never fires → `updateKeys()` is never called.

**Fix (event-driven, EVENT_SYSTEM_DESKTOPSWITCH):** `MSWindowsDesks::enable()` installs a
`SetWinEventHook(EVENT_SYSTEM_DESKTOPSWITCH, …)` on the primary (removed in `disable()`), and its
callback `onDesktopSwitchEvent` calls `updateKeys()` → `MSWindowsScreen::updateKeysCB()` →
`KeyState::updateKeyState()` (rebuilds the modifier mask from `GetKeyboardState()` OS ground truth +
pushes release fixes to the client). The WinEvent is **system-generated and fires independently of
`OpenInputDesktop`**, so it catches the exact secure-desktop round-trip the `checkDesk` poll misses —
event-driven, no polling for key state. Critical detail: `idProcess`/`idThread` MUST be `0` (the
switch is generated by the system, not by us — filtering to our own process would never fire);
`WINEVENT_OUTOFCONTEXT` delivers the callback to the registering thread's existing GetMessage pump
(`MSWindowsEventQueueBuffer`), so it's non-blocking and re-entrant-safe. A static `s_instance` routes
the parameter-less callback to the object. The 0.2 s `checkDesk` poll stays for its original duty
(reinstalling the input hooks on a new desktop) but no longer does per-tick key work; the
screen-transition resync (`307783ecc`) remains as a non-poll backstop. Confirmed live 2026-06-28: the
user identified Ctrl+Alt+Del as the trigger (the stuck state cleared mid-sentence the instant the
cursor crossed a seam, proving the resync works and only the trigger was missing); the WinEvent hook
installs successfully on the elevated UIAccess core. (Superseded an interim per-200 ms unconditional
`updateKeys()` poll, reverted in favour of this event-driven trigger.)

## Building

Standard upstream build (see `doc/dev/build.md`). On Windows: CMake + Ninja +
Qt 6.7+ + OpenSSL (via vcpkg). `deskflow-core` is the only target the wrapper
needs.
