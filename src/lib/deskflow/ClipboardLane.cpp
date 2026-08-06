/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 MouseTransfer fork
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/ClipboardLane.h"

#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/ProtocolTypes.h"
#include "net/LaneConn.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace deskflow {

namespace {

//! Bytes of every lane frame header: `uint8 type` + `uint32 bodyLength`, little-endian
constexpr uint32_t kFrameHeaderSize = 1 + 4;

//! How long a worker sits in select() before rechecking its queue and its stop flag
/*!
This is the only latency the single-worker design costs: a payload queued just after a wait started
waits up to this long for its first chunk. Irrelevant for a clipboard, and it buys a design with no
lock between reading and writing at all (see MT-CLIPBOARD-LANE-DESIGN.md §6.1). It is also the
ceiling on how long a teardown can take if shutdown() somehow fails to wake the wait.
*/
constexpr double kPollSeconds = 0.1;

//! Read buffer size. One TLS record is at most 16 KB, so this holds a whole record.
constexpr uint32_t kReadBufferSize = 16 * 1024;

void putU8(std::string &out, uint8_t value)
{
  out.push_back(static_cast<char>(value));
}

void putU32(std::string &out, uint32_t value)
{
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xff));
  }
}

uint32_t getU32(const uint8_t *bytes)
{
  return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

//! Build a complete frame: header then body
std::string frame(uint8_t type, const std::string &body)
{
  std::string out;
  out.reserve(kFrameHeaderSize + body.size());
  putU8(out, type);
  putU32(out, static_cast<uint32_t>(body.size()));
  out += body;
  return out;
}

} // namespace

ClipboardLaneManager::ClipboardLaneManager(IEventQueue *events, size_t maxPayloadBytes)
    : m_events(events),
      // Same min() as setMaxPayloadBytes, because a manager built before any configuration arrives
      // takes the INT_MAX default straight from Server.h / Client.h.
      m_maxPayloadBytes(std::min<size_t>(maxPayloadBytes, kLaneMaxPayloadBytes))
{
  // The workers post inbound payloads here; the handler runs on the main thread, which is the whole
  // point of routing them through the event queue rather than calling back directly.
  m_events->addHandler(EventTypes::ClipboardLaneReceived, this, [this](const Event &event) {
    if (const auto *info = dynamic_cast<const LaneClipboardInfo *>(event.getDataObject());
        info != nullptr && m_receiveHandler) {
      m_receiveHandler(*info);
    }
  });
}

ClipboardLaneManager::~ClipboardLaneManager()
{
  closeAll();
  m_events->removeHandler(EventTypes::ClipboardLaneReceived, this);
}

void ClipboardLaneManager::setReceiveHandler(ReceiveHandler handler)
{
  m_receiveHandler = std::move(handler);
}

void ClipboardLaneManager::setMaxPayloadBytes(size_t bytes)
{
  // THE one place the fork's ceiling is applied. Everything else -- send()'s TooLarge test, the
  // receiver's START check -- reads m_maxPayloadBytes, so the configured limit governs exactly as
  // before up to the ceiling, and no other site has to remember the ceiling exists.
  const size_t effective = std::min<size_t>(bytes, kLaneMaxPayloadBytes);
  if (effective != bytes) {
    // NOTE, not DEBUG: this means the configured clipboard limit is not the one in force, which is
    // the sort of thing that has to be visible when a transfer is later refused for a size the
    // configuration appears to allow. Once per settings change, not per clipboard.
    LOG_NOTE(
        "clipboard lane: the configured clipboard limit of %llu byte(s) is above this build's ceiling; "
        "using %llu byte(s)",
        static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(effective)
    );
  }
  m_maxPayloadBytes = effective;
}

std::string ClipboardLaneManager::openSession(const std::string &peer, const std::string &peerFingerprint)
{
  // 128 bits from OpenSSL's CSPRNG. RAND_bytes is the only randomness this code base already links,
  // and a failure from it is a hard no -- a predictable token must never be issued, so the session
  // simply does not open and the peer is never advertised a lane.
  std::vector<unsigned char> token(kLaneTokenSize);
  if (!lane::secureRandomBytes(token.data(), token.size())) {
    LOG_WARN("clipboard lane: no secure randomness available; not offering a lane to \"%s\"", peer.c_str());
    return {};
  }

  std::string issued(reinterpret_cast<const char *>(token.data()), token.size());
  // Outside the lock: joining a worker must never happen with the session map held.
  stopLane(installSession(peer, issued, peerFingerprint), "the peer opened a new session");

  LOG_DEBUG(
      "clipboard lane: session opened for \"%s\" (peer authenticated: %s)", peer.c_str(),
      peerFingerprint.empty() ? "no" : "yes"
  );
  return issued;
}

