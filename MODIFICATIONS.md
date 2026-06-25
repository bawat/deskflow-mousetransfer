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

## Building

Standard upstream build (see `doc/dev/build.md`). On Windows: CMake + Ninja +
Qt 6.7+ + OpenSSL (via vcpkg). `deskflow-core` is the only target the wrapper
needs.
