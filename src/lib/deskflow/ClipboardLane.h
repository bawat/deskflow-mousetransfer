/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 MouseTransfer fork
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/Event.h"
#include "deskflow/ClipboardTypes.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class IEventQueue;

namespace deskflow {

class LaneConn;

//! A clipboard payload that arrived over a lane
/*!
Carried by EventTypes::ClipboardLaneReceived so that the APPLY happens on the main thread -- lane
workers must never touch proxy or screen state.
*/
class LaneClipboardInfo : public EventData
{
public:
  std::string m_peer;            ///< screen name of the sender
  ClipboardID m_id = 0;          ///< which clipboard
  uint32_t m_sequenceNumber = 0; ///< the sender's clipboard sequence number
  std::string m_data;            ///< marshalled clipboard, exactly as the legacy path would carry it
};

//! Manager for dedicated clipboard connections
/*!
Owns one "session" per peer (the token that authorises a lane plus the peer's main-connection TLS
fingerprint), and at most one live lane per session. Server-side there is one manager with a session
per client; client-side there will be one with a single session.

**Why any of this exists.** A large clipboard queued onto the main connection sits in front of the
keepalives, the peer's 9-second death timer expires, it declares the server dead and reconnects with
a dirty clipboard, and the next screen switch re-sends from byte zero -- a loop that walls the mesh
off permanently. Moving the bytes to their own connection, serviced by their own low-priority
thread, is the fix. See MT-CLIPBOARD-LANE-DESIGN.md.

**Threading.** Every public method belongs to the main/event thread. Each lane has exactly ONE
worker thread, which touches only its own lane object and IEventQueue::addEvent; it never reads the
session map and never calls back into anything that could take a lock the main thread holds. Workers
are joinable and always joined -- on closeSession(), on a superseding attach(), on the destructor,
and lazily when a dead one is noticed. Nothing is ever detached.

**A lane failure is never allowed to touch the main session.** The worst outcome of anything going
wrong in here is a logged "clipboard not delivered".
*/
class ClipboardLaneManager
{
public:
  //! What became of a payload handed to send()
  enum class SendResult : uint8_t
  {
    Queued,   ///< accepted; it will go out, or be superseded by something newer, which is equivalent
    NoLane,   ///< no lane for this peer (an older client, or the lane is down)
    TooLarge, ///< over the configured clipboard size limit; deliberately dropped
  };

  //! Invoked on the MAIN thread for each completed inbound payload
  using ReceiveHandler = std::function<void(const LaneClipboardInfo &)>;

  /*!
  \p maxPayloadBytes is the largest clipboard this endpoint will send or accept, and should track
  the server's configured clipboard size limit.
  */
  ClipboardLaneManager(IEventQueue *events, size_t maxPayloadBytes);
  ClipboardLaneManager(const ClipboardLaneManager &) = delete;
  ClipboardLaneManager(ClipboardLaneManager &&) = delete;
  ClipboardLaneManager &operator=(const ClipboardLaneManager &) = delete;
  ClipboardLaneManager &operator=(ClipboardLaneManager &&) = delete;
  ~ClipboardLaneManager();

  void setReceiveHandler(ReceiveHandler handler);
  void setMaxPayloadBytes(size_t bytes);

  //! Arm a peer to open a lane; returns the fresh token to advertise (empty ⇒ no CSPRNG, no lane)
  /*!
  Replaces any previous session for \p peer, tearing down its lane -- a reconnecting client must not
  be able to use its previous session's token.

  \p peerFingerprint is the peer's TLS certificate fingerprint on the MAIN connection, or empty when
  the connection is not peer-authenticated. A lane must present the same one.
  */
  std::string openSession(const std::string &peer, const std::string &peerFingerprint);

  //! Create a session for a peer this endpoint DIALS rather than accepts
  /*!
  The end that opens a lane has nobody to validate. It authenticated its peer with TLS during the
  dial, and the token it presents is the peer's secret, not one of its own -- but attach() still
  needs a session to hang the lane on, so this creates one carrying no token and no fingerprint.

  Deliberately not "openSession() with a token nobody uses". A session made here can never
  authorise an INBOUND lane, because validate() compares tokens with lane::secretsEqual(), which
  refuses two empty secrets: the refusal is structural rather than a consequence of nobody happening
  to call validate() on this manager. It also does not depend on the CSPRNG, so a dialling endpoint
  cannot lose its lane to a randomness failure that has nothing to do with it.

  Replaces any previous session for \p peer and tears down its lane, so a fresh advert supersedes an
  old lane exactly as it does on the accepting side.
  */
  void openLocalSession(const std::string &peer);

