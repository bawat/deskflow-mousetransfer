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
   unknown minor down to `ClientProxy1_8`** instead of refusing the client — closing row 2
   for the NEXT bump as well. (Refusing an unknown-but-higher minor is the trap this table found;
   we do not want to re-arm it for 1.10.)
   > ⚠️ **1_8, not "the newest proxy we have" — corrected in review 2026-08-06.** Work out who
   > reaches that branch: a client of THIS fork clamps (item 2), so it can never announce more than
   > 9 and always lands on `case 9`. Everything left is a build that does NOT clamp, i.e. not ours,
   > and 1.9 here is not 1.9 anywhere else — in this fork it means precisely "understands
   > `kMsgDLaneAdvert`". `ClientProxy1_9` exists solely to send that advert, and row 4 of the table
   > says an unknown code kills the connection. Handing a stranger a 1_9 proxy therefore destroys
   > exactly the link this branch was written to save. 1_8 is the newest dialect that is safe to
   > speak to an unidentified peer, and the only thing it costs is the lane — which
   > `offerClipboardLane`'s `dynamic_cast<ClientProxy1_9 *>` gate would have withheld anyway.
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

> ⚠️ **Silence narrows that class; it does NOT close it — found in review, 2026-08-06.** A real
> clipboard races the peer's handoff exactly as an ACK did. The accepting end attaches and its
> worker writes a START for whatever was waiting; the dialling end has not detached yet, because it
> detaches on `StreamOutputFlushed`, a QUEUED event, and a LAN round trip beats an event-queue hop
> on a client whose event thread is busy relaying input. The START lands in `SecureSocket`'s input
> buffer and `detachTls()` refused it. Retaining payloads (above) makes this MORE likely rather than
> less, because a fresh lane now usually does have something to send — and the payload was then lost
> outright, the worker having already taken it out of the pending queue.
>
> **Fixed by carrying the bytes, not by adding another greeting.** `detachTls()` drains unread input
> into `DetachedTls::pending`; `LaneConn::readSome()` serves it before anything from the `SSL`
> object, and `waitReady()` reports readable while any remains (the socket cannot — those bytes are
> past the kernel). Unwritten OUTPUT and a latched partial `SSL_write` remain hard refusals; so does
> the packet filter's own buffered input, which cannot fire in this race (the flush event is queued
> before the read event that would fill the filter) and would need a messier drain.

**Latest-wins:** the sender keeps AT MOST ONE pending payload per (peer, clipId) — a newer copy
replaces an undelivered older one, and an in-flight send checks a superseded flag between chunks
and aborts (receiver sees a new START and discards). Bounded memory by construction: max one
payload per clipId per peer plus one in flight.

**And that payload is kept when there is no lane yet** — on the SESSION (`Session::m_held`), seeded
into the next lane by `attach()`. This is not an optimisation, it is what makes §5's "delivery
decoupling" true: both callers clear their dirty flag at the moment they call `send()`, and nothing
re-offers an unchanged clipboard (`Server` marks a client dirty again only when the clipboard
CHANGES), so a payload dropped for want of a lane is LOST, not delayed. The first implementation
dropped it, which silently lost every clipboard between a (re)connection and its lane coming up —
including the very common "client connects, user crosses onto it a second later" — and every
clipboard during a retry backoff. `SendResult::Held` reports it; `SendResult::NoLane` now means only
"this peer has no session at all", i.e. it is older than 1.9. (Review finding, 2026-08-06.)

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

## 7. Diagnostics — AS BUILT (stage 5, 2026-08-06)

> ⚠️ **The original of this section was wrong in a way that cost the rig two unexplained stalls.**
> It put the whole lifecycle at `LOG_DEBUG`, which reads as a sensible default and is, on a deployed
> machine, indistinguishable from not logging at all: **fleet cores run at the default INFO level,
> and the wrapper regenerates `settings\Deskflow.conf` on every engine start and every core respawn
> without ever writing `log/level`.** There is therefore no way to turn DEBUG on for the machine that
> is misbehaving, and nothing below INFO can be recovered after the fact. The `clipdiag` gating it
> promised was also never wired — `ClipboardChunk::diagEnabled()` appeared nowhere in lane code.

