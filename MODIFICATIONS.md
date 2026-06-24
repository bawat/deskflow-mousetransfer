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

### Client egress interface bind (2026-06-24)

**File:** `src/lib/net/TCPSocket.cpp`

Extended the existing `core/interface` setting so it also pins the **client's
outbound** connection to a chosen local adapter.

- Upstream honours `core/interface` only on the **server**, as the address its
  listen socket binds to (`ServerApp::parseArgs`). The **client** had no
  local-bind option — its outbound socket's source address followed OS routing.
- In `TCPSocket::connect()`, when `core/interface` is non-empty, the socket is
  bound to that address (with an ephemeral local port) before `connectSocket`.
  Only the client calls `connect()` (the server accepts already-connected sockets
  and listens via `TCPListenSocket`), so this affects *only* a client's egress and
  never the server.
- Address-family match: Deskflow's sockets are dual-stack (`newSocket` clears
  `IPV6_V6ONLY`), so the socket family equals the destination's. A literal IPv4
  interface resolves to `AF_INET`, but the socket is frequently `AF_INET6`, and a
  mismatched `bind()` fails with `WSAEINVAL`. So the patch binds an address of the
  SAME family as the connect target — trying the plain interface IP first, then its
  IPv4-mapped IPv6 form (`::ffff:<ip>`).
- A bind failure is logged and ignored (connection proceeds via the default
  route) so a stale/incorrect selection can't break connectivity.

Stock Deskflow leaves `core/interface` empty, so existing behaviour is unchanged.
This lets the "MouseTransfer" wrapper confine a client's KVM traffic to a
user-selected network adapter, symmetrically with the server's listen bind — set
purely via the config file the wrapper already writes; no source linkage.

## Building

Standard upstream build (see `doc/dev/build.md`). On Windows: CMake + Ninja +
Qt 6.7+ + OpenSSL (via vcpkg). `deskflow-core` is the only target the wrapper
needs.