void ClipboardLaneManager::openLocalSession(const std::string &peer)
{
  // No token and no fingerprint: see the header. This session exists only so that attach() has
  // somewhere to put the lane this endpoint dialled, and it can never validate an inbound one.
  stopLane(installSession(peer, std::string(), std::string()), "the server advertised a new lane");
  LOG_DEBUG("clipboard lane: local session opened for \"%s\"", peer.c_str());
}

std::unique_ptr<ClipboardLaneManager::Lane>
ClipboardLaneManager::installSession(const std::string &peer, std::string token, std::string fingerprint)
{
  std::scoped_lock lock{m_mutex};
  Session &session = m_sessions[peer];
  // A reconnecting peer gets a brand new session, and the previous lane goes with the previous one
  // -- an old token must never open a lane onto a new session, and a stale lane must never outlive
  // the session that authorised it.
  std::unique_ptr<Lane> displaced = std::move(session.m_lane);
  session.m_token = std::move(token);
  session.m_fingerprint = std::move(fingerprint);
  // A new session is a new peer state, so nothing the OLD one was holding carries over. Keeping it
  // would mean a client that reconnected minutes later received a clipboard from before it left,
  // which is the "replay history" behaviour the retention is careful not to be.
  session.m_held.clear();
  return displaced;
}

void ClipboardLaneManager::closeSession(const std::string &peer)
{
  std::unique_ptr<Lane> doomed;
  {
    std::scoped_lock lock{m_mutex};
    const auto found = m_sessions.find(peer);
    if (found == m_sessions.end()) {
      return;
    }
    doomed = std::move(found->second.m_lane);
    m_sessions.erase(found);
  }
  // Outside the lock: stopping a lane joins a thread, and nothing else should be waiting on the
  // session map for however long that takes.
  stopLane(std::move(doomed), "the main session closed");
  LOG_DEBUG("clipboard lane: session closed for \"%s\"", peer.c_str());
}

void ClipboardLaneManager::closeAll()
{
  std::map<std::string, Session> doomed;
  {
    std::scoped_lock lock{m_mutex};
    doomed.swap(m_sessions);
  }
  for (auto &entry : doomed) {
    stopLane(std::move(entry.second.m_lane), "this endpoint is shutting down");
  }
}

void ClipboardLaneManager::stopLane(std::unique_ptr<Lane> lane, const char *why)
{
  if (!lane) {
    return;
  }

  // Read BEFORE shutdown(), which records a reason of its own ("torn down locally") if the
  // connection has not already failed for a better one. The distinction is the whole point of the
  // line: "the peer closed the connection" and "the session was replaced" are different incidents
  // with different next steps.
  const char *failure = lane->m_conn ? lane->m_conn->deadReason() : nullptr;

  // The order is the contract from MT-CLIPBOARD-LANE-DESIGN.md §6: raise the flag, unblock the
  // wait, join, and only then let the connection be destroyed. Nothing is ever detached, so a
  // worker cannot outlive the object it points at.
  lane->m_stop = true;
  if (lane->m_conn) {
    lane->m_conn->shutdown();
  }
  if (lane->m_worker.joinable()) {
    lane->m_worker.join();
  }

  // Read AFTER the join, which is the synchronisation point that makes this plain member safe: the
  // worker wrote it before it shut the connection down. It takes priority because the only reason
  // that uses it is the one carrying numbers the reader needs (see Lane::m_downDetail).
  const char *reason = !lane->m_downDetail.empty()  ? lane->m_downDetail.c_str()
                       : failure != nullptr         ? failure
                                                    : why;

  // INFO, not DEBUG: the fleet runs its cores at the default log level and never writes log/level
  // into the settings a wrapper regenerates on every start, so anything below INFO does not exist
  // as far as a deployed machine is concerned. One line per lane teardown is not chatty -- a lane
  // is per-peer and lasts the whole session.
  LOG_INFO("clipboard lane down for \"%s\": %s", lane->m_peer.c_str(), reason);
  // ~Lane frees the LaneConn, which is now touched by nobody.
}

