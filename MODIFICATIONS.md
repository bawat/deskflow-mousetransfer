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

#### Extended to the keyboard hook (2026-07-26)

**File:** `src/lib/platform/MSWindowsHook.cpp`

The 2026-06-15 change covered only the mouse. `keyboardLLHook` now performs the
*same* check on `KBDLLHOOKSTRUCT::dwExtraInfo`: an injected key event carrying
`kDeskflowLocalInjectSignature` is passed straight through with
`CallNextHookEx`, so it reaches the local OS and is not eaten/relayed. The two
hooks are now symmetric — same constant, same meaning, same three lines.

Without it, a synthetic keystroke delivered on a machine whose core is relaying
(cursor on another screen) is swallowed by the core and forwarded to that other
screen instead of landing locally. That breaks two wrapper features that inject
keys deliberately: picture-in-picture keyboard passthrough (type into the machine
whose screen you are watching) and relaying a paired machine's own physical
keyboard to whichever screen is active.

Deskflow's own internal synthesis escape hatch
(`DESKFLOW_HOOK_FAKE_INPUT_VIRTUAL_KEY` / `g_fakeServerInput`) is deliberately
*not* reused: it is armed with a `PostThreadMessage` to Deskflow's own hook
thread and is unreachable from an external process — which is precisely why the
mouse side needed the cross-process `dwExtraInfo` tag in the first place.

Stock Deskflow never injects with this signature, so all existing keyboard
behaviour is unchanged.

**Note on `g_isPrimary` (documentation only, no behaviour change).** Both hooks
also contain a `!g_isPrimary && injected` passthrough branch that looks like it
should bypass relaying on client machines. `g_isPrimary` is declared
`static BOOL g_isPrimary = TRUE` in `MSWindowsHook.cpp` and, as of this revision,
is **never assigned anywhere in the tree** — `MSWindowsScreen` drives
primary-vs-secondary behaviour entirely through
`m_hook.setMode(kHOOK_RELAY_EVENTS / kHOOK_WATCH_JUMP_ZONE / kHOOK_DISABLE)`.
The condition is therefore always false and both branches are dead on Windows
regardless of role, which makes the `dwExtraInfo` tag the only working bypass for
either hook. The dead branches are left untouched (removing upstream code buys
nothing); a comment in the file records this so nobody builds role-dependent
injection logic on top of them.

### Clipboard format visibility on an ELEVATED core (2026-07-28)

**Files:** `src/lib/platform/MSWindowsClipboard.{h,cpp}`

`IsClipboardFormatAvailable()` **misreports a REGISTERED clipboard format that was
placed by a LOWER-PRIVILEGE process**, when the reader runs elevated (LocalSystem +
UIAccess — how the MouseTransfer wrapper starts the core so it can drive UAC prompts).

Scope matters, and was measured rather than assumed. A format the elevated core placed
**itself** is visible to it normally, so Deskflow's own echo-suppression has always
worked: across the whole server log, no client ever announced a grab after writing a
received clipboard. Two same-integrity processes also see each other's formats fine.
Only the cross-privilege direction fails — which is exactly the direction a wrapper
needs, and one stock Deskflow never exercises.

Measured on Windows 10 19045 with the core's own temporary instrumentation:

```
MTDIAG isOwnedByDeskflow: cached=C242 fresh=C242 availCached=0 availFresh=0 formats=[000D C242]
```

The atom is correct (a freshly registered atom equals the cached one, and equals the
atom the *writing* process used), `EnumClipboardFormats` lists `C242` sitting on the
clipboard — and `IsClipboardFormatAvailable(C242)` still answers "no" for the very
format it just enumerated. A core running as the normal **user** answers "yes" to the
identical clipboard, deterministically. Standard formats (`CF_TEXT`, `CF_UNICODETEXT`,
…) are reported correctly in both contexts; only registered ones are affected.

Two user-visible consequences, both of which presented as *"the core ignores what we
tagged"*:

1. **`Deskflow Ownership` set by another process was invisible**, so the elevated core
   treated every such write as a foreign clipboard change and fired `ClipboardGrabbed`
   — defeating its own ownership protocol and re-publishing content that had been
   explicitly marked as already-synced.
2. **`HTML Format` is likewise a REGISTERED format** (`MSWindowsClipboardHTMLConverter`),
   and normal apps that publish HTML (browsers, editors) run unelevated — the same
   cross-privilege direction — so `has()` would not see HTML on the clipboard and HTML
   sync degrades to plain text. *Inferred from the measured mechanism and fixed by the
   same change; not separately reproduced.*

Fix: a new `MSWindowsClipboard::isFormatOnClipboard()` used by `isOwnedByDeskflow()`
and `has()`. It keeps `IsClipboardFormatAvailable` as a fast path — so unelevated
behaviour is byte-identical — and falls back to an `EnumClipboardFormats` scan, which
is truthful in both contexts. The scan reuses an already-open clipboard when the caller
holds one (`has()`, the unit tests), and otherwise opens it with a **bounded retry**
(10 × 5 ms): the clipboard is a single-holder lock and this runs from the
clipboard-change notification, exactly when every other watcher is looking too — a
single attempt lost that race often enough to make the fix look intermittent
(measured 1-in-5 before the retry, 10-in-10 alternating trials after).

**Not sufficient on its own for "write without propagating"** — see the next entry.

### Clipboard ownership gates the FETCH path too (2026-07-28)

**Files:** `src/lib/server/Server.cpp`, `src/lib/deskflow/IPlatformScreen.h`,
`src/lib/deskflow/Screen.{h,cpp}`, `src/lib/server/PrimaryClient.{h,cpp}`,
`src/lib/platform/MSWindowsScreen.{h,cpp}`

Two paths publish the primary's clipboard, and only one consulted the ownership marker:

| path | trigger | gated on |
|---|---|---|
| announce | `MSWindowsScreen::onClipboardChange` → grab | `!isOwnedByDeskflow()` ✅ |
| fetch | `Server::onScreenSwitch` → `onClipboardChanged` | `m_clipboardOwner` only ❌ |

`m_clipboardOwner` is assigned at grab time and then sticks, so a screen that grabbed
once re-reads and re-publishes whatever is on its clipboard at **every later leave** —
marker or no marker.

Stock Deskflow never notices, which is why this was not a pre-existing bug: the only
content it writes locally came from the server, so the re-read marshals identical bytes
and the `data == m_clipboardData` dedup in `onClipboardChanged` swallows it. It matters
the moment something places **different** content and tags it as already-synced — a
wrapper writing a peer's clipboard — which is exactly what the marker asserts must not
spread.

Fix: gate that block on a new `isClipboardOwnedByUs()`, plumbed the same way as the
existing `isCursorClippedToSubRegion()` query (`IPlatformScreen` default **false** →
`Screen` → `PrimaryClient` → `MSWindowsScreen`, which answers with
`MSWindowsClipboard::isOwnedByDeskflow()`). Non-Windows platforms take the default, so
stock behaviour there is untouched. Now one rule holds on both paths: **content the sync
mechanism placed is never re-published.**

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

4. **`MSWindowsScreen::onClipboardChange` fires the grab on every non-Deskflow
   clipboard change** (2026-07-02), not only on the owned→unowned transition.
   The transition form depended on the ownership-assert blank that change (1)
   removed: without that write the screen was almost never "owned", so
   `m_ownClipboard` stayed false and user copies never reached the server as
   `ClipboardGrabbed`. Clipboard OWNERSHIP then froze on the primary, whose
   stored (stale) clipboard was re-pushed onto every screen the cursor entered —
   silently replacing files the user had just copied there (invisible to the
   core), i.e. "copied a file, Paste greyed out after a crossing". Firing the
   grab unconditionally restores the ownership flow: the server re-marks the
   owner and **empties its stored copy** (`handleClipboardGrabbed`), so nothing
   stale is ever re-pushed, and a text copy made on a *client* syncs again (its
   leave-time data send is no longer rejected as mis-sequenced). Redundant grabs
   are protocol-safe and cost one tiny message per user copy.

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