  //! Forget a peer's session and tear down its lane
  void closeSession(const std::string &peer);

  //! Tear everything down (also done by the destructor)
  void closeAll();

  //! May this connection become \p peer's lane? Fills \p reason on refusal. Nothing is mutated.
  bool validate(
      const std::string &peer, const std::string &token, const std::string &peerFingerprint, int16_t laneVersion,
      std::string &reason
  ) const;

  //! Adopt a validated connection as \p peer's lane, replacing any previous one
  void attach(const std::string &peer, std::unique_ptr<LaneConn> conn);

  bool hasLane(const std::string &peer);

  //! Queue a marshalled clipboard for \p peer, latest-wins
  /*!
  At most ONE payload is ever pending per (peer, clipboard id): a newer copy replaces an
  undelivered older one, and a payload that arrives while its own clipboard id is mid-flight makes
  the worker abandon that train between chunks and start the new one. Memory is bounded by
  construction -- one pending payload per clipboard id per peer, plus one in flight.

  This is also why delivery may be decoupled from the caller's dirty-flag bookkeeping: the newest
  state is what the lane retains, so a lane that comes up late still delivers something current
  instead of replaying history.

  \p sequenceNumber is the APPLICATION's clipboard sequence number -- the same value the in-stream
  path puts in kMsgDClipboard -- and is carried through to the receiver untouched. It is emphatically
  NOT the lane's own train counter: Server::onClipboardChanged drops an update whose sequence number
  is lower than the one it last saw ("mis-sequenced"), and a per-lane counter starting at 1 loses
  that comparison against the server's running total for the rest of the session. Pass what the
  legacy path would have sent, which for a server->client clipboard is 0.
  */
  SendResult send(const std::string &peer, ClipboardID id, uint32_t sequenceNumber, std::string payload);

private:
  //! A clipboard waiting to go out, with the application sequence number it belongs to
  struct Payload
  {
    uint32_t m_sequenceNumber = 0;
    std::string m_data;
  };

  //! One live lane and its worker
  struct Lane
  {
    std::string m_peer;
    std::unique_ptr<LaneConn> m_conn;
    std::thread m_worker;

    std::mutex m_mutex;                          ///< guards m_pending
    std::map<ClipboardID, Payload> m_pending;    ///< at most one payload per clipboard id
    std::atomic<bool> m_stop{false};             ///< set by the main thread; polled by the worker
    std::atomic<bool> m_finished{false};         ///< set by the worker as its last act
    uint32_t m_trainSeq = 0;                     ///< worker-private train counter (FRAMING only)
  };

  //! What we know about a peer that is allowed to open a lane
  struct Session
  {
    std::string m_token;
    std::string m_fingerprint;
    std::unique_ptr<Lane> m_lane;
  };

  //! Install a session, returning the lane it displaced so the CALLER can stop it outside the lock
  /*!
  The one place a session is created or replaced. Joining a worker while holding m_sessions would
  block every other caller for as long as the join takes, so the displaced lane is handed back
  rather than stopped here.
  */
  std::unique_ptr<Lane> installSession(const std::string &peer, std::string token, std::string fingerprint);

  //! Worker entry point. Catches EVERYTHING: an escaping exception would call std::terminate.
  void runLane(Lane *lane);
  //! The actual loop, free to throw
  void runLaneBody(Lane *lane);
  //! Raise the stop flag, unblock the wait, join the worker, then let the connection go
  static void stopLane(std::unique_ptr<Lane> lane);

  IEventQueue *m_events = nullptr;
  std::atomic<size_t> m_maxPayloadBytes;
  ReceiveHandler m_receiveHandler;

  mutable std::mutex m_mutex; ///< guards m_sessions
  std::map<std::string, Session> m_sessions;
};

} // namespace deskflow