bool ClipboardLaneManager::validate(
    const std::string &peer, const std::string &token, const std::string &peerFingerprint, int16_t laneVersion,
    std::string &reason
) const
{
  if (laneVersion != kLaneWireVersion) {
    reason = "lane wire version mismatch";
    return false;
  }

  std::scoped_lock lock{m_mutex};
  const auto found = m_sessions.find(peer);
  if (found == m_sessions.end()) {
    reason = "no live session with that screen name";
    return false;
  }

  const Session &session = found->second;

  // Constant time. Comparing a secret with == would leak its matching prefix through timing, and
  // secretsEqual() also refuses two EMPTY tokens, so a session that never got one cannot validate.
  if (!lane::secretsEqual(token, session.m_token)) {
    reason = "lane token does not match this session";
    return false;
  }

  // The lane must come from the same peer as the main session. With PeerAuth both fingerprints are
  // real and this is the binding that makes the token useless to anyone else; without it (plain
  // Encrypted, no client certificate requested) both are empty and this check is vacuous by
  // construction rather than by omission -- the token still gates.
  if (peerFingerprint != session.m_fingerprint) {
    reason = "lane peer certificate differs from the main session's";
    return false;
  }

  return true;
}

void ClipboardLaneManager::attach(const std::string &peer, std::unique_ptr<LaneConn> conn)
{
  if (!conn) {
    return;
  }

  std::unique_ptr<Lane> displaced;
  Lane *lane = nullptr;
  {
    std::scoped_lock lock{m_mutex};
    const auto found = m_sessions.find(peer);
    if (found == m_sessions.end()) {
      // Validated a moment ago, gone now. Dropping the connection is the whole recovery.
      LOG_DEBUG("clipboard lane: session for \"%s\" vanished before attach", peer.c_str());
      return;
    }

    // A superseding lane replaces the old one (§6). The old worker is stopped outside the lock.
    displaced = std::move(found->second.m_lane);

    auto fresh = std::make_unique<Lane>();
    fresh->m_peer = peer;
    fresh->m_conn = std::move(conn);
    // Everything the session was holding while it had no lane goes out on this one. Done WITHOUT
    // taking fresh->m_mutex because the worker that would contend for it does not exist yet -- it
    // is started below, deliberately last, so that it never sees a half-built lane.
    fresh->m_pending = std::move(found->second.m_held);
    found->second.m_held.clear();
    lane = fresh.get();
    found->second.m_lane = std::move(fresh);
  }
  stopLane(std::move(displaced), "a newer lane connection superseded it");

  // Started last, so the worker never sees a half-built lane.
  lane->m_worker = std::thread([this, lane] { runLane(lane); });
  // INFO for the same reason as the "down" line: on a deployed fleet the core's log level is the
  // default, so this pair is the ONLY evidence that the clipboard is on its dedicated connection at
  // all. Two lines per peer per session.
  LOG_INFO("clipboard lane up for \"%s\" %s", peer.c_str(), lane->m_conn->describe().c_str());
}

bool ClipboardLaneManager::hasLane(const std::string &peer)
{
  std::unique_ptr<Lane> doomed;
  bool live = false;
  {
    std::scoped_lock lock{m_mutex};
    if (const auto found = m_sessions.find(peer); found != m_sessions.end() && found->second.m_lane) {
      if (found->second.m_lane->m_finished) {
        doomed = std::move(found->second.m_lane);
      } else {
        live = true;
      }
    }
  }
  stopLane(std::move(doomed), "its worker stopped"); // lazy reap of a worker that has already given up
  return live;
}

