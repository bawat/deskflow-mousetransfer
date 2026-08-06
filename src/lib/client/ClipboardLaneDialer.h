/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 MouseTransfer fork
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "net/NetworkAddress.h"
#include "net/SecurityLevel.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class EventQueueTimer;
class IDataSocket;
class IEventQueue;
class ISocketFactory;
namespace deskflow {
class IStream;
class LaneConn;
} // namespace deskflow

//! Opens the client's end of a clipboard lane
/*!
The server advertises a lane on the main connection (kMsgDLaneAdvert) and the client answers by
opening a SECOND TLS connection to the same listener, greeting it with kMsgMTLaneHello instead of a
HelloBack, and taking the raw connection away from the socket machinery once the server accepts it.
This class is that dial, and nothing else: it hands the finished connection to a callback and never
learns what happens to it. See MT-CLIPBOARD-LANE-DESIGN.md §9.

**Why it is written as an event-driven state machine rather than a routine.** Everything slow about
a dial -- the TCP connect, the TLS handshake, waiting for the greeting, waiting for our own greeting
to reach the wire -- already happens on the SocketMultiplexer's service thread, exactly as it does
for the main connection. What runs on the event-queue thread (the thread that also delivers the
server's input to the screen) is only the short callbacks between those waits: read eleven bytes,
write about forty, then detach. A dial that stalls, or a server that never answers, therefore costs
the input path nothing at all -- it simply never posts its next event. That property is the reason
not to "simplify" this into a blocking helper, and the reason not to reach for IStream::flush(),
which would park the input thread on a condition variable until the multiplexer drained the socket.

**A failure here is always local.** Every refusal closes this connection, schedules a retry on a
doubling backoff, and touches nothing else. The dialer holds no reference to the main connection
and cannot write to it, disconnect it, or delay it.

**The token is a secret.** It authorises one lane onto the main session and is never logged, not
even at DEBUG, not truncated and not hashed.
*/
class ClipboardLaneDialer
{
public:
  //! Called on the main thread with a lane connection that is up and ready to be attached
  using LaneReady = std::function<void(std::unique_ptr<deskflow::LaneConn>)>;

  //! Called with each freshly created socket, to apply whatever the main connection applies
  /*!
  In practice Client::bindNetworkInterface: on a deployment that pins traffic to one interface the
  lane must leave from the same one, and the owner of that policy is the client, not this class.
  */
  using PrepareSocket = std::function<void(IDataSocket *)>;

  /*!
  \p serverAddress must already be RESOLVED -- it is copied from the address the main connection
  succeeded on, so that a lane dial never performs name resolution (which blocks) on the event
  thread. \p screenName is this client's own screen name, which is how the server finds the session
  the token belongs to. \p securityLevel is the level the main connection uses.
  */
  ClipboardLaneDialer(
      IEventQueue *events, ISocketFactory *socketFactory, const NetworkAddress &serverAddress, std::string screenName,
      SecurityLevel securityLevel, PrepareSocket prepareSocket, LaneReady onLaneReady
  );
  ClipboardLaneDialer(const ClipboardLaneDialer &) = delete;
  ClipboardLaneDialer(ClipboardLaneDialer &&) = delete;
  ClipboardLaneDialer &operator=(const ClipboardLaneDialer &) = delete;
  ClipboardLaneDialer &operator=(ClipboardLaneDialer &&) = delete;

  //! Abandons any attempt in flight and forgets the token
  ~ClipboardLaneDialer();

  //! The server advertised a lane: take the token and dial at once
  /*!
  A fresh advert supersedes everything the dialer was doing -- any attempt in flight was for the
  previous token -- and resets the backoff, because a new advert means a new session rather than
  another failure of the old one.
  */
  void advertised(int16_t laneVersion, std::string token);

  //! Dial now if there is a token, nothing is already in flight and no retry is pending
  /*!
  The lazy half of the retry policy: the caller notices it has a clipboard and no lane, and says so.
  Idempotent and cheap -- it is a no-op in every state except "idle with a token".
  */
  void nudge();

private:
  void startDial();
  //! Give up this attempt, say why at DEBUG, and schedule the next one
  void abandonDial(const char *why);
  //! The connection is ours: take it out of the socket machinery and hand it over
  void completeDial();
  //! Destroy the attempt's socket, stream, handlers and timeout (safe in any state)
  void cleanupAttempt();
  void scheduleRetry();
  void cancelRetry();
  void cancelDialTimeout();

  // event handlers -- all short, all on the event-queue thread
  void handleConnected();
  void handleConnectionFailed();
  void handleInputReady();
  void handleOutputFlushed();
  void handleBroken(const char *why);
  void handleDialTimeout();
  void handleRetryTimer();

  IEventQueue *m_events = nullptr;
  ISocketFactory *m_socketFactory = nullptr;
  NetworkAddress m_serverAddress;
  std::string m_screenName;
  SecurityLevel m_securityLevel = SecurityLevel::PlainText;
  PrepareSocket m_prepareSocket;
  LaneReady m_onLaneReady;

  // The lane token for the CURRENT session. Empty means "no lane has been advertised", which is
  // also how a refused advert is remembered. NEVER logged.
  std::string m_token;

  // The attempt in flight, if any. m_stream owns nothing (the packet filter is built with
  // adopt=false) so that SecureSocket::fromStream() will look through it and so that the socket
  // survives the stream's destruction -- both halves of the handoff need that.
  IDataSocket *m_socket = nullptr;
  deskflow::IStream *m_stream = nullptr;
  EventQueueTimer *m_dialTimer = nullptr;
  bool m_greeted = false;

  EventQueueTimer *m_retryTimer = nullptr;
  double m_retryDelay = 0.0;
};
