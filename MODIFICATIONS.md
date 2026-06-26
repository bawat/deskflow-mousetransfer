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

## Building

Standard upstream build (see `doc/dev/build.md`). On Windows: CMake + Ninja +
Qt 6.7+ + OpenSSL (via vcpkg). `deskflow-core` is the only target the wrapper
needs.