**At INFO, always — the minimal set a fleet log must carry.** One or two lines per peer per session
plus one per transfer; a lane is per-peer and long-lived, so this is not chatty.

| line | where | says |
|---|---|---|
| `clipboard lane up for "<peer>" [sock N]` | `ClipboardLaneManager::attach` | the lane exists |
| `clipboard lane down for "<peer>": <reason>` | `ClipboardLaneManager::stopLane` | it stopped, and why |
| `clipboard lane "<peer>": sent clipboard <id>, <n> byte(s) in <t>s` | sending worker | a transfer completed, from this end |
| `clipboard lane "<peer>": received clipboard <id>, <n> byte(s) in <t>s` | receiving worker | a transfer completed, at this end |
| `clipboard <id> for "<peer>" is waiting for the lane (<n> bytes)` | `ClientProxy1_6::setClipboard` / `ServerProxy::onClipboardChanged` | `SendResult::Held` — kept, not lost |
| `clipboard <id> not delivered ... older than protocol 1.9 ...` | same two, rate-limited | `SendResult::NoLane`, already WARN |
| `clipboard lane ... worker thread NOT demoted ...` | `runLaneBody` + `demoteCurrentThreadToBackground` | see below |

The **down reason** is recorded state, not a guess at the call site: `LaneConn::markDead()` publishes
a string literal through an atomic, first writer wins, so the real cause ("the peer closed the
connection", "a tls error", "the connection failed (socket error)") survives the teardown that
follows it and is never overwritten by "torn down locally". When the connection has no verdict of its
own, the caller's reason is used instead ("the main session closed", "a newer lane connection
superseded it", "this endpoint is shutting down", "its worker stopped").

**A transfer that never completes is diagnosed by the ABSENCE of a completion line between an `up`
and a `down`.** That is deliberate: a per-train START line at INFO would double the volume for a
case the up/down pair already brackets. The sender's completion line with no matching receiver line
names the direction that failed.

**Thread demotion failure is WARN** (`lane::demoteCurrentThreadToBackground`, `LaneConn.cpp`), not
the `LOG_DEBUG1` it was. It is the single most consequential thing that can quietly go wrong in
here: `deskflow-core` puts itself in **`REALTIME_PRIORITY_CLASS`** (sourced, not assumed —
`AppUtilWindows.cpp:112` calls `Thread::setPriority(-14)`, which indexes `ArchMultithreadWindows`'s
table at entry 22, `{REALTIME_PRIORITY_CLASS, THREAD_PRIORITY_TIME_CRITICAL}`), and an undemoted
worker streaming tens of megabytes in that process competes directly with the input relay this whole
design exists to keep clear. Two lines are emitted on failure — the OS error from inside the seam,
and the peer name from the worker.

**Per-chunk detail is gated on `mtDiagEnabled()`** — the same `clipdiag` sentinel `ClipboardChunk`
uses: a file named `clipdiag` in the core's working directory, or `MOUSETRANSFER_CLIPDIAG=1`. The
FILE is the primary switch on purpose, because the elevated core's environment block is frozen when
its task is registered. It is read ONCE per worker into a local, so the gate costs nothing per chunk.
Gated lines: worker start/exit, `tx DATA` and `rx DATA` per chunk with offsets, `rx START` — and
**`ssl_pending=` after every reassembled chunk, which is exactly the probe §9b left for the rig.**
Anything other than 0 there means OpenSSL is holding decrypted plaintext that `select()` will never
report, and the lane is losing a poll interval per read.

- A rejected lane still logs WHY at DEBUG (token mismatch vs no session vs fingerprint mismatch), in
  `ClientProxyUnknown` — unchanged, and deliberately not promoted: a refusal is per-connection and an
  attacker could make it spam.

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

## 9b. Adversarial review pass — AS FIXED (2026-08-06, stage 3)

Read by a third agent against this file, `claudemd/clipboard.md` and the full
`529fd9824..HEAD` diff, with the explicit brief of finding the storm-class bugs (per-connection
state in shared storage, buffered bytes surviving into a reused slot, handler lifetime versus
raw-pointer event dispatch). Seven defects confirmed by reading and fixed; the full write-up is in
`MODIFICATIONS.md` under "Adversarial review pass". In short:

| # | defect | where |
|---|---|---|
| 1 | a clipboard handed over before the lane came up was DROPPED, contradicting §4/§5 and losing it permanently (the caller's dirty flag is already clear) | `ClipboardLaneManager::send` |
| 2 | the session key (CANONICAL screen name) and the lane greeting / server send (the client's RAW name) were different name spaces — an alias or a case difference killed the lane silently | `handleLaneHello`, `ClientProxy1_6::setClipboard` |
| 3 | `initProxy`'s permissive default gave an unidentified peer a `ClientProxy1_9`, so the server would advertise a lane at a client that dies on unknown codes | `ClientProxyUnknown::initProxy` |
| 4 | a REFUSED `detachTls()` left a live socket out of the multiplexer | `SecureSocket::detachTls` |
| 5 | a peer's first frame can beat this end's detach, and was a refusal (see the §4 note) | `detachTls` / `LaneConn` |
| 6 | `m_dead` was a non-atomic `bool` written by `shutdown()` from another thread | `LaneConn` |
| 7 | `select()` on POSIX overflows an `fd_set` for a descriptor ≥ `FD_SETSIZE` | `LaneConn::waitReady` |

Plus: the "no lane" warning was suppressed for the first ~30 s of UPTIME, because `ARCH->time()` is
seconds since boot and the rate limit compared it against a `0.0` initialiser — on a fleet that
starts from a boot-time scheduled task, that is the whole window a rolling deploy's mixed versions
live in.

**Checked and found sound, recorded so they are not re-derived:** deleting the dial's stream and
socket from inside the packet filter's own `filterEvent` chain (the claimed precedent holds —
`Client::handleDisconnected → cleanupStream()` does the same, and neither filter frame touches a
member after the nested `dispatchEvent`); `nudge()` cannot produce a duplicate lane; the START frame
layout and `kLaneMaxFrameBody` slack are exact; no static or thread_local mutable state exists
anywhere in the new code; no token reaches any log line; the frame reader is bounded at every field
boundary and allocates only after checking the length; `Client::enter()`'s lane check cannot block
the input thread (no worker ever takes the session mutex, and no worker holds any lock during IO).

**Left for the rig, with the probe that would settle each:**

- **A refused dial leaves the server's lane attached until its worker notices the close.** Between
  the attach and the reap, `hasLane()` says live and a clipboard queued in that window goes onto a
  dying lane. Probe: force a refusal (delay the client's `StreamOutputFlushed` dispatch), copy on
  the server during the window, assert the clipboard arrives after the re-dial.
- **`SSL_pending` after a 16 KB read.** The read buffer equals one TLS record's maximum plaintext
  and OpenSSL's `read_ahead` is off, so a record should never be left half-consumed — but if it
  ever were, `select()` would not report it and the lane would stall a poll interval per byte.
  Probe: log `SSL_pending()` after every `SSL_read` under a big-clipboard transfer and assert zero.
- **`SSL_write` returning `WANT_READ` (TLS 1.3 key update / post-handshake ticket)** makes the
  worker spin at full speed until the peer sends something, because `select()` keeps reporting
  writable. Bounded and transient, on a background-priority thread. Probe: a long-lived lane with a
  forced `SSL_key_update`, watching the worker's CPU time.
- **A stale queued socket event delivered to a REUSED heap address.** `advertised()` is the one path
  that deletes a dial's socket and allocates a new one inside a single dispatch, so an event still
  queued for the old address could be handed to the new attempt. Cost is one spurious abandon plus a
  backoff. Probe: re-advert (force a client re-adopt) while a dial is in flight, repeatedly.
- **A clamped client against a genuinely OLD (<1.3) server.** The clamp now announces the server's
  minor, so such a pairing connects where it used to be refused — and a pre-1.3 server sends no
  `kMsgCKeepAlive`, which `ServerProxy`'s alarm requires. No such server exists in this fleet or
  upstream (v1.26.0 is 1.8), so this is a note rather than a finding.

## 11. Where a 64 MiB clipboard's memory and CPU actually go (stage 5, 2026-08-06)

Rig stage 4 passed functionally but produced three things that block deployment: one 64 MiB publish
took the server core from 3 MB to **517 MB** private bytes (~8×), settling ~388 MB; the server froze
once for ~3 minutes, silent, with its CPU advancing ~170 s; and a client froze once for ~90 s while
its whole 2-vCPU guest was CPU-pinned. Everything ≤48 MiB was uniformly clean in every run.

### 11.1 The memory is fully accounted for — and it is not a leak or a quadratic

Counted statically, per client that has been pushed the clipboard, with `N` = the marshalled payload
(64 MiB = 67.1 MB). Before this stage, on a server with two clients that have both been pushed:

| what | where | live copies of N |
|---|---|---|
| the Windows read: UTF-16 handle copy + UTF-8 result | `MSWindowsClipboardAnyTextConverter::toIClipboard` | 3 (transient) |
| the server's stored clipboard | `Server::ClipboardInfo::m_clipboard` | 1 (retained) |
| `marshall()`: the per-format temporaries + the result | `IClipboard::marshall` | 2 (transient) |
| the server's marshalled cache | `ClipboardInfo::m_clipboardData` | 1 (retained) |
| `onClipboardChanged`'s local `data` | `Server.cpp` | 1 (transient) |
| each proxy's mirror of the clipboard | `ClientProxy1_0::m_clipboard[id]` | 1 per client (retained) |
| each proxy's OWN marshalling of the same bytes | `ClientProxy1_6::setClipboard` | 2 per client (transient) |
| each peer's queued lane payload | `Lane::m_pending` / `Session::m_held` | 1 per client (retained) |

**Model: peak 8N = 537 MB, settle 6N = 402 MB. Measured: 517 MB and 388 MB.** Within 4 % of both.
There is no leak and no quadratic here — it is per-consumer copying, one copy per consumer, and the
consumers multiply with the client count.

Two more costs on the same account, both linear and both invisible in the peak because they are
transient: `switchScreen` re-marshalled BOTH clipboard ids from scratch on **every seam crossing**
purely to look at two sizes (268 MB of allocate-copy-free per crossing at 64 MiB, on the thread that
relays input), and the client marshalled the same clipboard **twice** per send — once in
`Client::sendClipboard` for the size and unchanged checks, then again in
`ServerProxy::onClipboardChanged`.

### 11.2 What stage 5 changed, and what it leaves

Fixed (commit `perf: marshall a clipboard once and share the one buffer`):

- One immutable `shared_ptr<const std::string>` per clipboard change, from `Server::onClipboardChanged`
  through every proxy, both queues and every worker. The per-proxy marshall and the per-peer queued
  copy are gone — the lane's payload cost no longer scales with the client count at all.
- `switchScreen` size-checks the cached marshalling. Per-crossing cost: 268 MB → **0**.
- `IClipboard::add(Format, std::string&&)` (default forwards to the copying overload, `Clipboard`
  overrides it to move) removes the extra full copy from `IClipboard::copy` AND `IClipboard::unmarshall`
  — i.e. from every `getClipboard`, every `setClipboard` and every apply, on both endpoints.
- `marshall()` releases each format's temporary as its bytes land in the result.
- The client marshalls once instead of twice.
- The lane receiver `reserve()`s the announced total (already bounded by the configured cap) instead
  of growing geometrically from 256 KB: ~2N of memcpy and a transient 2.5N peak → exactly N.
- The lane sender builds each `DATA` frame straight into its send buffer instead of into a temporary
  that `frame()` then copied: 128 MB of pure memcpy per 64 MiB transfer, and zero per-chunk
  allocation after the first.

**Model after: peak ~4N (268 MB), settle ~4N (268 MB)** for the same two-client 64 MiB publish — a
~50 % cut in peak and ~33 % in steady state, and the steady state now grows by 1N per client rather
than 2N.

**Left, deliberately, and NOT half-fixed:**

- **Each client proxy still keeps its own `Clipboard` mirror (1N per client, retained).** That is
  proxy STATE — what this client is believed to hold — not a transport buffer, and it is read by
  `ClientProxy1_0::getClipboard`. Removing it would change upstream semantics that predate the fork
  for one copy of saving.
- **The Windows clipboard WRITE path is the worst endpoint and is untouched.** Applying a received
  clipboard costs, in sequence: `convertLinefeedToWin32` (a full copy), `Unicode::UTF8ToUTF16` (2N
  for a mostly-ASCII payload), and a `GlobalAlloc` of the same 2N that is then memcpy'd into. With
  the assembly buffer and the unmarshalled `Clipboard` still live, a **receiving** endpoint peaks
  around **7N** — worse than the server. This is inherent to `CF_UNICODETEXT` and to
  `IMSWindowsClipboardConverter`'s `HANDLE fromIClipboard(const std::string &)` signature; removing
  it means changing that interface for every platform converter, which is out of proportion to this
  stage. **It is the binding constraint on §12's cap.**
- `IClipboard::marshall` still copies each format once via `get()`, because `get()` returns by value
  through a polymorphic interface and calling it twice is not an option (on Windows it is a clipboard
  read plus a full character-by-character conversion).

### 11.3 The two stalls are NOT explained. Stated plainly.

The memory account above closes; **the CPU does not.** 170 CPU-seconds on the server and ~90 on the
client are two to three orders of magnitude more than any linear pass over 64 MiB can cost, and a
static hunt for the classic cause found nothing:

- `IClipboard::marshall` reserves the **exact** total and appends once per format — linear.
- `IClipboard::unmarshall` walks the buffer once with no reallocation of the source — linear.
- The lane's reassembly appended 256 KB at a time into a `std::string`, which grows **geometrically**
  — amortised O(n) with a constant near 2, not O(n²). (Now exactly 1, via `reserve`.)
- `Unicode::UTF8ToUTF16` / `UTF16ToUTF8` / `convertLinefeedTo*` all reserve up front and append —
  linear, with a per-character constant.
- `ProtocolUtil` and `StreamChunker` are not on the lane's payload path at all.

So: **no O(n²) exists in the path, and the fixes above do not explain stalls A or B.** They will
lower the pressure that may have contributed, and they remove ~250 MB of the server's peak, but
claiming they close the stalls would be a guess. What stage 5 does instead is make the next
occurrence diagnosable, and name the three candidates with the instrument that now settles each:

1. **The lane worker is not actually demoted.** `LaneConn.cpp` assumes Windows background mode is the
   escape hatch from `REALTIME_PRIORITY_CLASS`; that assumption has never been tested in a realtime
   process. STALL B's shape — *the whole 2-vCPU guest CPU-pinned, the event thread missing keepalives,
   an SSH banner exchange timing out* — is precisely what a realtime-priority worker looks like.
   **Measured here (standalone probe, not the core):** background mode succeeds and pins the thread
   to absolute priority 4 under `NORMAL_PRIORITY_CLASS` (offset −4 from base 8) and under
   `HIGH_PRIORITY_CLASS` (offset −9 from base 13). `REALTIME_PRIORITY_CLASS` **could not be tested**
   — `SetPriorityClass(REALTIME)` silently degrades to `HIGH` without `SeIncreaseBasePriorityPrivilege`,
   which the probe did not have. Note the corollary: a core NOT running as LocalSystem is at HIGH,
   where the demotion demonstrably works, so this would present only on elevated cores.
   *Instrument:* the new WARN. If it appears on a rig guest, this is the cause and the seam needs a
   different mechanism for realtime processes.
2. **`SSL_write` returning `WANT_READ`** (TLS 1.3 key update / post-handshake ticket) spins the
   worker at full speed, because `select()` keeps reporting the socket writable. Already listed as an
   open item in §9b, bounded and transient in theory, unbounded in practice if the peer never speaks.
   *Instrument:* `clipdiag` — a burst of `tx DATA` lines with no advancing offset, or none at all
   while the worker is hot.
3. **The Windows clipboard write, on the main thread, logging nothing.** On the server the only code
   between the last line it printed (`screen "..." updated clipboard 1`, `Server.cpp`) and its
   silence is `m_active->setClipboard()`. If the active screen is the PRIMARY, that is
   `MSWindowsScreen::setClipboard` → two full character-by-character conversion passes → a 128 MiB
   `GlobalAlloc` → `SetClipboardData`, which then notifies **every clipboard listener in the session**
   — including the MouseTransfer wrapper's own watcher, which reads the clipboard back. None of that
   logs at any level, and it is the only silent, CPU-bound, main-thread work of the right shape.
   *Instrument:* none added here — it is outside the lane. The rig can settle it by watching whether
   the freeze reproduces with the wrapper's clipboard watcher disabled.

## 12. Payload cap — recommendation and the arithmetic (stage 5; NO cap changed)

The wrapper emits `clipboardSharingSize = 512 MB` as a memory guardrail. The rig's knee: 64 MiB
intermittently bad on 2-vCPU/4 GB guests, ≤48 MiB always clean.

**The binding constraint after §11.2 is the RECEIVING endpoint, not the server.** The server's
multiplier fell from ~8N to ~4N, which would move its knee to roughly 128 MiB; the receiver's fell
only from ~8N to ~7N, because the Windows clipboard write path (§11.2) is untouched. So the knee
should be expected to move very little, and a cap chosen from the server's improvement would be
wrong.

Worst-endpoint arithmetic, after the fixes: `peak ≈ 7 × N` on a machine applying a received
clipboard (assembly N, unmarshalled `Clipboard` N, linefeed copy N, UTF-16 conversion 2N, `HGLOBAL`
2N). Solving for a memory budget `B`: `N ≤ B / 7`.

| budget for one transfer | machine it suits | implied cap |
|---|---|---|
| 256 MB (~6 % of RAM) | a 4 GB guest / the Win8 laptop | **~36 MB** |
| 512 MB | an 8 GB fleet machine | ~73 MB |
| 3.5 GB (today's 512 MB setting) | nothing in this fleet | — |

**Recommendation — the owner decides the number; this is the basis:**

1. **Wrapper `clipboardSharingSize`: 32768 KB (32 MB).** It is the operational knob, per-fleet
   tunable, and it already reaches both endpoints (the server's `m_maximumClipboardSize` and, via
   `kMsgDSetOptions`, the client's). 32 MB sits inside the measured always-clean band with a 1.5×
   margin, costs ~224 MB on the worst endpoint of a 4 GB guest, and is 128 lane chunks.
2. ~~**A fork-side absolute clamp as well, at 128 MiB**~~ — ✅ **IMPLEMENTED**, see §12.1. 4× the
   operational number, so it never binds in normal use. This is not belt-and-braces, it closes a real
   hole: `m_maximumClipboardSize` defaults to `INT_MAX` (`Server.h`, `Client.h`), i.e. ~2 TB, whenever
   the option is absent — a stale `Deskflow.conf`, a non-MouseTransfer deployment, a config the
   wrapper did not regenerate. **Stage 5's `reserve(total)` change makes that default the literal size
   of a single allocation the receiver will attempt from a peer-supplied length**, so the setting is
   now load-bearing in a way it was not before. The lane is authenticated (token + matching peer
   certificate), so the threat model is a paired machine rather than a stranger, and the failure is
   graceful (`bad_alloc` → caught → the lane goes down, the main session untouched) — but a
   configuration accident should not be able to ask for 4 GB.

Recommendation 1 was **DECLINED by the owner (2026-08-06)**: the wrapper's emitted 512 MB
`clipboardSharingSize` stands unchanged, so the fleet's effective per-payload bound is the 128 MiB
ceiling below. The owner accepts the measured residuals of that choice, recorded by the stage-6 rig
re-check on 2-vCPU/4 GB guests: a server core RETAINS ~453 MB while a 64 MiB clipboard stays live
(released on the next copy — retention, not a leak; the §11.2 upstream proxy/server state), relay
latency during such a transfer degraded to 3.8 s worst-case, and a payload the ceiling REFUSES still
costs the SENDER full marshalling before the refusal (929 MB transient at 132 MiB). File copies are
unaffected at any size (CF_HDROP never rides the clipboard path). Revisit only with fleet log
evidence — the INFO lifecycle lines exist precisely so that evidence would be there.

### 12.1 The ceiling, AS BUILT

`kLaneMaxPayloadBytes = 128 MiB`, in `ProtocolTypes.h` beside the other lane constants, with the §12
arithmetic recorded there as its justification (4× the recommended 32 MB knob, bounded by the
worst-endpoint 7N account, so the very worst it permits is ~900 MB rather than the 4 GB a `uint32`
`totalLen` could otherwise ask for).

**It is applied in exactly one place** — `ClipboardLaneManager::setMaxPayloadBytes()` and the
constructor take `min(configured, kLaneMaxPayloadBytes)`. Every other check on both paths reads the
result through `maxPayloadBytes()`, so **the wrapper-emitted `clipboardSharingSize` governs exactly
as it does today, right up to the ceiling**, and no other site has to remember the ceiling exists. A
configured value above the ceiling is reported once, at NOTE, when it is set — because a transfer
later refused for a size the configuration appears to allow is otherwise baffling.

**Receive.** `runLaneBody`'s START handler now has two tiers, and the order matters:

- `total > kLaneMaxPayloadBytes` ⇒ **the lane is dropped, before `reserve()` allocates anything.** No
  conforming sender can emit this, because every sender applies the same ceiling, so it means a
  broken or hostile peer rather than a disagreement. The reason carries the numbers and comes out on
  the INFO down-line: `clipboard lane down for "<peer>": the peer announced a <n> byte clipboard,
  above this build's ceiling of <m> bytes`, plus a WARN at the moment of refusal. (`LaneConn`'s
  `deadReason()` is an `atomic<const char *>` of literals — which is what makes it any-thread
  readable — so a formatted reason goes through the new `Lane::m_downDetail`, written by the worker
  and read by `stopLane()` **after the join**.)
- `total > m_maxPayloadBytes` but within the ceiling ⇒ **unchanged**: the train is refused at DEBUG
  and the lane stays up. Two peers may legitimately be configured differently.

**Send.** `ClipboardLaneManager::send()` already tested `payload->size() > m_maxPayloadBytes`, and
that value is now the clamped one — so a misconfigured sender gets `SendResult::TooLarge` and skips
with a NOTE at both call sites, having dialled nothing and queued nothing. Both messages now print
the limit **in force** rather than describing it as "the configured limit", because those are
different numbers precisely when the ceiling is what bit.

## 10. Working agreements for implementing agents

- Repo: this checkout (`X:\Users\bawat\Documents\deskflow-fork`, branch `merged-local-inject`).
  Commit granularly with the fork's existing style (`fix:`/`feat:`/`docs:` imperative summary,
  body explains WHY). Do not push; the orchestrator pushes after review.
- Read before writing: the storm + keepalive history in the wrapper repo's
  `claudemd/clipboard.md` (X:\Users\bawat\Documents\KVMDocumentDrag\claudemd\clipboard.md) and
  this file. Source every constant (no guessed magic numbers — derive or cite).
- Never touch the wrapper repo's code. Never launch deskflow-core on this machine.
- Update THIS file's §3/§9 with verified decisions as you make them.