ClipboardLaneManager::SendResult ClipboardLaneManager::send(
    const std::string &peer, ClipboardID id, uint32_t sequenceNumber, std::shared_ptr<const std::string> payload
)
{
  if (!payload) {
    return SendResult::NoLane;
  }
  if (payload->size() > m_maxPayloadBytes) {
    return SendResult::TooLarge;
  }

  std::unique_ptr<Lane> doomed;
  SendResult result = SendResult::NoLane;
  {
    std::scoped_lock lock{m_mutex};
    if (const auto found = m_sessions.find(peer); found != m_sessions.end()) {
      Session &session = found->second;

      // A worker that has already given up is not a lane. Reaped here (joined outside the lock,
      // below) so that this payload goes to the holding queue instead of into a dead lane's
      // pending map, where nothing would ever pick it up again.
      if (session.m_lane && session.m_lane->m_finished) {
        doomed = std::move(session.m_lane);
      }

      // Latest-wins: assignment, not append, in BOTH queues. This IS the bound on memory, and it
      // is also what kills the reconnect-and-resend-from-zero pathology -- what a late lane
      // finally sends is the newest state, not a backlog of history. The sequence number travels
      // WITH the payload it belongs to, so a superseded copy takes its stale one away with it.
      if (session.m_lane) {
        Lane &lane = *session.m_lane;
        std::scoped_lock queueLock{lane.m_mutex};
        lane.m_pending[id] = Payload{sequenceNumber, std::move(payload)};
        result = SendResult::Queued;
      } else {
        // The session exists but its lane does not (not yet, or not any more). RETAINED, not
        // dropped: the caller cleared its dirty flag when it called us and nothing re-offers a
        // clipboard that has not changed, so dropping here loses it outright. See send()'s header.
        session.m_held[id] = Payload{sequenceNumber, std::move(payload)};
        result = SendResult::Held;
      }
    }
  }
  stopLane(std::move(doomed), "its worker stopped");
  return result;
}

void ClipboardLaneManager::runLane(Lane *lane)
{
  // An exception escaping a std::thread's entry function calls std::terminate -- i.e. a clipboard
  // problem would take the whole core down, which is the exact opposite of this feature's one
  // non-negotiable property. A lane may only ever fail by going quiet. The realistic candidate is
  // bad_alloc on a genuinely large clipboard.
  try {
    runLaneBody(lane);
  } catch (const std::exception &e) {
    LOG_WARN("clipboard lane \"%s\" failed: %s", lane->m_peer.c_str(), e.what());
  } catch (...) {
    LOG_WARN("clipboard lane \"%s\" failed", lane->m_peer.c_str());
  }

  if (mtDiagEnabled()) {
    LOG_NOTE(
        "clipdiag: lane worker for \"%s\" exiting: %s", lane->m_peer.c_str(),
        lane->m_conn && lane->m_conn->deadReason() != nullptr ? lane->m_conn->deadReason() : "asked to stop"
    );
  }

  // Last act, and the only thing the main thread reads from here: it may now stop and join us.
  lane->m_finished = true;
}