### Core->wrapper event push (loopback UDP) + stdout flush (2026-08-10)

`ConsoleLogOutputter::write` (`src/lib/base/LogOutputters.cpp`) now, on **Windows only**:

1. `fflush(stdout)` / `fflush(stderr)` after the existing `std::cout.flush()`, and
2. fire-and-forget sends the **verbatim log line** over a loopback UDP datagram to
   `127.0.0.1:24820` whenever the line carries one of four latency-sensitive transition
   markers: `entering screen`, `leaving screen`, `switch from "` or `mousetransfer lbutton up`.

**Additive only — no behaviour change to the KVM path, and no new log output.** A small
anonymous-namespace helper (guarded by `#if defined(_WIN32)`) owns one process-lifetime
non-blocking UDP socket; the needle set is exactly the substrings the wrapper's own log
classifier keys on. The port is fixed (env override `MOUSETRANSFER_CORENOTIFY_PORT` for
emergencies, must match on both sides). `winsock2.h`/`ws2tcpip.h` are included at the very top
of the file (before `arch/Arch.h`, which can pull in `windows.h`) to avoid the winsock1/2 clash;
`ws2_32.lib` is linked via `#pragma comment`.

Why: the MouseTransfer wrapper learns that the shared cursor has crossed a seam (and sees the
left-button-up drop marker) by watching this core's stdout. When the core runs **elevated**
(LocalSystem) its stdout is redirected to `coreout.log`, which the CRT block-buffers, and the
wrapper reads it back with a tailing poll — together adding variable, sometimes-large latency
that has broken drag handoffs with a few-hundred-ms freshness window (an RDP window-drag handoff
missed its 400 ms latch because `leaving screen` arrived late). The push gives the wrapper the
same events **event-driven** (it blocks on the socket, no poll); the `fflush` independently keeps
the existing log-tail timely as a fallback. Both are pure **process-boundary** emits — a datagram
and a file write — so nothing links the wrapper to this GPL core. The wrapper de-duplicates by
line, so an older wrapper (log-tail only) or a lost datagram still works via the tail.

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

**Required companion fix — `pollPressedKeys()` must read GLOBAL key state.** Live testing showed the
WinEvent fired but the modifier *still* didn't clear. Root cause: `KeyState::updateKeyState()` rebuilds
the shadow key array `m_keys` from `MSWindowsKeyState::pollPressedKeys()`, which used **`GetKeyboardState`
— a THREAD-LOCAL API** (it reflects only the input the calling thread's message queue has processed).
The resync runs on the desk thread, whose queue never saw the Ctrl+Alt key-UP that the SAS routed to the
secure desktop, so `GetKeyboardState` there reports Ctrl+Alt stuck DOWN permanently and the resync
overwrote the latched modifier with the same stale value. (`pollActiveModifiers()` derives the Ctrl/Alt/
Shift bits from `m_keys` via `isKeyDown`, so it inherits the staleness.) Confirmed by the wrapper's kbdiag
at the SAS instant: `async=[]` (GetAsyncKeyState, global, clean) vs `logical=[LCTRL,LALT]` (GetKeyState,
thread-local, stuck). Fix: `pollPressedKeys()` now reads **`GetAsyncKeyState`** (the global, real-time
physical+injected state, clean after the SAS) instead of `GetKeyboardState`, so the resync clears the
stale modifier from any thread. `pollPressedKeys()` is only called by `updateKeyState()`, so the change is
scoped to the resync. This is also why the earlier screen-transition resync appeared to work: crossing a
seam runs `AttachThreadInput`, which syncs the thread's keyboard state first — the new global poll removes
that dependency.

**THE actual fix — hook `g_keyState` re-sync bit test (`MSWindowsHook.cpp`).** The visible symptom
(relayed keys translated as AltGr after Ctrl+Alt+Del) is driven by the low-level keyboard hook's OWN
key-state array `g_keyState`, used in `keyboardHookHandler` to decide AltGr translation (`ToUnicode`) for
**relayed** keys (local typing is unaffected — the OS translates that with its own clean state).
`keyboardGetState()` is meant to re-sync `g_keyState` from the global `GetAsyncKeyState` whenever the
current key is down — but the gate was `if (key & 0x80)`, and `GetAsyncKeyState` reports "down" in the HIGH
bit `0x8000`, not `0x80` (the loop right below correctly uses `key < 0`). So the re-sync almost never ran;
`g_keyState` drifted on any missed event. Ctrl+Alt+Del is the worst case: the SAS routes the Ctrl/Alt
key-UP to the Winlogon secure desktop the hook can't see, so `g_keyState[Ctrl]`/`[Alt]` latch `0x80`
forever and every relayed key is translated through the AltGr (Ctrl+Alt) layer. Fix: test `key < 0`, so the
re-sync fires on each key-down and clears the stale modifier from the global state on the very next
keystroke — event-driven, no polling. Confirmed by the exact user-reported behaviour: local typing on the
server is fine (OS state clean); relayed typing breaks; pressing Ctrl clears it (the Ctrl key-up updates
`g_keyState[Ctrl]=0` incrementally, breaking the both-Ctrl-and-Alt AltGr condition). This is the primary
fix; the EVENT_SYSTEM_DESKTOPSWITCH resync + `pollPressedKeys`-global change above remain as belt-and-
suspenders for the `m_keys`/`m_mask` path.

### Don't poison the server language when the layout read fails (2026-07-03)

**File:** `src/lib/deskflow/win32/AppUtilWindows.cpp` (`getCurrentLanguageCode`). Commit `606c64c6b`.

The **second facet** of the same Ctrl+Alt+Del trigger, distinct from the `g_keyState` modifier latch
above and NOT covered by it — which is why the "accented vowels on a client after Ctrl+Alt+Del" symptom
recurred on cores that already had `ca93bbf8b`. `Server::handleKeyDownEvent` calls
`getCurrentLanguageCode()` on **every** keystroke and ships the result to the client with the key.
That function reads the foreground window's HKL via `getCurrentKeyboardLayout()`
(`GetGUIThreadInfo(0, &gti)`). While the **Winlogon secure desktop** is up (Ctrl+Alt+Del / a UAC
prompt) this process cannot read that desktop's foreground, so `gti.hwndActive` is null, the function
logs `failed to determine current keyboard layout` and returns `nullptr`. `getCurrentLanguageCode()`
then fell through returning its pre-initialised `std::string code("", 2)` — **two NUL bytes, size 2,
NOT empty**. On the client, `ServerProxy::setActiveServerLanguage()` sees a non-empty (`"\0\0"`)
language, so it takes the *populated* branch: caches it as `m_serverLanguage`, finds it "not installed
on client", and degrades every subsequent relayed keystroke's translation (accented / vowel-mangled
characters). The state persists because the client only overwrites `m_serverLanguage` when a *different*
non-empty language arrives.

Fix, in `getCurrentLanguageCode()`: (1) on a null foreground layout, fall back to `GetKeyboardLayout(0)`
— this thread's own input layout, which stays valid across the desktop switch — so a real `"en"` is
still reported; (2) if even that is null, return an **empty** `std::string()` so the client takes its
`active server language is empty` branch and **keeps the last known-good language** instead of caching
`"\0\0"`. Observable signal that it worked: after a Ctrl+Alt+Del the client no longer logs
`current server language is not installed on client`. Stock Deskflow bug (upstream `AppUtilWindows.cpp`
is identical) — upstream cherry-pick candidate like `ca93bbf8b`.

### Re-sync the hook key-state on a desktop switch — the actual persistent-AltGr fix (2026-07-03)

**Files:** `src/lib/platform/MSWindowsHook.{h,cpp}` (`resyncKeyState`) + `MSWindowsDesks.cpp`
(`onDesktopSwitchEvent`). Commit `e71ac1d32`. Supersedes the effectiveness of `ca93bbf8b`'s gate for
the AltGr-latch case.

