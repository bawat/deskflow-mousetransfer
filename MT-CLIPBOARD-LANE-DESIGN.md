# MT Clipboard Lane — design & implementation brief (2026-08-06)

Owner decisions (locked): the fix lives IN THE FORK (not the wrapper); clipboard data moves on a
DEDICATED connection serviced OFF the realtime path; this COMPLETELY REPLACES the in-stream
clipboard data transfer (`DCLP` trains on the main connection are never SENT by a new core; they
are still PARSED for old peers). Same port 24800 (firewall-neutral). No wrapper changes.

## 1. Problem being solved

A large clipboard (a 1080p screenshot marshals to ~8.3 MB × 2 clipboard ids) queued onto the main
connection starves the client's keepalive death timer (9 s, resets ONLY on `kMsgCKeepAlive`;
kAlives are FIFO behind the blob), the client declares "server is dead", disconnects (cursor
teleports off the peer), reconnects with `m_dirty = true` default, and the next screen switch
re-sends from byte zero — a permanent wall-off loop (2026-08-06 FANTASY incident; 56 cycles).
Upstream history: Synergy fixed the starvation in 1.8 with per-chunk interleaved keepalives;
Deskflow deleted that accidentally in `5365e34f0`; our base v1.26.0 contains the deletion.
Related history in this fork: the 2026-07-29 SecureSocket function-local-statics storm
(`4e2987ff7`, `b8d844715`) — clipboard bursts through the multiplexer write path are the
established danger zone. The lane avoids that machinery entirely.

## 2. Architecture

- The CLIENT opens a SECOND TLS connection to the server's existing 24800 listener.
- After TLS, instead of a HelloBack it sends a **lane-hello**: magic + protocol version +
  client screen name + a 16-byte per-session token the server issued on the MAIN connection.
- The server validates (live session for that name; token match, constant-time; the lane
  connection's peer TLS fingerprint EQUALS the main session's peer fingerprint), replies
  **lane-ack**, then **detaches the socket from the SocketMultiplexer** and hands fd+SSL to the
  lane workers. Reject ⇒ close quietly (log at DEBUG; no effect on the main session).