void ClipboardLaneManager::runLaneBody(Lane *lane)
{
  // Before ANY io, and for this thread's whole life. A REFUSAL is news at WARN, not a detail: this
  // worker is about to stream tens of megabytes inside a process running at REALTIME priority
  // class, and the demotion is the only thing keeping it off the input relay's back. See
  // lane::demoteCurrentThreadToBackground().
  if (!lane::demoteCurrentThreadToBackground()) {
    LOG_WARN(
        "clipboard lane \"%s\": worker thread NOT demoted to background scheduling; large clipboards "
        "may compete with input relay on this machine",
        lane->m_peer.c_str()
    );
  }

  // Read ONCE per worker: the switch is a file/env check that never changes within a process, and
  // per-chunk logging must not add a per-chunk syscall of its own. Same sentinel ClipboardChunk uses
  // (a `clipdiag` file in the core's working directory, or MOUSETRANSFER_CLIPDIAG=1) -- deliberately
  // a FILE, because the elevated core's environment is frozen at task-registration time.
  const bool diag = mtDiagEnabled();
  if (diag) {
    LOG_NOTE("clipdiag: lane worker started for \"%s\" %s", lane->m_peer.c_str(), lane->m_conn->describe().c_str());
  }

  LaneConn &conn = *lane->m_conn;

  // ---- inbound state (worker-private; nothing else may look at it) ----
  uint8_t rxHeader[kFrameHeaderSize];
  uint32_t rxHeaderGot = 0;
  uint8_t rxType = 0;
  uint32_t rxBodyLen = 0;
  std::string rxBody;
  bool rxInBody = false;

  struct Assembly
  {
    bool active = false;
    uint32_t seq = 0;     ///< the sender's TRAIN counter: matches Data/End frames to this Start
    uint32_t clipSeq = 0; ///< the sender's APPLICATION clipboard sequence number: reported upwards
    uint32_t total = 0;
    std::string data;
    std::chrono::steady_clock::time_point started; ///< when this train's Start landed (for the summary)
  };
  Assembly assembly[kClipboardEnd];

  // ---- outbound state ----
  std::string txFrame; // the frame currently going out, byte-for-byte
  uint32_t txSent = 0;
  bool txTrainActive = false;
  ClipboardID txId = 0;
  uint32_t txSeq = 0;     // train counter, for framing only
  uint32_t txClipSeq = 0; // the application sequence number this payload was queued with
  // SHARED and immutable: the same buffer the queue holds, and the same one every other peer's
  // worker is sending. Nothing here ever writes through it.
  std::shared_ptr<const std::string> txPayload;
  uint32_t txOffset = 0;

  // One INFO line per completed OUTBOUND train, emitted when its End frame has actually reached the
  // wire rather than when it was produced -- "sent" must mean sent. These three carry the summary
  // across the gap between producing the End frame and finishing the write of it.
  bool txEndOnWire = false;
  uint32_t txTrainBytes = 0;
  std::chrono::steady_clock::time_point txTrainStart;

  // NOTHING is sent here. A lane is SILENT from the moment it is attached until one end has a
  // clipboard to send, and that is a correctness property, not politeness.
  //
  // The first draft had this end greet its peer with a LaneFrame::Ack, so that the dialling end
  // could tell "attached" from "still connecting". It cannot: whichever end speaks first breaks the
  // OTHER end's handoff, because the handoff is only legal while the connection is quiescent.
  //
  //   * An Ack from the DIALLING end can reach the accepting end before its event loop has read the
  //     lane greeting. PacketStreamFilter::readMore() drains everything available, so those bytes
  //     land in the filter's buffer and hasBufferedInput() refuses the handoff -- "the peer
  //     pipelined data behind its greeting", which it did not.
  //   * An Ack from the ACCEPTING end can reach the dialling end before IT has detached. The
  //     dialling end detaches on StreamOutputFlushed, which is an event-queue hop AFTER the bytes
  //     went out, so on a busy client the round trip finishes first; the Ack then sits in the
  //     socket's input buffer and detachTls() refuses -- correctly, since it would be lost.
  //
  // Both would have been intermittent, load-dependent and self-healing-with-a-dropped-clipboard,
  // which is the worst way for a bug to present. Silence removes the class. LaneFrame::Ack stays
  // defined and an unknown frame type is skipped by its length, so a later version can reintroduce
  // a greeting once it has somewhere safe to put it (after both ends are detached, not during).

  const auto abandonTrain = [&]() {
    txTrainActive = false;
    txPayload.reset();
    txOffset = 0;
  };

  while (!lane->m_stop && !conn.dead()) {
    // Is there anything to write, or anything worth writing next?
    bool pendingWork = false;
    if (txFrame.empty() && !txTrainActive) {
      std::scoped_lock queueLock{lane->m_mutex};
      pendingWork = !lane->m_pending.empty();
    }
    const bool wantWrite = !txFrame.empty() || txTrainActive || pendingWork;

    const unsigned ready = conn.waitReady(true, wantWrite, kPollSeconds);
    if (lane->m_stop) {
      break;
    }

    // ---------------- read ----------------
    if ((ready & LaneConn::Ready::Read) != 0) {
      uint8_t buffer[kReadBufferSize];
      const int got = conn.readSome(buffer, sizeof(buffer));
      if (got < 0) {
        break;
      }
      for (int offset = 0; offset < got;) {
        if (!rxInBody) {
          const uint32_t want = kFrameHeaderSize - rxHeaderGot;
          const uint32_t take = std::min<uint32_t>(want, static_cast<uint32_t>(got - offset));
          memcpy(rxHeader + rxHeaderGot, buffer + offset, take);
          rxHeaderGot += take;
          offset += static_cast<int>(take);
          if (rxHeaderGot < kFrameHeaderSize) {
            continue;
          }
          rxType = rxHeader[0];
          rxBodyLen = getU32(rxHeader + 1);
          rxHeaderGot = 0;
          // A length is the one number a peer can make us allocate from, so it is bounded before a
          // single byte is reserved. kLaneMaxFrameBody is derived from the chunk size, not chosen.
          if (rxBodyLen > kLaneMaxFrameBody) {
            LOG_DEBUG(
                "clipboard lane \"%s\": frame body of %u byte(s) exceeds the maximum; dropping the lane",
                lane->m_peer.c_str(), rxBodyLen
            );
            conn.shutdown();
            break;
          }
          rxBody.clear();
          rxBody.reserve(rxBodyLen);
          rxInBody = true;
        }

        if (rxInBody) {
          const uint32_t want = rxBodyLen - static_cast<uint32_t>(rxBody.size());
          const uint32_t take = std::min<uint32_t>(want, static_cast<uint32_t>(got - offset));
          rxBody.append(reinterpret_cast<const char *>(buffer + offset), take);
          offset += static_cast<int>(take);
          if (rxBody.size() < rxBodyLen) {
            continue;
          }
          rxInBody = false;

          // ---- a whole frame ----
          const auto *body = reinterpret_cast<const uint8_t *>(rxBody.data());
          if (rxType == LaneFrame::Start && rxBody.size() >= kLaneStartBodySize) {
            const ClipboardID id = body[0];
            const uint32_t seq = getU32(body + 1);
            const uint32_t total = getU32(body + 5);
            const uint32_t clipSeq = getU32(body + 9);
            if (total > kLaneMaxPayloadBytes) {
              // BEYOND THE FORK'S ABSOLUTE CEILING, and therefore not a configuration disagreement:
              // no conforming sender can ever emit this, because every sender applies the same
              // ceiling through ClipboardLaneManager::send(). Treated as a broken or hostile peer
              // and the lane is dropped BEFORE the reserve() below allocates anything -- which is
              // the whole point of the ceiling, since `total` is a uint32 a peer chose and the
              // configured limit it is otherwise checked against defaults to ~2 TB.
              //
              // Formatted through m_downDetail rather than LaneConn::markDead() because the numbers
              // are the news here, and deadReason() carries literals only. stopLane() prefers this,
              // so it comes out on the INFO "clipboard lane down" line.
              char detail[160];
              snprintf(
                  detail, sizeof(detail),
                  "the peer announced a %u byte clipboard, above this build's ceiling of %llu bytes", total,
                  static_cast<unsigned long long>(kLaneMaxPayloadBytes)
              );
              lane->m_downDetail = detail;
              LOG_WARN(
                  "clipboard lane \"%s\": %s; dropping the lane", lane->m_peer.c_str(), lane->m_downDetail.c_str()
              );
              conn.shutdown();
              break;
            }
            if (id >= kClipboardEnd || total > m_maxPayloadBytes) {
              // Inside the ceiling but over the CONFIGURED limit (or a clipboard id we do not have).
              // A legitimate disagreement -- two peers can be configured differently -- so the train
              // is refused and the lane stays up, exactly as before.
              LOG_DEBUG(
                  "clipboard lane \"%s\": refusing clipboard %d of %u byte(s)", lane->m_peer.c_str(),
                  static_cast<int>(id), total
              );
            } else {
              // A new START discards whatever was half-assembled for this id -- that is exactly how
              // the sender's supersede works: it stops mid-train and starts a fresh one.
              assembly[id] = Assembly{true, seq, clipSeq, total, std::string(), std::chrono::steady_clock::now()};
              if (diag) {
                LOG_NOTE(
                    "clipdiag: lane \"%s\" rx START clipboard %d, %u byte(s), train=%u seqnum=%u",
                    lane->m_peer.c_str(), static_cast<int>(id), total, seq, clipSeq
                );
              }
              // Reserve the WHOLE announced total, which the branch above has just bounded by
              // m_maxPayloadBytes -- the same configured clipboard limit the sender obeys. Growing
              // instead costs a full geometric-growth sequence: reaching 64 MiB from a 256 KB start
              // is roughly a dozen reallocations, ~2x the payload in memcpy, and a transient peak of
              // the old buffer plus a 1.5x-larger new one, all on a background-priority thread.
              //
              // This is not a new way to make us allocate: `total` was already trusted enough to
              // decide whether to accept the train at all, the lane is authenticated (token +
              // matching peer certificate), and a peer that wanted us to hold that much memory could
              // simply send it. The earlier caution here was about the case with NO configured
              // limit, where the ceiling is 4 GB -- but that is a configuration problem, and it is
              // the one thing this reserve now makes loud instead of silent.
              assembly[id].data.reserve(total);
            }
          } else if (rxType == LaneFrame::Data && rxBody.size() >= kLaneDataHeaderSize) {
            const ClipboardID id = body[0];
            const uint32_t seq = getU32(body + 1);
            const uint32_t at = getU32(body + 5);
            const char *chunk = rxBody.data() + kLaneDataHeaderSize;
            const size_t chunkSize = rxBody.size() - kLaneDataHeaderSize;
            if (id < kClipboardEnd && assembly[id].active && assembly[id].seq == seq &&
                at == assembly[id].data.size() && assembly[id].data.size() + chunkSize <= assembly[id].total) {
              assembly[id].data.append(chunk, chunkSize);
              if (diag) {
                // Per-CHUNK, so it is gated: a 64 MiB clipboard is 256 of these. SSL_pending is the
                // probe MT-CLIPBOARD-LANE-DESIGN.md §9b asked for -- anything but 0 means OpenSSL is
                // holding plaintext select() will never tell us about.
                LOG_NOTE(
                    "clipdiag: lane \"%s\" rx DATA clipboard %d, +%u at %u of %u, ssl_pending=%u",
                    lane->m_peer.c_str(), static_cast<int>(id), static_cast<uint32_t>(chunkSize), at,
                    assembly[id].total, static_cast<uint32_t>(conn.pendingPlaintext())
                );
              }
            } else if (id < kClipboardEnd) {
              // Out of order or from an abandoned train: drop the assembly rather than guess.
              assembly[id].active = false;
            }
          } else if (rxType == LaneFrame::End && rxBody.size() >= kLaneEndBodySize) {
            const ClipboardID id = body[0];
            const uint32_t seq = getU32(body + 1);
            if (id < kClipboardEnd && assembly[id].active && assembly[id].seq == seq &&
                assembly[id].data.size() == assembly[id].total) {
              auto *info = new LaneClipboardInfo;
              info->m_peer = lane->m_peer;
              info->m_id = id;
              // The APPLICATION sequence number from the START frame, never the train counter --
              // see ClipboardLaneManager::send().
              info->m_sequenceNumber = assembly[id].clipSeq;
              info->m_data = std::move(assembly[id].data);
              // ONE line per completed transfer, at INFO. This is the receiving half of the pair a
              // fleet log needs: with the sender's matching line it bounds the transfer at both
              // ends, which is what turns "the clipboard did not arrive" into a direction.
              const auto elapsed =
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - assembly[id].started).count();
              LOG_INFO(
                  "clipboard lane \"%s\": received clipboard %d, %u byte(s) in %.1fs", lane->m_peer.c_str(),
                  static_cast<int>(id), assembly[id].total, elapsed
              );
              // Thread-safe hand-off to the main thread, where the apply is allowed to happen.
              // The cast is load-bearing, not decoration: Event has both an `EventData *` and a
              // `void *data` constructor, and picking the wrong one would compile silently and then
              // fail twice -- getDataObject() would be null so every payload would be dropped, and
              // deleteData() would free() a new'd non-POD.
              m_events->addEvent(Event(EventTypes::ClipboardLaneReceived, this, static_cast<EventData *>(info)));
            }
            if (id < kClipboardEnd) {
              assembly[id] = Assembly{};
            }
          }
          // Any other type -- including Ping/Pong and anything a future version invents -- is
          // skipped using its own length. That is what makes new frame types additive.
        }
      }
      if (conn.dead()) {
        break;
      }
    }

    // ---------------- write ----------------
    if ((ready & LaneConn::Ready::Write) == 0) {
      continue;
    }

    // Nothing half-written? Produce the next frame of the current train, or start a new one.
    if (txFrame.empty()) {
      if (txTrainActive) {
        // Has something newer for this clipboard arrived? Then this train is stale: abandon it
        // between chunks and let the next loop pick the new payload up. The receiver discards its
        // partial assembly when the new START lands, so no END is needed or wanted.
        bool superseded = false;
        {
          std::scoped_lock queueLock{lane->m_mutex};
          superseded = lane->m_pending.count(txId) != 0;
        }
        if (superseded) {
          LOG_DEBUG("clipboard lane \"%s\": clipboard %d superseded mid-send", lane->m_peer.c_str(),
                    static_cast<int>(txId));
          abandonTrain();
        } else if (txOffset >= txPayload->size()) {
          std::string body;
          putU8(body, txId);
          putU32(body, txSeq);
          txFrame = frame(LaneFrame::End, body);
          // The summary belongs to the moment this frame reaches the wire, not to now.
          txEndOnWire = true;
          abandonTrain();
        } else {
          const uint32_t take = std::min<uint32_t>(kLaneChunkSize, static_cast<uint32_t>(txPayload->size() - txOffset));
          // Built straight into txFrame, header and all, rather than into a `body` that frame() then
          // copies again: the payload slice is 256 KB, so the intermediate cost a second 256 KB
          // allocation and memcpy for every chunk -- 128 MB of pure copying across a 64 MiB
          // clipboard, for nothing. txFrame keeps its capacity between chunks (clear() does not
          // release it and reserve() will not shrink it), so after the first chunk this allocates
          // nothing at all.
          txFrame.clear();
          txFrame.reserve(kFrameHeaderSize + kLaneDataHeaderSize + take);
          putU8(txFrame, LaneFrame::Data);
          putU32(txFrame, kLaneDataHeaderSize + take);
          putU8(txFrame, txId);
          putU32(txFrame, txSeq);
          putU32(txFrame, txOffset);
          txFrame.append(*txPayload, txOffset, take);
          if (diag) {
            LOG_NOTE(
                "clipdiag: lane \"%s\" tx DATA clipboard %d, +%u at %u of %u", lane->m_peer.c_str(),
                static_cast<int>(txId), take, txOffset, static_cast<uint32_t>(txPayload->size())
            );
          }
          txOffset += take;
        }
      } else {
        Payload payload;
        ClipboardID id = 0;
        bool took = false;
        {
          std::scoped_lock queueLock{lane->m_mutex};
          if (const auto first = lane->m_pending.begin(); first != lane->m_pending.end()) {
            id = first->first;
            payload = std::move(first->second);
            lane->m_pending.erase(first);
            took = true;
          }
        }
        if (took) {
          txId = id;
          txClipSeq = payload.m_sequenceNumber;
          txPayload = std::move(payload.m_data);
          txOffset = 0;
          txSeq = ++lane->m_trainSeq;
          txTrainActive = true;
          txTrainBytes = static_cast<uint32_t>(txPayload->size());
          txTrainStart = std::chrono::steady_clock::now();
          std::string body;
          putU8(body, txId);
          putU32(body, txSeq);
          putU32(body, static_cast<uint32_t>(txPayload->size()));
          // The application's own sequence number, carried end to end so the receiver's
          // mis-sequence check sees the number the in-stream path would have given it.
          putU32(body, txClipSeq);
          txFrame = frame(LaneFrame::Start, body);
          LOG_DEBUG(
              "clipboard lane \"%s\": sending clipboard %d, %u byte(s), seqnum=%u", lane->m_peer.c_str(),
              static_cast<int>(txId), static_cast<uint32_t>(txPayload->size()), txClipSeq
          );
        }
      }
      txSent = 0;
    }

    if (!txFrame.empty()) {
      // Same buffer, same remaining length, every retry -- OpenSSL requires it, and txFrame is not
      // touched again until a write reports progress.
      const int put = conn.writeSome(txFrame.data() + txSent, static_cast<uint32_t>(txFrame.size() - txSent));
      if (put < 0) {
        break;
      }
      txSent += static_cast<uint32_t>(put);
      if (txSent >= txFrame.size()) {
        txFrame.clear();
        txSent = 0;
        if (txEndOnWire) {
          txEndOnWire = false;
          const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - txTrainStart).count();
          LOG_INFO(
              "clipboard lane \"%s\": sent clipboard %d, %u byte(s) in %.1fs", lane->m_peer.c_str(),
              static_cast<int>(txId), txTrainBytes, elapsed
          );
        }
      }
    }
  }
}

} // namespace deskflow