**The bug.** `keyboardHookHandler` runs `ToUnicode(..., g_keyState)` to build the relayed **KeyID** for
a key typed on the primary while the cursor is on a client. If `g_keyState[Ctrl]`/`[Alt]` are latched,
`a` is baked into the KeyID as `á` (`id=0x00e1`) with a *clean* modifier mask, and the client faithfully
types the accent. `ca93bbf8b` corrected the re-sync bit test (`key & 0x80` → `key < 0`) but left the
re-sync **gated on `GetAsyncKeyState(vkCode) < 0`** (the current key reading down), which inside a
`WH_KEYBOARD_LL` hook is usually still **up** (the event hasn't entered the system yet), so the gate
never opens and after Ctrl+Alt+Del the phantom `g_keyState[Ctrl]`/`[Alt]` stay latched.

**The trap that makes this subtle** (a first attempt, `110e92360`, resynced modifiers from
`GetAsyncKeyState` on *every* keystroke and REGRESSED — it broke relayed capital letters). While the
primary is relaying, the physical modifier keys are **swallowed** by the hook, so `GetAsyncKeyState`
**under-reports a genuinely-held modifier**. A per-keystroke resync therefore drops a held Shift/Ctrl
(no capitals). `g_keyState` is in fact maintained accurately by the hook's own key up/down events; the
ONLY case it drifts is a key-UP eaten by a desktop switch.

**The fix.** Expose `MSWindowsHook::resyncKeyState()` (a full `g_keyState[i] = GetAsyncKeyState(i)`
refresh) and call it **only** from the `EVENT_SYSTEM_DESKTOPSWITCH` handler (`onDesktopSwitchEvent`,
already present from `d6a08dd71`). The desktop switch is the one moment `g_keyState` can drift AND the
one moment `GetAsyncKeyState` is trustworthy — no key is being pressed/swallowed, so it reads the true
idle physical state and copying it clears the phantom Ctrl/Alt without ever touching a held modifier
during normal typing. Keeps BOTH the stuck-AltGr fix and Shift capitals. Verified live .12/.18/.65:
identical Ctrl+Alt+Del, `aeiou` relayed clean AND `Shift`+letter produces capitals. Stock Deskflow bug —
upstream cherry-pick candidate.

## Building

Standard upstream build (see `doc/dev/build.md`). On Windows: CMake + Ninja +
Qt 6.7+ + OpenSSL (via vcpkg). `deskflow-core` is the only target the wrapper
needs.

### Graceful keepalive: don't drop a slow-but-connected client (2026-07-13)

**Files:** `src/lib/server/ClientProxy1_0.cpp`, `src/lib/server/ClientProxy1_0.h`

Stock Deskflow's server drops a client the instant it misses the heartbeat death window
(`kKeepAliveRate` 3s × `kKeepAlivesUntilDeath` 3 = **9s**): `ClientProxy1_0::handleFlatline()`
logged `"client is dead"` and `disconnect()`ed immediately, WITHOUT checking whether the TCP
connection was still alive. On a momentarily CPU-saturated client (whose single-threaded event
loop can't emit a heartbeat within 9s) this caused a needless **disconnect → reconnect flap**, a
cursor-relay outage each cycle (observed on the MouseTransfer 2-vCPU test-rig guests: ~1000 flaps
under browser+product load).

`handleFlatline()` now treats a flatline as **"slow / busy, still connected"** rather than dead:
it keeps the client and gives it another window, up to `kMaxMissedHeartbeats` (3) CONSECUTIVE
silent windows (~27s of total silence) before declaring it dead. Any data from the client (a
heartbeat echo or real input) resets the count in `handleData()`. A genuinely closed TCP still
drops immediately via the separate `handleDisconnect()`/`handleWriteError()` paths, unchanged. Net:
a transiently slow client is no longer flapped, while a truly-dead one is still dropped.

### Clipboard-chunk storm diagnostics (2026-07-28)

**Files:** `src/lib/client/Client.cpp`, `src/lib/deskflow/ClipboardChunk.cpp`,
`src/lib/deskflow/ClipboardChunk.h`, `src/lib/server/ClientProxy1_6.cpp`,
`src/lib/server/ClientProxy1_6.h`

**Diagnostic only — no behaviour change.** Added while investigating an outage in which every
client lost the shared cursor for 90+ minutes: a ~8 MB screenshot on the SERVER's clipboard was
chunked into 512 KB `kMsgDClipboard` messages, and after a client dropped mid-transfer that chunk
stream began arriving at the head of every NEW connection, ahead of the server's greeting.
`Client::handleHello` reads a fixed 7-byte protocol name (`kMsgHello` = `"%7s%2i%2i"`), so each
client read `'DCLP'`, rejected the greeting and retried ~1 Hz forever. A machine that had been
switched OFF for the whole incident failed identically on its first ever attempt, and only
restarting the SERVER's core cleared it.

1. `Client::handleHello` — when the protocol name is unrecognised, also log the greeting bytes in
   hex and `m_stream->getSize()` (how much is already queued behind them), and decode the
   clipboard id when the bytes are a `kMsgDClipboard` header. The bare token could not distinguish
   an incompatible PEER (upgrade it) from a MIS-FRAMED stream (a message body arrived where the
   greeting belongs — nothing is wrong with either build), and those call for opposite actions.
   Logged at WARN so it is visible at the default log level.

2. `ClipboardChunk::send` — log the DESTINATION STREAM pointer alongside the chunk header
   (clipboard id, sequence, mark). These events are queued against a raw proxy pointer, so "which
   stream did this chunk go to" is the question that matters, and the old `"sending clipboard
   chunk"` line could not answer it.

3. `ClientProxy1_6` — construction/destruction now log the proxy and stream addresses. The
   destructor is no longer `= default` purely so it can trace. It DELIBERATELY does not remove the
   `ClipboardSending` handler the constructor registers on `this`; adding that removal is the
   leading candidate fix, and fixing and measuring in the same build would destroy the evidence
   that the fix is the right one.

(2) and (3) are switched on by a `clipdiag` SENTINEL FILE in the core's working directory (the
once) and emit at NOTE when enabled, falling back to DEBUG1 otherwise — deployed cores run at the
DEFAULT log level, so DEBUG1 alone would have made them invisible on exactly the machines that
need observing. Per 512 KB chunk, never per frame, so the cost is negligible even when on.

### FIX: SecureSocket's per-socket write state (was function-local statics) (2026-07-29)

**Files:** `src/lib/net/SecureSocket.cpp`, `src/lib/net/SecureSocket.h`

**This is a real upstream bug, not a MouseTransfer-specific tweak** — it predates this fork
(`git log -S s_staticBuffer` traces the block to `665bd91db "#5628 Move SSL socket code from plugin
to lib/net"`). Worth reporting to Deskflow.

`SecureSocket::doWrite()` kept its pending-write state in FOUR function-local statics —
`s_retry`, `s_retrySize`, `s_staticBuffer`, `s_staticBufferSize` — i.e. **one set shared by every
TLS socket in the process**:

```cpp
if (s_retry) {
  bufferSize = s_retrySize;          // another socket's SIZE...
} else {
  bufferSize = m_outputBuffer.getSize();
  memcpy(s_staticBuffer, m_outputBuffer.peek(bufferSize), bufferSize);
}
status = secureWrite(s_staticBuffer, bufferSize, bytesWrote);   // ...and its DATA
if (status == 0) { s_retry = true; s_retrySize = bufferSize; return New; }
```

When a socket backs up, `SSL_write` returns `WANT_WRITE`, and the retry branch exists because
OpenSSL requires a retried write to be handed the same buffer contents again. But the latch is
GLOBAL: a socket that hit `WANT_WRITE` set `s_retry` with its own payload still in the shared
buffer, and **if that connection then died, nothing cleared either**. The next socket's `doWrite`
took the retry branch, used the DEAD connection's size, never copied its own output buffer, and
transmitted the dead connection's data down the new connection. `discardWrittenData()` then popped
from the new socket's own buffer, corrupting that too.

Observed 2026-07-28 as a total, non-self-healing KVM outage: a large clipboard on the server backed
a socket up, a client died mid-transfer, and from then on EVERY newly accepted connection received
a 524,302-byte clipboard chunk where its 11-byte greeting belonged, so every client rejected the
greeting and retried at ~1 Hz forever. Only restarting the server's core cleared it, because the
statics live for the life of the process. Caught on the wire — first read of a brand-new socket:

```
HEALTHY : bytes=15    first: 00 00 00 0b 42 61 72 72 69 65 72 00   len=11,     "Barrier"
FAILING : bytes=4096  first: 00 08 00 0e 44 43 4c 50 00 00 00 00   len=524302, "DCLP"
```

`TCPSocket::doWrite()` (the plaintext path) never had this — it peeks its own buffer. The
asymmetry was the bug.

The state is now per-socket members (`m_writeRetry`, `m_writeRetrySize`, `m_writeBuffer`). The
buffer is a `std::vector<uint8_t>` rather than the old realloc'd raw pointer, so it is released
with its socket and cannot outlive it — the static version had already needed one leak fix upstream
(`a56abf68d "#6488 Fixed a memory leak in the TLS socket code"`).

`SecureSocket::doRead()`'s `static uint8_t buffer[4096]` — also shared process-wide, where
`TCPSocket::doRead` uses a plain local — is made a plain local in the same change. It did NOT cause
this outage (each call memsets and refills it), but it is the same hazard class for no benefit.

Full investigation, the deterministic repro and the refuted theories: the wrapper repo's
`claudemd/clipboard.md` and `seeyoulater/testtasks/clipboard-chunk-storm-repro.md`.

### Audit: every other shared-mutable-state instance of the same class (2026-07-29)

After fixing `SecureSocket::doWrite`, the fork was swept for the same bug class — per-connection or
per-instance state held in process-wide storage. Six more live instances were found and fixed; the
rest of the codebase is clean.

**`SecureSocket`: four more `static int retry;`** (in `secureRead`, `secureWrite`, `secureAccept`,
`secureConnect`). These are worse than they look. `checkResult()` does **`retry++`** for every
"want read/write/connect/accept" case rather than assigning, so the counter ACCUMULATED across
unrelated sockets, and `ssl_mutex_` is a per-INSTANCE member so nothing serialised them either.
`secureAccept` and `secureConnect` then branch on `retry == 0` to decide a TLS handshake is
complete:

```cpp
if (retry == 0) {          // "if not fatal and no retry, state is good"
  ...verify fingerprint...
  m_secureReady = true;
}
```

So one socket completing its handshake could make a different socket, still mid-handshake, conclude
it was secure — or a socket whose `SSL_accept` had succeeded could stall because an unrelated socket
had left the counter positive. Now four per-operation members
(`m_sslReadRetry` / `m_sslWriteRetry` / `m_sslAcceptRetry` / `m_sslConnectRetry`).

**Clipboard reassembly: `ClipboardChunk::s_expectedSize` plus a `static std::string dataCached` in
BOTH `ServerProxy::setClipboard` (client side) and `ClientProxy1_6::recvClipboard` (server side).**
Between them, one buffer and one expected-size served every clipboard id and every connection — on
the server, every connected CLIENT — so two overlapping transfers appended into a single buffer and
were checked against a single size. That is the outage's other half, the observed
`corrupted clipboard data, expected size=8294452 actual size=17113192` (exactly two whole transfers
plus one 512 KB chunk). Replaced by `ClipboardChunk::Assembly`, an array held per connection and
indexed by clipboard id, so a transfer for clipboard 0 cannot disturb clipboard 1 and one connection
cannot disturb another. Two robustness fixes ride along: the clipboard id is validated BEFORE it is
used to index (it comes straight off the wire), and the buffer is RESET on error — previously a
corrupt transfer was left in place and poisoned the size check of the next one to arrive without an
intervening DataStart.

**Assessed and left alone** (genuinely process-global, not per-instance state):
`MSWindowsClipboard::s_ownershipFormat` (a `RegisterClipboardFormat` id, identical process-wide by
definition), `MSWindowsScreen::s_windowInstance` (the module `HINSTANCE`),
`OSXScreen::s_testedForGHOM`/`s_hasGHOM` (a one-time OS capability probe), the Meyers singletons in
`I18N`/`Settings`, and `MSWindowsScreen`'s `bogusZoneSize` (declared `static` but never written — a
constant in disguise). A scan for file-scope mutable statics across `net`/`deskflow`/`server`/
`client`/`base`/`io`/`mt` came back empty.

