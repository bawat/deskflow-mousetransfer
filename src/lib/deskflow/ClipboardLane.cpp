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

void putU16(std::string &out, uint16_t value)
{
  out.push_back(static_cast<char>(value & 0xff));
  out.push_back(static_cast<char>((value >> 8) & 0xff));
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
      m_maxPayloadBytes(maxPayloadBytes)
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
  m_maxPayloadBytes = bytes;
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

  std::unique_ptr<Lane> displaced;
  std::string issued;
  {
    std::scoped_lock lock{m_mutex};
    Session &session = m_sessions[peer];
    // A reconnecting client gets a brand new token, and its previous lane goes with the previous
    // session -- an old token must never open a lane onto a new session.
    displaced = std::move(session.m_lane);
    session.m_token.assign(reinterpret_cast<const char *>(token.data()), token.size());
    session.m_fingerprint = peerFingerprint;
    issued = session.m_token;
  }
  // Outside the lock: joining a worker must never happen with the session map held.
  stopLane(std::move(displaced));

  LOG_DEBUG(
      "clipboard lane: session opened for \"%s\" (peer authenticated: %s)", peer.c_str(),
      peerFingerprint.empty() ? "no" : "yes"
  );
  return issued;
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
  stopLane(std::move(doomed));
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
    stopLane(std::move(entry.second.m_lane));
  }
}

void ClipboardLaneManager::stopLane(std::unique_ptr<Lane> lane)
{
  if (!lane) {
    return;
  }

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
  LOG_DEBUG("clipboard lane: down for \"%s\"", lane->m_peer.c_str());
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
    lane = fresh.get();
    found->second.m_lane = std::move(fresh);
  }
  stopLane(std::move(displaced));

  // Started last, so the worker never sees a half-built lane.
  lane->m_worker = std::thread([this, lane] { runLane(lane); });
  LOG_DEBUG("clipboard lane: up for \"%s\" %s", peer.c_str(), lane->m_conn->describe().c_str());
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
  stopLane(std::move(doomed)); // lazy reap of a worker that has already given up
  return live;
}

ClipboardLaneManager::SendResult
ClipboardLaneManager::send(const std::string &peer, ClipboardID id, uint32_t sequenceNumber, std::string payload)
{
  if (payload.size() > m_maxPayloadBytes) {
    return SendResult::TooLarge;
  }

  std::unique_ptr<Lane> doomed;
  SendResult result = SendResult::NoLane;
  {
    std::scoped_lock lock{m_mutex};
    if (const auto found = m_sessions.find(peer); found != m_sessions.end() && found->second.m_lane) {
      if (found->second.m_lane->m_finished) {
        doomed = std::move(found->second.m_lane);
      } else {
        Lane &lane = *found->second.m_lane;
        std::scoped_lock queueLock{lane.m_mutex};
        // Latest-wins: assignment, not append. This IS the bound on memory, and it is also what
        // kills the reconnect-and-resend-from-zero pathology -- what a late lane finally sends is
        // the newest state, not a backlog of history. The sequence number travels WITH the payload
        // it belongs to, so a superseded copy takes its stale sequence number away with it.
        lane.m_pending[id] = Payload{sequenceNumber, std::move(payload)};
        result = SendResult::Queued;
      }
    }
  }
  stopLane(std::move(doomed));
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

  // Last act, and the only thing the main thread reads from here: it may now stop and join us.
  lane->m_finished = true;
}

void ClipboardLaneManager::runLaneBody(Lane *lane)
{
  // Before ANY io, and for this thread's whole life.
  lane::demoteCurrentThreadToBackground();

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
  };
  Assembly assembly[kClipboardEnd];

  // ---- outbound state ----
  std::string txFrame; // the frame currently going out, byte-for-byte
  uint32_t txSent = 0;
  bool txTrainActive = false;
  ClipboardID txId = 0;
  uint32_t txSeq = 0;     // train counter, for framing only
  uint32_t txClipSeq = 0; // the application sequence number this payload was queued with
  std::string txPayload;
  uint32_t txOffset = 0;

  // The server speaks first on a lane, so the client can tell "attached" from "still connecting".
  {
    std::string body;
    putU16(body, static_cast<uint16_t>(kLaneWireVersion));
    txFrame = frame(LaneFrame::Ack, body);
  }

  const auto abandonTrain = [&]() {
    txTrainActive = false;
    txPayload.clear();
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
            if (id >= kClipboardEnd || total > m_maxPayloadBytes) {
              LOG_DEBUG(
                  "clipboard lane \"%s\": refusing clipboard %d of %u byte(s)", lane->m_peer.c_str(),
                  static_cast<int>(id), total
              );
            } else {
              // A new START discards whatever was half-assembled for this id -- that is exactly how
              // the sender's supersede works: it stops mid-train and starts a fresh one.
              assembly[id] = Assembly{true, seq, clipSeq, total, std::string()};
              // Reserve a first chunk's worth rather than the announced total. `total` is a number
              // the PEER chose, and with no configured clipboard limit the ceiling on it is 4 GB --
              // reserving that up front would throw before a single byte of it had arrived. append()
              // grows geometrically, so the cost of not front-loading is a few reallocations on a
              // payload that is genuinely large.
              assembly[id].data.reserve(std::min<uint32_t>(total, kLaneChunkSize));
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
              LOG_DEBUG(
                  "clipboard lane \"%s\": received clipboard %d, %u byte(s)", lane->m_peer.c_str(),
                  static_cast<int>(id), assembly[id].total
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
        } else if (txOffset >= txPayload.size()) {
          std::string body;
          putU8(body, txId);
          putU32(body, txSeq);
          txFrame = frame(LaneFrame::End, body);
          abandonTrain();
        } else {
          const uint32_t take = std::min<uint32_t>(kLaneChunkSize, static_cast<uint32_t>(txPayload.size() - txOffset));
          std::string body;
          body.reserve(kLaneDataHeaderSize + take);
          putU8(body, txId);
          putU32(body, txSeq);
          putU32(body, txOffset);
          body.append(txPayload, txOffset, take);
          txOffset += take;
          txFrame = frame(LaneFrame::Data, body);
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
          std::string body;
          putU8(body, txId);
          putU32(body, txSeq);
          putU32(body, static_cast<uint32_t>(txPayload.size()));
          // The application's own sequence number, carried end to end so the receiver's
          // mis-sequence check sees the number the in-stream path would have given it.
          putU32(body, txClipSeq);
          txFrame = frame(LaneFrame::Start, body);
          LOG_DEBUG(
              "clipboard lane \"%s\": sending clipboard %d, %u byte(s), seqnum=%u", lane->m_peer.c_str(),
              static_cast<int>(txId), static_cast<uint32_t>(txPayload.size()), txClipSeq
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
      }
    }
  }
}

} // namespace deskflow