- Lane IO runs on **ONE dedicated worker thread per lane** — see §6.1 for why one and not a
  reader/writer pair — which demotes itself at start: Windows `THREAD_MODE_BACKGROUND_BEGIN` (this
  escapes the REALTIME process class's floor — relative thread priorities do not), POSIX equivalent
  (per-thread nice) behind a small OS seam. No multiplexer jobs, no WANT_WRITE retry state shared
  with anything, no buffers shared between sockets.
- Full duplex: both endpoints send clipboard trains on the one lane connection.
- The lane is persistent once up; reconnects lazily (client re-dials on demand with backoff);
  **a lane failure must NEVER disconnect or write to the main session** — worst case is a logged
  "clipboard not delivered".

## 3. Negotiation (mixed-mesh safety — VERIFIED 2026-08-06, mechanism CHOSEN)

### 3.1 What was verified in the v1.26.0 code (read, not assumed)

| question | answer | where |
|---|---|---|
| Client announcing a **lower** minor than the server | Accepted. `ClientProxyUnknown::initProxy` switches on the minor and builds the matching `ClientProxy1_N`, so the server simply speaks the older dialect. | `server/ClientProxyUnknown.cpp:146-195` |
| Client announcing a **higher** minor than the server knows | **REJECTED OUTRIGHT.** `initProxy`'s `switch` ends `case 8: … default: break;`, leaving `m_proxy == nullptr`, which throws `IncompatibleClientException` ⇒ `kMsgEIncompatible` ⇒ the client calls `refuseConnection`. Not "degraded" — **no KVM link at all.** | `ClientProxyUnknown.cpp:186-194, 237-240`; `client/ServerProxy.cpp:166-173` |
| Does the client cap its announced version to the server's? | **No.** `Client::handleHello` reads the server's major/minor and then unconditionally writes `kProtocolMajorVersion`/`kProtocolMinorVersion` — its OWN constants. The server's numbers are read and discarded. | `client/Client.cpp:570-638` |
| Unknown 4-char message code, client side | Fatal. `ServerProxy::handleData` → `Unknown` ⇒ `LOG_ERR("invalid message from server")` and then **`while (m_stream->read(nullptr, 4));` — it drains the entire stream** because message boundaries are unknowable. The connection is finished. | `client/ServerProxy.cpp:99-105, 193-195, 313-315` |
| Unknown `kMsgDSetOptions` option id, client side | **Silently ignored.** `ServerProxy::setOptions` walks the id/value pairs with an `if/else-if` chain and no `else`; the whole list is also forwarded to `Client::setOptions` → `Screen::setOptions` → the platform screen, all of which match on known ids only. | `client/ServerProxy.cpp:759-794`; `deskflow/Screen.cpp:240-272` |

The decisive finding is row 2: **the version bump is only safe if the client also clamps what it
announces.** Without that, the FIRST machine in a rolling fleet deploy to get a 1.9 core becomes
unable to connect to any not-yet-updated 1.8 server — and because roles here are dynamic, that is a
random total KVM outage, not a clipboard degradation. (This is precisely the case the original
sentence "tolerates older minors" got backwards: tolerance is one-directional.)

### 3.2 Chosen mechanism

1. **Protocol minor 1.8 → 1.9** (fork-local, `ProtocolTypes.h`).
2. **`Client::handleHello` announces `min(kProtocolMinorVersion, serverMinor)`.** Implemented in
   stage 1 even though it lives in client code, because it is the safety property that makes the
   bump deployable at all; the version bump and the clamp must never ship apart. A clamping client
   talking to an old server announces 1.8 and behaves exactly as today.
3. **`initProxy` gains `case 9` → `ClientProxy1_9`, and its `default` now clamps a *higher*
   unknown minor down to the newest proxy it has** instead of refusing the client — closing row 2
   for the NEXT bump as well. (Refusing an unknown-but-higher minor is the trap this table found;
   we do not want to re-arm it for 1.10.)
4. **`kMsgDLaneAdvert = "DLAN%2i%s"`** — lane wire version + the 16-byte token as a
   length-prefixed string (binary-safe: `%s` is a 4-byte length plus bytes, so embedded NULs are
   fine). The code `DLAN` was checked against every code in `ProtocolTypes.cpp`
   (`CNOP CBYE CINN COUT CCLP CSEC CROP CIAK CALV DKDL DKDN DKRP DKUP DMDN DMUP DMMV DMRM DMWM
   DCLP DINF DSOP DFTR DDRG SECN LSYN QINF EICV EBSY EUNK EBAD`) — no collision.
5. **The advert is sent only from `ClientProxy1_9`**, and only from `Server::adoptClient` AFTER the
   session token is registered — so it is structurally impossible to send it to a <1.9 client (row 4
   says that would kill the connection), and impossible for a client to dial a lane the server has
   not yet armed.
6. **`kMsgMTLaneHello = "MTLH%2i%s%s"`** — lane version + screen name + token, written by the
   client on the LANE connection where a `kMsgHelloBack` would normally go. Also collision-checked
   against the list above.

### 3.3 Mechanism considered and rejected: advertise via `kMsgDSetOptions`

Row 5 of the table makes a zero-risk alternative possible — a new `OPTION_CODE` carrying the token
as four `int32`s would be silently ignored by every old client with **no version bump at all**, so
no rollout ordering could ever break a link. It was rejected because (a) it smuggles a
connection-scoped secret through a config-shaped broadcast channel that `Server::sendOptions`
rebuilds from `m_config` on every reconfiguration, (b) a 16-byte secret split across four option
values is unreadable at the wire level, and (c) the explicit message plus the clamp is equally safe
once §3.2 item 2 exists. Recorded here so a later session does not re-derive it as "the obvious
missed idea".

### 3.4 Mixed-version behaviour (accepted transition cost, owner-approved)

New cores never send clipboard data in-stream, so old↔new pairs get no clipboard sync **in either
direction**: a new server drops a clipboard bound for an old client (no lane), and a new client
drops one bound for an old server (nothing advertised a lane). Both log a rate-limited warning
naming the reason. Receiving still works both ways in a mixed pair, because new cores keep the
legacy RECEIVE parsers (`ServerProxy::setClipboard`, `ClientProxy1_6::recvClipboard`) intact and
tolerant — an OLD peer's in-stream clipboard is still understood by a NEW one.

> ✅ **Resolved 2026-08-06 (stage 2).** The client half exists: a 1.9 client dials the lane, so a
> new↔new pair moves clipboards on it in both directions. The stage-1-only caveat that stood here
> no longer applies.

**Plaintext deployments lose clipboard sync entirely (new limitation).** A lane must be a TLS
connection — the handoff has an `SSL` object to give the worker, and `ClientProxyUnknown` refuses to
detach anything else ("not a tls connection"). A client whose security level is `PlainText`
therefore does not dial at all (it would only ever be refused, and declining early costs no
sockets), and a plaintext pair gets no clipboard sync in either direction. This is acceptable here
because MouseTransfer always runs `PeerAuth`, but it IS a behaviour change for a stock unencrypted
Deskflow setup, and a plaintext lane would need both a `TCPSocket`-level detach and a non-TLS
`LaneConn` mode to fix.

## 4. Lane wire format (after lane-ack)

Frames, all little-endian, length-prefixed (`{u8 type, u32 bodyLength}` then the body):

- `START {clipId u8, seq u32, totalLen u32, clipboardSeq u32}` — receiver discards any partial train
  for that clipId and begins a new assembly. Enforce `totalLen` ≤ the configured maximum clipboard
  size (`m_maximumClipboardSize`, KB — same limit as today, wrapper emits 512 MB) BEFORE allocating.
  **`seq` and `clipboardSeq` are different numbers and must not be conflated** (stage 2 fix): `seq`
  is the sender's private TRAIN counter, which exists only to match DATA/END frames to their START;
  `clipboardSeq` is the APPLICATION's clipboard sequence number, the same value the in-stream path
  carries, and it is what the receiver reports upwards. `Server::onClipboardChanged` drops an update
  whose sequence number is lower than the last it saw, and a per-lane counter starting at 1 loses
  that comparison for the rest of the session — every clipboard a client copied would have been
  silently discarded as "mis-sequenced".
- `DATA {clipId u8, seq u32, offset u32, chunk bytes}` — chunk size ~256 KB.
- `END {clipId u8, seq u32}` — receiver validates completeness, then posts an event so the
  APPLY happens on the screen/main thread (clipboard APIs are thread-bound), which routes into
  the SAME apply path used today (ownership marker, `size<=4` empty-skip, dedup all preserved).
- `ACK`, `PING`, `PONG` — **reserved; v1 sends none of them.** An unknown frame type is skipped by
  its own length, so adding one later is additive and needs no lane version bump.

**A lane is SILENT from attach until one end has a clipboard, and that is a correctness property.**
The first draft had the accepting end greet with an `ACK` so the dialling end could tell "attached"
from "still connecting". It cannot, and the attempt breaks what it was meant to confirm — the socket
handoff is only legal while the connection is quiescent, so whichever end speaks first can wreck the
OTHER end's handoff. Both directions race:

- An `ACK` from the **dialling** end can reach the accepting end before its event loop has read the
  lane greeting. `PacketStreamFilter::readMore()` drains everything available, so those bytes land
  in the filter's buffer and `hasBufferedInput()` refuses the handoff for "the peer pipelined data
  behind its greeting" — which it did not do.
- An `ACK` from the **accepting** end can reach the dialling end before IT has detached. The dial
  detaches on `StreamOutputFlushed`, an event-queue hop AFTER the bytes went out, so on a loaded
  client the round trip finishes first; the ACK then sits in the socket's input buffer and
  `detachTls()` refuses — correctly, because those bytes would be lost.

Both would have been intermittent, load-dependent and self-healing-after-a-dropped-clipboard, which
is the worst way for a bug to present. A greeting can come back when there is a safe moment for it:
after BOTH ends have detached, not during.

**Latest-wins:** the sender keeps AT MOST ONE pending payload per (peer, clipId) — a newer copy
replaces an undelivered older one, and an in-flight send checks a superseded flag between chunks
and aborts (receiver sees a new START and discards). Bounded memory by construction: max one
payload per clipId per peer plus one in flight.

## 5. Send-path replacement (the "completely replace" part)

- `ClientProxy1_6::setClipboard` (server→client): hand the marshalled payload to the server-side
  lane manager for that client instead of `StreamChunker::sendClipboard`. If the client has no
  lane (old client, or lane down), DROP with a rate-limited LOG_WARN — never fall back in-stream.
- `Client::sendClipboard` (client→server, the leave-time send): hand to the client-side lane. Same
  rule.
- Remove the `ClipboardSending` event registration/handler in `ClientProxy1_6` (retires the
  documented handler-leak concern with it) and all `StreamChunker::sendClipboard` call sites.
  Prefer deleting now-dead code over leaving it compiled-but-unused UNLESS the diff gets large —
  reviewer's call, note the choice in MODIFICATIONS.md.
- Keep ALL existing send-DECISION gates exactly where they are: `isClipboardOwnedByUs()` leave
  gate, the `marshall().size() <= 4` empty skip, the size cap, dirty-flag bookkeeping. Only the
  TRANSPORT changes. Delivery decoupling: it is fine that `m_dirty` clears at queue time — the
  lane manager retains the latest payload per (peer, clipId) until delivered or superseded, so a
  lane that comes up late still delivers the newest state (this deliberately kills the
  reconnect-resend-from-zero pathology).

## 6. Threading & teardown rules (non-negotiable)

- Never block the screen/main thread: marshalling stays on it; hand-off to workers via
  mutex-guarded queue + condition variable; results back via `EventQueue::addEvent` (thread-safe).
- Worker threads are joinable, never detached while holding `this`; teardown on: main session
  close (server `removeClient` / client disconnect), socket error, superseding lane connection,
  process shutdown. Unblock blocking reads via `shutdown()` on the socket then close, then join.
- SSL objects are single-thread-owned after detach. NO process-wide state, NO statics — see the
  storm history.
- The multiplexer detach must leave no dangling jobs/callbacks referencing the old SecureSocket
  wrapper (an explicit release method, taken under the multiplexer removal + the socket mutex).
- Lane threads self-demote before first IO (see §2 priority).

### 6.1 DECIDED: one worker thread per lane, socket stays NON-blocking (deviation, with reasons)

The brief's "blocking IO, one reader thread + one writer thread, coordinated by a per-lane lock
around SSL_read/SSL_write" does not survive contact with two hard constraints, so the lane runs
ONE thread per connection driving both directions from a `select()` loop, on a socket left in the
non-blocking mode the multiplexer already put it in.

1. **A reader/writer pair on a blocking socket deadlocks the lane whenever both endpoints send a
   large clipboard at once.** A's writer holds A's SSL lock inside a blocking `SSL_write` that
   cannot complete until B reads; B's writer is in the same state; each side's reader is parked
   waiting for its own SSL lock and therefore never drains. Both sides wedge permanently. Chunking
   the write does not help — progress requires the reader to run *while* the writer is blocked
   inside `SSL_write`, which a lock cannot allow. (The lane failing this way would not touch the
   main session, but "two stuck threads and clipboard dead until restart" is not an acceptable
   resting state for a fix whose whole purpose is to stop a wedge.)
2. **OpenSSL forbids two threads inside one `SSL` object concurrently**, so the lock cannot simply
   be dropped. Given this fork's history — the entire 2026-07-29 outage was per-connection state
   living in the wrong place — an "everyone does it and it usually works" concurrency shortcut is
   exactly the wrong trade.

One thread per lane removes the whole class: the `SSL` object is touched by exactly one thread for
its entire life, there is no lock, no lock ordering, and no interleaving question. A pending write
can never starve reads because both are serviced by the same `select()`. The cost is that a queued
payload waits up to one poll interval (100 ms) before its first chunk goes out, which is
irrelevant for a clipboard.

`ARCH` exposes no public "set blocking" call (`setBlockingOnSocket` is private to each
`ArchNetwork*` backend, and sockets are created non-blocking), so leaving the socket non-blocking is
also the option that adds no new arch API. On Windows the detach additionally clears the
`WSAEventSelect` association the multiplexer installed, so the fd is a plain non-blocking socket
that `select()` owns outright.

Teardown stays exactly as specified: set the stop flag, `shutdown()` the socket, `join()` the
worker. The worker is never detached and never outlives its manager; the `select()` timeout bounds
how long teardown can take even if `shutdown()` were a no-op.

## 7. Diagnostics

- Lifecycle at LOG_DEBUG (`lane up/down/reject reason`, per-train summaries).
- Per-chunk verbosity gated on the existing `clipdiag` sentinel-file mechanism.
- A rejected/failed lane must log WHY (token mismatch vs no session vs fingerprint mismatch) —
  at DEBUG, never spammy at INFO.

## 8. Verification plan

- Compile: `inc-build.bat` (Qt 6.8.1 + Ninja). The `MT_QT5` path must still CONFIGURE (guard any
  new Qt usage — the lane should be Qt-free: std::thread + OpenSSL + Arch primitives only).
- Do NOT run deskflow-core instances on this dev box — a live production mesh runs here.
  End-to-end verification happens on the 3-VM rig (separate stage): deploy the built core to the
  VMs, big-bitmap repro via the `switchreq` mechanism, assert (a) no death loop, (b) clipboard
  arrives, (c) cursor stays live during transfer, (d) kill-mid-train recovers, (e) old↔new mixed
  mesh degrades per §3.
- MODIFICATIONS.md gains a lane entry (what + why + the owner decisions above).

## 9. File plan — STAGE 1 (server half + shared infra) AS BUILT

Done:

- NEW `src/lib/net/LaneConn.{h,cpp}` — owns `ArchSocket` + `SSL` + `SSL_CTX` after detach; frame
  read/write over a non-blocking socket with an internal `select()`; `shutdown()` is the only
  method callable from another thread. Also hosts the priority OS seam
  (`deskflow::lane::demoteCurrentThreadToBackground()`).
- NEW `src/lib/deskflow/ClipboardLane.{h,cpp}` — `deskflow::ClipboardLaneManager`: per-peer
  sessions (token + peer TLS fingerprint), per-peer lanes, one worker thread each, latest-wins
  queue (max ONE pending payload per (peer, clipId)), supersede-mid-train, teardown, inbound
  reassembly posted to the main thread via `EventTypes::ClipboardLaneReceived`.
- NEW `src/lib/server/ClientProxy1_9.{h,cpp}` — protocol 1.9 proxy; the ONLY thing that can emit
  `kMsgDLaneAdvert`.
- MOD `src/lib/net/SecureSocket.{h,cpp}` — `detachTls()` + `peerFingerprint()`.
- MOD `src/lib/server/ClientProxyUnknown.{h,cpp}` — lane-hello recognition, validation, detach and
  handoff; reuses the existing `ClientProxyUnknownFailure` teardown route (the socket wrapper is
  inert by then, so the normal cleanup deletes an empty shell).
- MOD `src/lib/server/Server.{h,cpp}` — owns the lane manager, mints the per-session token in
  `adoptClient`, sends the advert, tears the lane down in `removeClient`, applies inbound lane
  clipboards, keeps the manager's size cap in step with `m_maximumClipboardSize`.
- MOD `src/lib/server/ClientProxy1_6.{h,cpp}` — send-path replacement (§5) + `applyLaneClipboard`.
- MOD `src/lib/deskflow/ProtocolTypes.{h,cpp}` — minor 9, `kMsgDLaneAdvert`, `kMsgMTLaneHello`,
  lane wire constants.
- MOD `src/lib/base/EventTypes.h` — `ClipboardLaneReceived`.
- MOD `src/lib/client/Client.cpp` — announced-minor clamp (§3.2 item 2; safety, not the client half).
- MOD `src/lib/{net,deskflow,server}/CMakeLists.txt`.

> Two stage-1 defects were found and fixed while building the client half, both invisible until a
> client dialled: the lane dropped the application clipboard sequence number (§4), and the worker's
> opening `ACK` raced the peer's handoff (§4). Both are listed under §9a.

`src/lib/server/ClientListener.cpp` was NOT modified: the lane-hello arrives on a stream that
`ClientProxyUnknown` already owns, and the existing failure route already deletes the unknown
proxy, its stream and its socket in the right order.

## 9a. File plan — STAGE 2 (client half) AS BUILT

Done:

- NEW `src/lib/client/ClipboardLaneDialer.{h,cpp}` — the dial, as an event-driven state machine. It
  holds the token, opens the second connection, discards the server's `kMsgHello`, writes
  `kMsgMTLaneHello`, waits for `StreamOutputFlushed`, detaches, and hands a `LaneConn` to a
  callback. Owns the retry backoff. Knows nothing about `Client` (it takes a
  "prepare this socket" and a "here is a lane" callback), so a lane failure has no route to the
  main connection at all.
- MOD `src/lib/client/Client.{h,cpp}` — owns the client-side `ClipboardLaneManager` (built in the
  constructor, so it is always valid) and the dialer (built on the first advert, destroyed with the
  main session). Applies inbound lane clipboards on the main thread, keeps the manager's size cap in
  step with `setOptions`, and re-checks the lane on every `enter()`.
- MOD `src/lib/client/ServerProxy.{h,cpp}` — parses `kMsgDLaneAdvert` in BOTH parsers, sends the
  clipboard over the lane instead of `StreamChunker`, applies a lane-received clipboard through the
  same code the legacy receive uses, and no longer registers the `ClipboardSending` handler.
- MOD `src/lib/deskflow/ClipboardLane.{h,cpp}` — `send()` carries the application sequence number
  (see §4); `openLocalSession()` for the dialling end; the ACK is gone (see §4).
- MOD `src/lib/deskflow/ProtocolTypes.h` — `kUnknownClientTimeout` (was a bare `30.0` in
  `ClientListener`; the dial needs the same number), the lane frame body sizes as named constants,
  and the `START` layout + `Ack` doc changes.
- MOD `src/lib/server/{ClientListener,ClientProxy1_6}.cpp` — the timeout constant, and the
  server→client `send()` call passing sequence number 0 exactly as the `StreamChunker` call it
  replaced did.
- MOD `src/lib/client/CMakeLists.txt`.

**Where the dial runs, and why it cannot delay input.** Entirely on the existing event queue +
`SocketMultiplexer`, exactly like the main connection. Every slow part — TCP connect, TLS handshake,
waiting for the greeting, waiting for our own greeting to reach the wire — happens on the
multiplexer's service thread. What runs on the event-queue thread (which is also the thread that
puts the server's relayed input onto this screen) is only the short callbacks between those waits:
read 11 bytes, write ~40, detach. A dial that stalls or fails costs the input path nothing, because
it simply never posts its next event. Two consequences are load-bearing and must not be "simplified"
away:

- **`IStream::flush()` is deliberately NOT used** to wait for the greeting to drain, even though §9
  step 3 offered it: it parks its caller on a condition variable until the multiplexer has drained
  the socket, and that caller is the input thread. `EventTypes::StreamOutputFlushed` is used instead.
- **The address is copied already-resolved** from `Client::m_serverAddress`, so a dial never calls
  `NetworkAddress::resolve()` — DNS blocks.

**Session plumbing — the choice §9 left open.** `ClipboardLaneManager::openLocalSession()` was added
rather than calling `openSession()` and ignoring the token it mints. Reasons, in order of weight:
(1) a session created this way can NEVER authorise an inbound lane, because `validate()` compares
tokens with `lane::secretsEqual()`, which refuses two empty secrets — the refusal is a property of
the code, not of nobody happening to call `validate()` on a client's manager; (2) it does not mint a
real secret that nothing validates against; (3) it does not make a dialling endpoint's lane depend
on the CSPRNG, for a value it would never use. Both entry points go through one `installSession()`,
so "replace the session, hand the displaced lane back to be stopped outside the lock" exists once.

The client's manager is keyed by the fixed name `"server"`: the protocol never tells a client what
its server is called (`kMsgHello` carries a product name and a version), and a client has exactly
one peer, so there is nothing else to use and nothing it could be confused with.

**Retry policy.** A failed dial closes its own connection and schedules a retry with a doubling
backoff, from `kKeepAliveRate` (3 s) to `kUnknownClientTimeout` (30 s). Two event-driven nudges sit
on top, both no-ops unless the dialer is idle with a token: a clipboard send that finds no lane, and
every `Client::enter()`. The second exists because a lane can die WITHOUT the main connection dying
— an idle lane sends nothing, so a stateful firewall can reap it — and only the client can re-dial;
without it, server→client clipboard would stay dead until this machine next copied something.

**Deviation from §9 step 6 (the ACK), deliberate.** "Treat the server's `LaneFrame::Ack` as lane up"
is not implemented, because the ACK itself had to go — see §4 for the two races it caused. A refusal
after the handoff is instead detected by the connection closing: the worker exits, `hasLane()` reaps
it, and the next crossing or clipboard re-dials. The cost is that a refused lane is noticed one
event later rather than immediately; it cannot be noticed sooner without either blocking the input
thread on a lane read or teaching the manager a "confirmed" state, and the failure it guards against
(a valid token, for a live session, with a matching fingerprint, being refused) is not a case that
arises in a healthy mesh.

**The client's lane handshake, in the order the server half requires:**

1. Connect + TLS as normal. `ClientListener` will greet the new connection with `kMsgHello` before
   it knows what it is — **read and discard that packet**. Do NOT reply with `kMsgHelloBack`.
2. `ProtocolUtil::writef(stream, kMsgMTLaneHello, kLaneWireVersion, &screenName, &token)`, and write
   NOTHING else. The server refuses the handoff if anything is pipelined behind the greeting
   (`PacketStreamFilter::hasBufferedInput()`), because the lane takes the raw connection and would
   lose whatever the filter still held.
3. **Wait for the output to drain before detaching.** `SecureSocket::detachTls()` refuses a socket
   with a non-empty output buffer, and the greeting is still in it the instant after `writef`.
   **Wait for `EventTypes::StreamOutputFlushed`.** (This step originally offered `IStream::flush()`
   as an alternative; stage 2 rejected it — `flush()` parks its caller on a condition variable until
   the multiplexer has drained the socket, and on the client that caller is the thread delivering
   relayed input to the screen.) A refused detach is not fatal — close and re-dial on a backoff.
4. `SecureSocket::fromStream(stream)` → `detachTls()` → `deskflow::LaneConn::adopt(...)` →
   `ClipboardLaneManager::attach(peer, std::move(conn))`. The dialling end needs a session too:
   **`openLocalSession()`**, added in stage 2 — see the reasoning under §9a.
5. Tear the stream/socket wrapper down WITHOUT closing the connection. They are inert after a
   successful detach, so the ordinary teardown (remove handlers, delete the stream, delete the
   socket) frees empty shells and leaves the connection alone. Note that a REFUSED `detachTls()`
   still leaves the socket out of the multiplexer — it calls `setJob(nullptr)` before its
   preconditions — so a refusal must always be followed by a close, never by carrying on.
6. ~~The server sends a `LaneFrame::Ack` as its first lane frame; treat its arrival as "lane up".~~
   **Withdrawn in stage 2.** A lane is silent until someone has a clipboard: any greeting in either
   direction races the other end's handoff and breaks it. See §4 and the deviation note in §9a.

The manager, `LaneConn`, the frame codec, the latest-wins queue, teardown and the priority seam are
all shared — the client half needed no new lane machinery beyond `openLocalSession()`, only the dial
and the `ServerProxy` edits.

## 10. Working agreements for implementing agents

- Repo: this checkout (`X:\Users\bawat\Documents\deskflow-fork`, branch `merged-local-inject`).
  Commit granularly with the fork's existing style (`fix:`/`feat:`/`docs:` imperative summary,
  body explains WHY). Do not push; the orchestrator pushes after review.
- Read before writing: the storm + keepalive history in the wrapper repo's
  `claudemd/clipboard.md` (X:\Users\bawat\Documents\KVMDocumentDrag\claudemd\clipboard.md) and
  this file. Source every constant (no guessed magic numbers — derive or cite).
- Never touch the wrapper repo's code. Never launch deskflow-core on this machine.
- Update THIS file's §3/§9 with verified decisions as you make them.