## SEC-08b — a fatal TLS handshake no longer freezes the whole mesh (2026-08-03)

`SecureSocket::secureAccept` slept a full second (`Arch::sleep(1)`) on a FATAL handshake, on the shared
`SocketMultiplexer` service thread — the one thread servicing every socket in the process, live KVM
connections included. So a single failed handshake froze all input relay for a second, and ~10
back-to-back reached the flatline window and dropped clients: an **unauthenticated ~1 packet/second DoS
against the whole mesh**, needing no certificate at all (a plaintext connect to the KVM port 24800 is
enough — confirmed live 2026-08-03, a 60 s flood stuttered the cursor and dropped a client). Matches
upstream [deskflow#8214](https://github.com/deskflow/deskflow/issues/8214) ("Non-TLS client interrupts
mouse cursor for connected TLS clients"), **Closed without the mechanism named**; the sleep survived to
v1.26.0. **Removed the sleep** — `serviceAccept` returns `nullptr` on a fatal accept (`status < 0`), so
the multiplexer TEARS THE SOCKET DOWN: there is no same-socket "hammering" to throttle, and each
attacker connection is a fresh socket accepted, failed and closed in microseconds. The neighbouring
`s_retryDelay` (10 ms) sleeps on the non-fatal WANT_READ/WANT_WRITE retry path are left as-is (100×
smaller, on the legitimate in-progress-handshake path, and the retry job only re-fires on readability).
**To be reported upstream** with the mechanism spelled out.

## SEC-08 — rejected handshakes no longer leak the socket (cherry-picked from upstream, 2026-08-03)

A fingerprint-REJECTED TLS handshake on the SERVER leaked resources per connection — a Winsock handle,
an `SSL`, an `SSL_CTX` holding the RSA-4096 cert/key, the object's buffers, and an un-removed
event-handler entry. The rejected socket sat in `ClientListener::m_clientSockets` with no
`SocketDisconnected` subscriber, because the `PacketStreamFilter`/`ClientProxyUnknown` scaffolding is
only built in `handleClientAccepted`, which a rejected handshake never reaches; `SocketMultiplexer`
deletes only the JOB, and `ArchSocket` refcounting means `closesocket()` is never called. Pre-auth
reachable — `verifyIgnoreCertCallback` returns 1 unconditionally, so any self-signed cert completes the
TLS handshake and the fingerprint pin (the only gate) runs AFTER accept + insertion — so a slow flood of
untrusted-cert connections exhausts the server. The CLIENT role never leaked: `Client::connect` adopts a
`PacketStreamFilter` and subscribes `SocketDisconnected`, so its chain unwinds; the server's pre-accept
path had none of that scaffolding. Present verbatim through v1.26.0 and our fork HEAD.

**Fix: cherry-picked upstream's own two commits** (they postdate v1.26.0 and, crucially, landed BEFORE
upstream's breaking log refactor, so they lift out with no dependency on it):
- `220bf3178` — "correctly deletes sockets that failed to become clients": adds an additive event
  `ClientListenerDisconnectedOnAccept`, fired by `SecureSocket::serviceAccept` on a fatal accept
  (`status < 0`), handled in `ClientListener::handleClientConnecting` by a new `removeClientSocket()`
  (erase from `m_clientSockets` + `removeHandlers(getEventTarget())` + `delete`).
- `d392547fd` — "ClientProxyUnknownFailure should also remove its client socket": covers the sibling
  case where the unknown-client handshake fails, deleting the underlying socket when the stream filter
  did not adopt it (`StreamFilter::adoptedStream()`).

Applied clean onto our fork: SEC-08b touched `secureAccept` (not `serviceAccept`), and none of our other
patches touch `ClientListener`. Verify: the server's open-handle count stays FLAT under a flood of
fingerprint-rejected connections (valid TLS, untrusted cert — the unthrottled reject path, not garbage
bytes).

## MT clipboard lane — a dedicated connection for clipboard data (2026-08-06)

**New files:** `src/lib/net/LaneConn.{h,cpp}`, `src/lib/deskflow/ClipboardLane.{h,cpp}`,
`src/lib/server/ClientProxy1_9.{h,cpp}`, `src/lib/client/ClipboardLaneDialer.{h,cpp}`.
**Modified:** `src/lib/deskflow/ProtocolTypes.{h,cpp}`, `src/lib/base/EventTypes.h`,
`src/lib/net/{SecureSocket,TCPSocket}.{h,cpp}`, `src/lib/deskflow/PacketStreamFilter.{h,cpp}`,
`src/lib/server/{Server,ClientProxy1_6,ClientProxyUnknown,ClientListener}.{h,cpp}`,
`src/lib/client/{Client,ServerProxy}.{h,cpp}`.
**Design record:** `MT-CLIPBOARD-LANE-DESIGN.md` in the repository root.

**The problem.** A large clipboard (a 1080p screenshot marshals to ~8.3 MB, and there are two
clipboard ids) queued onto the main connection sits in front of the keepalives, which are FIFO
behind it. The client's death timer resets only on `kMsgCKeepAlive`, so after 9 s it declares the
server dead, disconnects (the cursor teleports off that screen), reconnects with `m_dirty` true by
default, and the next screen switch re-sends the same blob from byte zero — a permanent wall-off
loop, observed for 56 cycles on 2026-08-06. Synergy fixed this class in 1.8 with per-chunk
interleaved keepalives; Deskflow deleted that in `5365e34f0` and our base v1.26.0 contains the
deletion.

**The change.** Clipboard data moves on its OWN TLS connection to the same port 24800, serviced by
its own low-priority worker thread, entirely outside the `SocketMultiplexer`. This **completely
replaces** the in-stream transfer server→client: `kMsgDClipboard` trains are never SENT by a new
core, though they are still PARSED so older peers keep working. A lane failure can only ever mean
"clipboard not delivered" — it must never write to, block, or disconnect the main session.

- **Negotiation.** Protocol minor 1.8 → 1.9. The server sends `kMsgDLaneAdvert` ("DLAN", lane wire
  version + a 16-byte CSPRNG token) only from `ClientProxy1_9`, the proxy it builds only for a
  client that announced 1.9 — a type rather than a version check, because an unknown 4-char code
  makes a Deskflow client drain its whole stream and die, so this must be impossible by
  construction rather than by condition. The client greets on the lane connection with
  `kMsgMTLaneHello` ("MTLH") where a `kMsgHelloBack` would go.
- **Two clamps ship with the bump and must never be separated from it.** Minor-version tolerance in
  Deskflow is one-directional: a server accepts any minor it has a proxy for, but a client
  announcing a HIGHER minor is REFUSED (`ClientProxyUnknown::initProxy`'s null default →
  `IncompatibleClientException`), and stock `Client::handleHello` announces its own constant
  unconditionally. Unclamped, the first machine of a rolling deploy could not connect to a
  not-yet-updated server AT ALL. So `Client::handleHello` now announces `min(ours, the server's)`,
  and `initProxy`'s default negotiates an unknown-but-higher minor DOWN to **`ClientProxy1_8`** — the
  newest dialect that is safe to speak to a peer we have not identified. Not 1_9: a client of this
  fork clamps, so it can never announce more than 9 and always lands on `case 9`; anything reaching
  the default is therefore a build that does NOT clamp (i.e. not ours), and 1_9's entire reason to
  exist is to send `kMsgDLaneAdvert`, which such a peer would treat as an unknown code and die on.
- **Socket handoff.** `SecureSocket::detachTls()` leaves the multiplexer, then hands over the socket
  handle, `SSL` and `SSL_CTX` and empties itself, so its `close()`/destructor become no-ops. It
  REFUSES a connection that is not quiescent — a latched partial `SSL_write`, or unwritten OUTPUT —
  because a detach that stranded a half-written record would corrupt the stream exactly as the
  2026-07-28 outage did, and a **refusal restores the multiplexer job**, so declining never leaves a
  live socket stranded out of the poll set. Unread INPUT is not a refusal: it is drained into
  `DetachedTls::pending` and served by `LaneConn::readSome()` before anything is taken from the
  `SSL` object (see the review note below). `TCPSocket::releaseSocket()` and
  `PacketStreamFilter::hasBufferedInput()` exist for this (`getSize()` reports 0 both for "empty"
  and for "half a packet", and only one of those is safe to hand off).
- **Threading.** ONE worker thread per lane owns its `SSL` for the connection's whole life, driving
  both directions from a `select()` loop on a non-blocking socket. A reader/writer pair sharing a
  per-lane SSL lock was rejected: it deadlocks whenever both endpoints send a large clipboard at
  once, because progress then requires the reader to run WHILE the writer is blocked inside
  `SSL_write`. Dropping the lock is not available — OpenSSL forbids two threads in one `SSL`.
  Workers demote themselves to background scheduling before any IO (Windows
  `THREAD_MODE_BACKGROUND_BEGIN`, which is the only knob that works inside this core's REALTIME
  priority class; per-thread `nice` on Linux; an honest no-op elsewhere). Teardown is always: raise
  the stop flag, `shutdown()` the socket, join, then destroy. Nothing is detached, and the worker
  catches every exception, because one escaping a thread entry function would call `std::terminate`.
- **No statics anywhere in the new code.** That is aimed directly at the 2026-07-29 class of bug
  (`4e2987ff7`, `b8d844715`).
- **Latest-wins, and retained across a missing lane.** At most ONE payload is pending per (peer,
  clipboard id); a newer copy replaces an undelivered older one, and a payload arriving mid-train
  makes the sender abandon that train between chunks (the receiver discards its partial assembly
  when the new START lands). A payload handed over while the peer has a SESSION but no live lane is
  held on the session (`Session::m_held`) and seeded into the next lane by `attach()`, because both
  callers clear their dirty flag at the moment they call `send()` and nothing ever re-offers an
  unchanged clipboard — so dropping there loses it rather than delaying it. Memory is bounded by
  construction (one payload per clipboard id per peer, and only peers that speak the lane protocol
  have a session at all), and this is what kills the reconnect-and-resend-from-zero pathology: a
  lane that comes up late delivers current state, not a backlog.
- **Authorisation.** A lane is accepted only for a live session name, with a constant-time token
  match, and only if the lane connection's peer TLS fingerprint EQUALS the main session's. (Without
  peer authentication both fingerprints are empty and the token alone gates — vacuous by
  construction rather than by omission.) Every refusal is a quiet DEBUG line naming which check
  failed. A token is never logged.
- **The dial (client side).** On `kMsgDLaneAdvert` the client opens a second TLS connection to the
  same listener, reads and DISCARDS the `kMsgHello` the listener greets every connection with (a
  HelloBack would make it a second KVM client), writes `kMsgMTLaneHello` and nothing else, waits for
  `StreamOutputFlushed`, then detaches and attaches. It is an event-driven state machine on the
  existing event queue + multiplexer, not a routine: everything slow (connect, TLS, both waits)
  happens on the multiplexer's service thread exactly as it does for the main connection, and what
  runs on the event-queue thread — which is also the thread that puts relayed input on this screen —
  is only the short callbacks in between. For the same reason it waits for the flush EVENT rather
  than calling `IStream::flush()`, which would park the input thread on a condition variable, and it
  copies an already-resolved address so a dial never performs DNS. A failed dial retries on a
  doubling backoff (3 s → 30 s, both existing protocol constants) and touches nothing else; the
  dialer holds no reference to the main connection.
- **A lane is SILENT from attach until someone has a clipboard.** The first draft had each worker
  greet its peer with a `LaneFrame::Ack`. That breaks the handoff it was meant to confirm, in both
  directions: the handoff is only legal while the connection is quiescent, so an ACK from the
  dialling end can land in the accepting end's packet filter ("pipelined data behind its greeting")
  and an ACK from the accepting end can land in the dialling end's input buffer before it detaches
  (`detachTls()` then refuses, correctly). Intermittent, load-dependent, self-healing after a
  dropped clipboard — the worst possible presentation. The frame type stays reserved and unknown
  types are skipped by length, so a greeting can return once there is a safe moment for it.
- **The application clipboard sequence number is carried end to end**, separately from the lane's
  own train counter. `Server::onClipboardChanged` drops an update whose sequence number is below the
  last it saw, and a per-lane counter starting at 1 would have lost that comparison for the rest of
  the session — every clipboard a client copied would have been silently discarded as
  "mis-sequenced".
- **Removed:** the `ClipboardSending` handler registration in BOTH `ClientProxy1_6` and
  `ServerProxy`, and both `StreamChunker::sendClipboard` calls — which retires the documented
  handler-leak concern along with the path it served (neither destructor ever removed that handler).
  `StreamChunker` and `ClipboardChunk::send` are now unreferenced but are LEFT IN PLACE: deleting
  them reaches across three libraries for no behavioural gain, and `ClipboardChunk::assemble` next
  door is still what both legacy receive paths use.
  **Kept, deliberately:** `ClientProxy1_6::recvClipboard`, `ServerProxy::setClipboard`, and every
  send DECISION gate (the ownership marker, the `size <= 4` empty skip, the size cap, the dirty and
  sent/unchanged bookkeeping). Only the transport changed.

> ⚠️ **A lane requires TLS, so a PLAINTEXT deployment now has no clipboard sync in either
> direction.** The handoff needs an `SSL` object to give the worker and the server refuses to detach
> anything else, so a `PlainText` client does not dial at all. Acceptable here — MouseTransfer always
> runs `PeerAuth` — but it is a behaviour change for a stock unencrypted Deskflow setup.

### Adversarial review pass (2026-08-06) — seven defects found by reading, all fixed

Recorded because each one was invisible to a compiler and to a happy-path run, and three of them
would have presented as "clipboard sync intermittently doesn't work", which is the failure mode this
project has historically spent whole sessions on.

1. **A clipboard handed over before the lane came up was DROPPED, not retained.** `send()` queued
   only onto a live lane; with a session but no lane the payload was destroyed and `NoLane`
   returned. Both callers clear their dirty flag at that moment and nothing re-offers an unchanged
   clipboard (`Server` marks a client dirty again only when the clipboard CHANGES), so the payload
   was lost until the user copied something else. The window is the one the feature exists for:
   connect → advert → dial (TCP + a 4096-bit TLS handshake) is comfortably long enough for
   `switchScreen()`'s leave-time push to land in it, and a lane outage opens it again for a whole
   retry backoff. Fixed with `Session::m_held` + a new `SendResult::Held`, which also splits the
   diagnostics: `NoLane` now means only "this peer is older than 1.9".
2. **The lane session key and the lane greeting used different name spaces.** Sessions are opened,
   torn down and looked up under `Server::getName()` — `Config::getCanonicalName`, a CASELESS lookup
   that also resolves aliases — while `ClientProxyUnknown::handleLaneHello` matched the name the
   client announced about itself and `ClientProxy1_6::setClipboard` sent under the proxy's own. Any
   screen the configuration spells differently, or knows by an alias, would have had every lane
   refused ("no live session with that screen name", at DEBUG) and clipboard sync dead in both
   directions with nothing else wrong. `Server::canonicalName()` is now used at both.
3. **`initProxy`'s permissive default handed a stranger a `ClientProxy1_9`** and would then advertise
   the lane at it — an unknown 4-char code, which makes a Deskflow client drain its whole stream and
   die. The branch written to save an unidentified peer's KVM link would have killed it. Negotiates
   down to 1_8 now; see the bullet above.
4. **A refused `detachTls()` left a live socket out of the multiplexer.** It calls `setJob(nullptr)`
   before its preconditions — correctly, that is what makes the buffer checks race-free — but then
   returned "no" having silently stopped servicing a healthy connection. Both callers happen to
   close afterwards, so it was correct only by what every current caller does next. A refusal now
   re-arms the job.
5. **Unread input was a refusal, and silence does not remove the race it covers.** The peer may
   legitimately speak as soon as IT has detached, which can be before this end has run the
   event-queue hop that gets it to `detachTls()` — a LAN round trip beats a queued event on a busy
   client. Fix 1 makes it *more* likely by giving a fresh lane something to send immediately. Those
   bytes are now carried across the handoff instead of dropping the connection.
6. **`LaneConn::m_dead` was a plain `bool` written cross-thread.** `shutdown()` is the documented
   any-thread teardown call and the owning worker reads that flag in its loop condition and every
   IO call: a data race, and on a weakly-ordered target (this fork cross-builds for ARMv6) a worker
   really can fail to observe it. Now `std::atomic<bool>`.
7. **`waitReady()` used `select()` on POSIX**, where an `fd_set` is a bitmap indexed by descriptor
   number — `FD_SET` on a descriptor ≥ `FD_SETSIZE` (1024) writes past the end of the object. A lane
   is by definition the second connection a peer opens. POSIX uses `poll()` now; Windows keeps
   `select()`, which is correct there.

Also fixed: the "no lane" warning was rate-limited against `ARCH->time()` (a steady_clock reading,
i.e. seconds since BOOT) compared with a `0.0` initialiser, so on a machine that had just started —
which is how this fleet starts — the FIRST warning was suppressed. An explicit flag replaces the
sentinel arithmetic on both ends.

**Verified-and-refuted, so nobody re-derives them:** deleting the dialer's stream and socket from
inside the packet filter's own `filterEvent` dispatch chain is the same shape as stock
`Client::handleDisconnected → cleanupStream()` and is safe — `StreamFilter::filterEvent` and
`PacketStreamFilter::filterEvent` touch no member after the nested `dispatchEvent`, the filter's
mutex is released before it, and every dialer handler is a single tail call, which is what makes
erasing the currently-executing `std::function` survivable. `nudge()` cannot open a duplicate lane
(both call sites gate on "no lane", and `attach()` supersedes rather than accumulates). The START
frame layout matches `kLaneStartBodySize` exactly and `kLaneMaxFrameBody` has 55 bytes of slack over
the largest legal body. `Client::enter()`'s lane check is a one-entry map lookup plus, at worst, a
join of a thread that has already returned — no worker holds a lock during IO, and no worker ever
touches the session map, so there is no path by which one can block the input thread.

### Stage 5 — operability and the copy account (2026-08-06)

Rig stage 4 passed functionally and produced three deployment blockers: a 64 MiB publish took the
server core from 3 MB to 517 MB private bytes (~8×, settling ~388 MB), the server froze once for
~3 minutes CPU-bound and silent, and a client froze once for ~90 s with its whole 2-vCPU guest
pinned. Everything ≤48 MiB was clean in every run. Full write-up, with the numbers, in
`MT-CLIPBOARD-LANE-DESIGN.md` §7 (as built), §11 and §12.

**Diagnostics, because the design's §7 was operationally wrong.** It put the lane's whole lifecycle
at `LOG_DEBUG`. Fleet cores run at the default INFO level and the wrapper regenerates
`settings\Deskflow.conf` on every engine start and core respawn without ever writing `log/level` — so
DEBUG cannot be turned on for the machine that is misbehaving, and nothing below INFO survives to be
read afterwards. Both rig stalls had their entire evidence trail in lines a fleet log would not carry.
Promoted to INFO, and no further: lane up (peer + socket), lane down **with the reason**, one
completion line per transfer on each of the sending and receiving workers (bytes + elapsed), and
"clipboard is waiting for the lane". A transfer that never finishes is then diagnosed by the absence
of a completion line between an `up` and a `down`. The down reason is recorded state, not a guess:
`LaneConn::markDead()` publishes a string literal through an atomic, first writer wins, so the real
cause survives the teardown that follows it.

The `clipdiag` gating the design promised was never wired — `ClipboardChunk::diagEnabled()` appeared
nowhere in lane code. Per-chunk lines now sit behind `mtDiagEnabled()` (the same `clipdiag` sentinel
FILE, chosen because an elevated core's environment block is frozen at task-registration time), read
once per worker so the gate costs nothing per chunk. They include `SSL_pending()` after every
reassembled chunk — the probe §9b left for the rig.

A REFUSED thread demotion is now WARN rather than `LOG_DEBUG1`. `deskflow-core` runs in
`REALTIME_PRIORITY_CLASS` (`AppUtilWindows.cpp:112` → `Thread::setPriority(-14)` → entry 22 of
`ArchMultithreadWindows`'s table), and an undemoted worker streaming tens of megabytes there competes
directly with the input relay the lane exists to protect. A standalone probe confirmed background
mode works and pins a thread to absolute priority 4 under NORMAL and HIGH classes; REALTIME could not
be tested without `SeIncreaseBasePriorityPrivilege`, so the WARN is what will settle it on the fleet.

**The 8× is per-consumer copying, not a leak and not a quadratic.** Counted statically for a server
with two clients: the payload was marshalled once by the server and then again, byte for byte, inside
every `ClientProxy1_6::setClipboard`; each proxy kept a `Clipboard` mirror AND handed the lane its own
private string; `IClipboard::copy` and `unmarshall` each took a whole extra copy because `add()` could
only copy; `marshall()` held every format's temporary until it returned; and `switchScreen`
re-marshalled both clipboard ids on every seam crossing purely to size-check them. Model: peak 8N,
settle 6N — 537 MB and 402 MB at 64 MiB, against 517 MB and 388 MB measured.

Fixed by one immutable `shared_ptr<const std::string>` per clipboard change, shared through every
proxy, both queues and every worker; a cached marshalling on `Server::ClipboardInfo` that is assigned
everywhere `m_clipboard` is (including the over-limit early return, so "current" is an invariant);
`IClipboard::add(Format, std::string&&)` so the two copy paths move; a single marshall on the client's
send path instead of two; `reserve()` of the announced total in the lane's reassembly; and `DATA`
frames built straight into the send buffer. Model after: peak ~4N, settle ~4N, and the steady state
now grows by 1N per client instead of 2N. Per-crossing cost 268 MB → 0.

**The stalls remain UNEXPLAINED, and this says so rather than claiming them.** 170 and 90 CPU-seconds
are orders of magnitude beyond any linear pass over 64 MiB, and there is no O(n²) anywhere on the
path: marshall and unmarshall reserve exactly and walk once, the reassembly grew geometrically
(amortised linear), the Unicode converters all reserve up front, and `ProtocolUtil`/`StreamChunker`
are not on the payload path at all. §11.3 names the three surviving candidates — an undemoted worker,
`SSL_write` returning `WANT_READ` and spinning, and the entirely unlogged Windows clipboard WRITE path
on the main thread — each with the instrument that now settles it.

**Cap: the operational number is analysis only; the fork-side ceiling is IMPLEMENTED.** The binding constraint after these fixes is the RECEIVING
endpoint (~7N, because the Windows clipboard write path is untouched), not the server (~4N). §12
recommends 32 MB for the wrapper's emitted `clipboardSharingSize` plus a fork-side absolute clamp at
128 MiB — the latter because `m_maximumClipboardSize` defaults to `INT_MAX` when the option is absent,
and the new `reserve(total)` makes that default the literal size of an allocation the receiver will
attempt from a peer-supplied length.
#### The fork-side payload ceiling (stage 5, follow-up)

`kLaneMaxPayloadBytes = 128 MiB` in `ProtocolTypes.h`, beside the other lane constants and with the
§12 arithmetic recorded as its justification: 4x the recommended 32 MB operational knob, bounded by
the worst-endpoint 7N account, so the worst it permits is ~900 MB rather than the 4 GB a `uint32`
`totalLen` could otherwise ask for.

Applied in ONE place — `ClipboardLaneManager::setMaxPayloadBytes()` and the constructor take
`min(configured, kLaneMaxPayloadBytes)` — so the wrapper-emitted `clipboardSharingSize` still governs
exactly as it does today right up to the ceiling, and no other site has to remember the ceiling
exists. A configured value above it is reported once at NOTE when it is set, because a transfer later
refused for a size the configuration appears to allow is otherwise baffling.

Receive is two-tiered, and the tiers mean different things. A START announcing more than the CEILING
drops the lane, before `reserve()` allocates anything — no conforming sender can emit that, since
every sender applies the same ceiling, so it is a broken or hostile peer rather than a configuration
disagreement. Its reason carries the numbers and appears on the INFO "clipboard lane down" line, via
a new `Lane::m_downDetail` written by the worker and read after the join (`LaneConn::deadReason()` is
an `atomic<const char *>` of literals, which is what makes it any-thread readable and also what stops
it carrying a size). A START over the CONFIGURED limit but inside the ceiling behaves exactly as
before: the train is refused at DEBUG and the lane stays up.

Send needed no new gate — `send()` already tested against `m_maxPayloadBytes`, which is now the
clamped value, so a misconfigured sender gets `TooLarge` and skips with a NOTE having dialled nothing
and queued nothing. Both call sites now print the limit IN FORCE instead of calling it "the
configured limit", since those are different numbers precisely when the ceiling is what bit.

**No emitted `clipboardSharingSize` and no wrapper file was touched.** The operational knob (§12
recommendation 1, 32 MB) remains the owner's decision.

## RDP window handoff: hold an exported window foreground during viewer interaction (2026-08-09)

`src/lib/platform/MSWindowsDesks.{h,cpp}`. The MouseTransfer wrapper exports a window over RDP to a
peer (its "RDP window handoff" feature) and delivers the viewer's typing as `SendInput` tagged with
the local-inject signature — real input, which the OS routes to the FOREGROUND thread's focus
window. But while the shared cursor is away, `deskLeave` has made this core's own `DeskflowDesk`
hider the foreground window (primary + low-level hooks), and the non-elevated wrapper cannot take
foreground back from a LocalSystem+UIAccess-owned window: UIPI refuses the steal, so the viewer's
keystrokes landed on the wrong window and typing appeared dead. Only this process can reliably
reassign its own foreground, so it does.

Mechanism (mirrors the switchreq file poll): the wrapper writes `<hwnd> <pid>` (both decimal; pid =
the wrapper's own) to a signal file — `MOUSETRANSFER_RDPFGFILE`, else `rdp-fg-hold` relative to the
CWD (the bundle dir) — while a viewer is interacting with that export, and deletes it when the
episode ends. `checkRdpFgHold()`, piggybacked on the existing 0.2 s desk-check timer, honours the
hold only while (a) the cursor is away (`m_isOnScreen` false), (b) the named window is alive, and
(c) the WRITER process is alive (a crashed wrapper must not leave its export pinned). While all
three hold, the desk thread (`deskRdpFgHold`, `DESKFLOW_MSG_RDP_FG_HOLD`) keeps that window
foreground with deskLeave's own AttachThreadInput recipe, RE-ASSERTING each tick — a hold, not a
one-shot. On release while still away (primary + low-level), the `DeskflowDesk` hider is put back
in charge, leaving `desk->m_foregroundWindow` untouched so `deskEnter` still restores the window
the user actually had foreground. `deskLeave` additionally refuses to save a held window as the
foreground-to-restore (it is invisible; restoring it at enter would strand the user typing into
nothing).

No behaviour changes when the signal file does not exist — one `ifstream` open per 0.2 s tick while
the cursor is away is the whole steady-state cost.

### Tagged injected moves keep the relay's saved cursor position honest (2026-08-09)

**Files:** `src/lib/platform/MSWindowsHook.h`, `MSWindowsHook.cpp`, `MSWindowsScreen.cpp`

The local-inject passthrough (the first modification in this file) delivers a tagged event to the
local OS and hides it from the relay — but a tagged MOUSE MOVE still physically displaces the
cursor, and `MSWindowsScreen::onMouseMove` computes every relayed hardware delta against its saved
"last known position" (`m_xCursor`/`m_yCursor`), which the hidden move left stale. The next real
motion therefore relayed a delta that INCLUDED the injected displacement — the client's cursor
jumped by (injected position − saved position). Harmless for the passthrough's original one-shot
uses; fatal for the RDP window handoff's real input, where a viewer-driven drag is a real button
HELD at a real position across many relayed motions (a Save As scrollbar drag teleported the
viewer's own cursor on its first movement).

Fix, pure bookkeeping: `mouseLLHook` now posts a new `DESKFLOW_MSG_INJECT_AT` (x; y) to the screen
thread for every tagged move it passes through, and `onPreDispatchPrimary` answers it with
`saveMousePosition` alone — nothing is relayed, nothing is warped. The new message sits INSIDE the
`DESKFLOW_MSG_INPUT_FIRST..LAST` range on purpose, so `warpCursor()`'s discard loop flushes a stale
one exactly like any other queued input message (the warp re-saves the true position itself);
`SCREEN_SAVER` and `DEBUG` shift up one slot, which is safe because every id is symbolic and
in-process only (`MSWindowsDesks` derives its ids from `DESKFLOW_HOOK_LAST_MSG` and follows).

No behaviour changes for untagged input, for clients (the message is only posted by the primary's
installed hook), or when nothing injects tagged moves.

### Jump diagnostics: name the reason when a relayed delta teleports the client cursor (2026-08-09)

**Files:** `src/lib/platform/MSWindowsScreen.h`, `MSWindowsScreen.cpp`

Diagnostic only — no relayed behaviour changes. `onMouseMove` now keeps a small ring of the events
that touch the delta reference (H = hardware motion processed, I = tagged-injection bookkeeping,
W/C = the two warp saves, B = a bogus-zone drop) and, whenever a single relayed motion's delta
exceeds 128 px on either axis (beyond any per-event mouse report; the class users see as the
client's cursor "teleporting"), logs `MT-jumpdiag:` at INFO with the event, the saved reference it
was computed against, and the ring with millisecond ages — enough to see which bookkeeping step
went missing or stale. The delta is still relayed unchanged; the log is the whole feature.

### Suppress the per-motion warp while a tagged injection stream owns the cursor (2026-08-09)

**Files:** `src/lib/platform/MSWindowsScreen.h`, `MSWindowsScreen.cpp`

The measured cause of the RDP handoff's remaining client-cursor teleports (MT-jumpdiag, same day):
during a viewer-driven real drag the shadow server re-positions the cursor along the drag path with
tagged injections every few ms, while `onMouseMove` warps it back to the screen centre after every
relayed hardware motion. Two writers oscillate the cursor centre↔drag-position through the OS input
pipeline, hardware events get stamped against whichever writer won at their instant, and deltas of
(drag position − centre) scale escape the bogus-zone filter and reach the client as teleports —
the client's recorded jumps matched the leaked deltas to the pixel.

`onMouseMove` now skips the warp while a tagged injection stream is live: within 100 ms of the last
`DESKFLOW_MSG_INJECT_AT`, extended to 5 s while a real mouse button is held (the drag), read with
`GetAsyncKeyState` (cheap userland call, per relayed motion, no shell-out). With the warp silent,
every reference stays in the drag neighbourhood, so an interleaving error is bounded by one
injection step (a few px) rather than half a screen. The stream re-centres the cursor itself when
it ends (the button-up's batch snap-homes with a tagged move), and the suppression cannot stick —
both bounds expire on their own. Relaying itself is unchanged; only the warp is gated.

### Re-anchor the saved position during an injection stream (2026-08-09, completes the previous fix)

**Files:** `src/lib/platform/MSWindowsScreen.h`, `MSWindowsScreen.cpp`

The warp suppression above stopped the teleports and immediately revealed what else the warp had
been doing: the RELAY-mode hook EATS every hardware motion (the physical cursor never moves from
hardware while relaying — each event's pt is just current-cursor + that event's own delta), and the
per-motion warp was what re-synced the saved position to the actual cursor so successive deltas
came out right. With the warp suppressed, the first motion after an injection relayed d1 correctly
but left `saved` advanced to (anchor + d1) while the cursor stayed at the anchor — so the next
motion relayed d2 − d1, and a steady drag cancelled itself to a crawl (owner: "a lot of resistance
to movement"; measured ~77 px/s tracking during a fast drag).

Fix: while the injection stream is live, after relaying each motion, `saveMousePosition` back to
the LAST INJECTED point — where the eaten-events cursor actually is — so every hardware delta
relays in full. Ordering is exact because `DESKFLOW_MSG_INJECT_AT` and `DESKFLOW_MSG_MOUSE_MOVE`
arrive in hook order: the anchor at processing time is always the injection the cursor sat at when
the motion was stamped. Non-injection relaying is untouched (warp + its own save, as always).

### INJECT_AT saves the clamped truth, not the request (2026-08-09)

**File:** `src/lib/platform/MSWindowsScreen.cpp`

An absolute `SendInput` can name a point beyond the screen edge (an RDP-resized window can hang
off-screen and the viewer aims at its off-screen pixels), but the OS pins the real cursor at the
edge. The INJECT_AT bookkeeping saved the unclamped request, so every following hardware delta
included the difference (measured: injections aimed at x=2143 on a 1920-wide screen produced
−223 px client-cursor teleports). The handler now clamps the saved position to the screen bounds
the bogus filter already reads.

### Drop mis-stamped hardware motions during an injection stream (2026-08-09)

**File:** `src/lib/platform/MSWindowsScreen.cpp`

SendInput's non-interleaving promise does not extend to how a concurrent hardware event gets its
position stamped: a wheel batch (move to the point → wheel → snap home) can have a hardware motion
stamped AT the wheel point yet delivered AFTER the snap-home's INJECT_AT, so its delta against the
home anchor is the whole injected displacement (measured: −845 relayed; drags never showed it
because their injections track the cursor within pixels). During an injection stream an honest
per-event hand delta is small, so a delta of displacement scale is provably mis-stamped — it is
dropped (ring kind 'B'), and the re-anchor has already restored the truth for the next event. Cost:
one lost hand-motion event during a batch, instead of a screen-scale client-cursor jump.

### RDP window handoff: adaptive cursor park (2026-08-11)

**Files:** `src/lib/platform/MSWindowsDesks.{h,cpp}`, `src/lib/platform/MSWindowsScreen.{h,cpp}`

While the shared cursor is away on a client, the primary parks its own cursor at the screen centre
(`m_xCenter/m_yCenter`) and relays hand motion as deltas measured from there (the low-level hook
eats hardware motion, so the physical cursor never budges — see `onMouseMove`). During an RDP window
handoff that centre sits **on top of** the window being streamed to the far side, so the streamed
source cursor is drawn over the content and its hover highlights the wrong element.

The wrapper now publishes an off-window rest point in a sibling signal file, `rdp-park`
(`"<x> <y> <pid>"`, decimal, same liveness rule as `rdp-fg-hold`). `MSWindowsDesks::checkRdpPark`
polls it on the existing 0.2 s `handleCheckDesk` cadence — only while the cursor is away and the
writer is alive — into one lock-free atomic (`getRdpPark`). `MSWindowsScreen::rdpWarpCentre`
substitutes that park for the screen centre in `leave()` and in `onMouseMove`'s warp-and-recentre,
**and the bogus-delta guard uses the same point**, so the usable-delta room is measured from
wherever the cursor actually rests. With no park published (any non-handoff moment) behaviour is
byte-for-byte the stock centre-warp. The wrapper keeps the park off every window it streams and with
enough edge margin that a single hand-motion delta never clamps or trips the bogus guard.
