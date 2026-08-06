/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 MouseTransfer fork
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "client/ClipboardLaneDialer.h"

#include "arch/Arch.h"
#include "base/EventQueueTimer.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/DeskflowException.h"
#include "deskflow/PacketStreamFilter.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "io/IStream.h"
#include "net/IDataSocket.h"
#include "net/ISocketFactory.h"
#include "net/LaneConn.h"
#include "net/SecureSocket.h"

#include <algorithm>
#include <cassert>
#include <exception>
#include <utility>

namespace {

//! How long an entire dial may take before it is abandoned
/*!
Sourced from the server's own patience rather than picked: ClientListener gives a connection that
has not yet identified itself kUnknownClientTimeout seconds before destroying its proxy. A dial
still unfinished at that point is talking to something the server has already thrown away, so
waiting any longer cannot succeed -- it can only delay the retry that might.
*/
constexpr double kDialTimeout = kUnknownClientTimeout;

//! Delay before the first retry after a failed dial
/*!
One keep-alive period. Fast enough that a clipboard copied a moment after a hiccup still finds a
lane, slow enough that a server refusing every dial cannot turn into a connection storm -- which
matters more than it sounds, because this fork's history includes an outage caused by a per-address
connection fan-out saturating a gateway.
*/
constexpr double kRetryFirst = kKeepAliveRate;

//! Ceiling the doubling backoff stops at
/*!
The same kUnknownClientTimeout again: retrying more often than the server would have given up on the
previous attempt gains nothing, and a lane that is failing structurally (an option the server has
turned off, a firewall that only permits one connection) should settle into costing almost nothing.
*/
constexpr double kRetryMax = kUnknownClientTimeout;

} // namespace

ClipboardLaneDialer::ClipboardLaneDialer(
    IEventQueue *events, ISocketFactory *socketFactory, const NetworkAddress &serverAddress, std::string screenName,
    SecurityLevel securityLevel, PrepareSocket prepareSocket, LaneReady onLaneReady
)
    : m_events(events),
      m_socketFactory(socketFactory),
      m_serverAddress(serverAddress),
      m_screenName(std::move(screenName)),
      m_securityLevel(securityLevel),
      m_prepareSocket(std::move(prepareSocket)),
      m_onLaneReady(std::move(onLaneReady)),
      m_retryDelay(kRetryFirst)
{
  assert(m_events != nullptr);
  assert(m_socketFactory != nullptr);
  assert(m_onLaneReady != nullptr);
}

ClipboardLaneDialer::~ClipboardLaneDialer()
{
  cancelRetry();
  cleanupAttempt();
  // m_token dies with us. Nothing else here outlives this object: the attempt's socket, stream,
  // handlers and timers are all gone by now, and a lane that was successfully handed over belongs
  // to the lane manager, which owns its worker.
}

void ClipboardLaneDialer::advertised(int16_t laneVersion, std::string token)
{
  // An advert for a lane format we do not speak is not a failure to retry -- retrying would produce
  // the same refusal for as long as the session lasts -- so the token is dropped and the client
  // simply has no lane. The server refuses the mismatch too (ClipboardLaneManager::validate); this
  // just declines to make it say so.
  if (laneVersion != kLaneWireVersion) {
    LOG_DEBUG(
        "clipboard lane: server offers lane wire version %d, we speak %d; not dialling", laneVersion, kLaneWireVersion
    );
    m_token.clear();
    cancelRetry();
    cleanupAttempt();
    return;
  }

  // A fresh advert means a fresh session on the server, so anything in flight was for a token that
  // is now dead, and the backoff earned by the old session's failures does not carry over.
  cancelRetry();
  cleanupAttempt();
  m_token = std::move(token);
  m_retryDelay = kRetryFirst;
  startDial();
}

void ClipboardLaneDialer::nudge()
{
  // Deliberately a no-op unless we are idle with a token: an attempt in flight must not be
  // restarted (it would drop a connection the server may be about to accept) and a pending retry
  // must not be pulled forward (that is what the backoff is for).
  if (m_stream != nullptr || m_retryTimer != nullptr) {
    return;
  }
  startDial();
}

void ClipboardLaneDialer::startDial()
{
  if (m_token.empty() || m_stream != nullptr) {
    return;
  }
  cancelRetry();

  // A lane is a TLS connection or it is nothing. The handoff has to give the lane worker an SSL
  // object, and the server refuses to detach anything else ("not a tls connection"), so dialling
  // an unencrypted connection could only ever produce a refusal. Declining here rather than
  // learning it a round trip later also means an unencrypted deployment opens no extra sockets at
  // all. See MT-CLIPBOARD-LANE-DESIGN.md §3.4.
  if (m_securityLevel == SecurityLevel::PlainText) {
    LOG_DEBUG("clipboard lane: the connection to the server is not encrypted; no lane is possible");
    m_token.clear();
    return;
  }

  try {
    // The address was resolved by the main connection and copied here, so this path never performs
    // name resolution -- which blocks, and would block the thread that delivers input.
    m_socket = m_socketFactory->create(ARCH->getAddrFamily(m_serverAddress.getAddress()), m_securityLevel);
    if (m_prepareSocket) {
      m_prepareSocket(m_socket);
    }

    // adopt = false, for two reasons that both matter: SecureSocket::fromStream() refuses to look
    // through a filter that has adopted its stream (that is its "we are not allowed to take this
    // apart" check), and the socket has to outlive the stream during the handoff.
    m_stream = new PacketStreamFilter(m_events, m_socket, false);

    void *target = m_stream->getEventTarget();
    // Only the SECURE connected event is watched: a plaintext dial was refused above.
    m_events->addHandler(EventTypes::DataSocketSecureConnected, target, [this](const auto &) { handleConnected(); });
    m_events->addHandler(EventTypes::DataSocketConnectionFailed, target, [this](const Event &event) {
      // The connection-failed event carries an owned info object that its handler is expected to
      // free (the event is posted with DontFreeData). Freed before anything else, because
      // handleConnectionFailed() destroys this handler on its way through.
      delete static_cast<IDataSocket::ConnectionFailedInfo *>(event.getData());
      handleConnectionFailed();
    });
    m_events->addHandler(EventTypes::StreamInputReady, target, [this](const auto &) { handleInputReady(); });
    m_events->addHandler(EventTypes::StreamOutputFlushed, target, [this](const auto &) { handleOutputFlushed(); });
    m_events->addHandler(EventTypes::StreamInputShutdown, target, [this](const auto &) {
      handleBroken("the server closed the lane connection");
    });
    m_events->addHandler(EventTypes::StreamOutputShutdown, target, [this](const auto &) {
      handleBroken("the server closed the lane connection");
    });
    m_events->addHandler(EventTypes::StreamOutputError, target, [this](const auto &) {
      handleBroken("error writing to the lane connection");
    });
    m_events->addHandler(EventTypes::SocketDisconnected, target, [this](const auto &) {
      handleBroken("the lane connection dropped");
    });

    // Bounds the whole dial, including a connect that neither succeeds nor fails.
    m_dialTimer = m_events->newOneShotTimer(kDialTimeout, nullptr);
    m_events->addHandler(EventTypes::Timer, m_dialTimer, [this](const auto &) { handleDialTimeout(); });

    LOG_DEBUG(
        "clipboard lane: dialling %s:%d", ARCH->addrToString(m_serverAddress.getAddress()).c_str(),
        m_serverAddress.getPort()
    );
    m_socket->connect(m_serverAddress);
  } catch (const BaseException &e) {
    abandonDial(e.what());
  } catch (const std::exception &e) {
    abandonDial(e.what());
  } catch (...) {
    abandonDial("unknown error");
  }
}

void ClipboardLaneDialer::handleConnected()
{
  LOG_DEBUG1("clipboard lane: connected, waiting for the server's greeting");
}

void ClipboardLaneDialer::handleConnectionFailed()
{
  abandonDial("could not connect");
}

void ClipboardLaneDialer::handleBroken(const char *why)
{
  abandonDial(why);
}

void ClipboardLaneDialer::handleDialTimeout()
{
  abandonDial("the server did not accept the lane in time");
}

void ClipboardLaneDialer::handleInputReady()
{
  if (m_stream == nullptr) {
    return;
  }

  try {
    if (m_greeted) {
      // We have already said the only thing a lane connection is allowed to say. Anything arriving
      // now is the server talking on a connection it should be handing over in silence -- in
      // practice a refusal, whose reason is in the SERVER's log at DEBUG, not decodable here.
      abandonDial("the server answered our greeting instead of accepting the lane");
      return;
    }

    // The listener greets every new connection with kMsgHello before it can possibly know what the
    // connection is for. Read it and throw it away: replying with a HelloBack is what would make
    // this a second KVM client, which is precisely what a lane is not.
    std::string protocolName;
    int16_t serverMajor = 0;
    int16_t serverMinor = 0;
    if (!ProtocolUtil::readf(m_stream, kMsgHello, &protocolName, &serverMajor, &serverMinor)) {
      abandonDial("no greeting from the server");
      return;
    }
    LOG_DEBUG1("clipboard lane: greeted by %s %d.%d", protocolName.c_str(), serverMajor, serverMinor);

    // The lane greeting, and NOTHING after it until the server answers in lane frames. The server
    // refuses the handoff outright if anything is pipelined behind this message, because the lane
    // takes the raw connection and whatever the packet filter still held would go with it
    // (ClientProxyUnknown::handleLaneHello).
    //
    // The token is written, never logged.
    ProtocolUtil::writef(m_stream, kMsgMTLaneHello, kLaneWireVersion, &m_screenName, &m_token);
    m_greeted = true;

    // The detach itself waits for StreamOutputFlushed: detachTls() refuses a socket whose output
    // buffer still holds anything, and the greeting is in it the instant after this write. Waiting
    // for the event rather than calling flush() is deliberate -- flush() parks the caller on a
    // condition variable until the multiplexer has drained the socket, and the caller here is the
    // thread that delivers the server's input to the screen.
  } catch (const BaseException &e) {
    abandonDial(e.what());
  } catch (const std::exception &e) {
    abandonDial(e.what());
  } catch (...) {
    abandonDial("error greeting the server");
  }
}

void ClipboardLaneDialer::handleOutputFlushed()
{
  // Only our own greeting can have been in that buffer -- nothing else is ever written on this
  // connection -- but the state is checked rather than assumed.
  if (m_stream == nullptr || !m_greeted) {
    return;
  }
  completeDial();
}

void ClipboardLaneDialer::completeDial()
{
  // Every check below is a REFUSAL, not an error. Closing and retrying costs one round trip;
  // improvising past any of them is what would hand a half-consumed or half-written TLS stream to
  // a new owner, which is the shape of the 2026-07-28 corruption.
  if (auto *filter = dynamic_cast<PacketStreamFilter *>(m_stream); filter != nullptr && filter->hasBufferedInput()) {
    abandonDial("the server sent something behind its greeting");
    return;
  }

  auto *secure = SecureSocket::fromStream(m_stream);
  if (secure == nullptr) {
    abandonDial("this is not a tls connection");
    return;
  }

  const auto detached = secure->detachTls();
  if (!detached.valid()) {
    // detachTls() already said which precondition failed, at DEBUG. Note that it leaves the socket
    // out of the multiplexer either way, so a refused detach is only ever followed by a close --
    // which is exactly what abandonDial() does.
    abandonDial("the connection could not be detached");
    return;
  }

  auto conn = deskflow::LaneConn::adopt(detached.socket, detached.ssl, detached.sslContext);
  if (!conn) {
    // Unreachable behind valid(), and handled rather than asserted. adopt() takes ownership
    // unconditionally and has already freed the connection, which matters here: cleanupAttempt()
    // could not close it -- the socket wrapper gave it away a moment ago.
    abandonDial("the detached connection could not be adopted");
    return;
  }

  // The socket wrapper and the stream own nothing now: close() and both destructors are no-ops for
  // a detached connection, so the ordinary teardown runs without touching the connection the lane
  // is about to take. Done BEFORE the handover so that no stale handler of ours can fire during it.
  cleanupAttempt();
  m_retryDelay = kRetryFirst;

  LOG_DEBUG("clipboard lane: dialled and detached; handing the connection over");
  try {
    m_onLaneReady(std::move(conn));
  } catch (const std::exception &e) {
    // Attaching a lane starts a thread, and thread creation is the one thing on this path that can
    // throw something which is not a BaseException -- it would escape the event dispatch entirely.
    // A clipboard lane must never be able to take the process with it.
    LOG_WARN("clipboard lane could not be started: %s", e.what());
  } catch (...) {
    LOG_WARN("clipboard lane could not be started");
  }
}

void ClipboardLaneDialer::abandonDial(const char *why)
{
  cleanupAttempt();
  // DEBUG, not WARN: a lane that does not come up costs a clipboard, and the send path already
  // warns about that (rate limited) at the moment it actually matters. A refusal here is normal on
  // a server that has not armed a session for us.
  LOG_DEBUG("clipboard lane: dial failed (%s); retrying in %.0f second(s)", why != nullptr ? why : "", m_retryDelay);
  scheduleRetry();
}

void ClipboardLaneDialer::cleanupAttempt()
{
  cancelDialTimeout();
  m_greeted = false;

  if (m_stream != nullptr) {
    m_events->removeHandlers(m_stream->getEventTarget());
    // adopt = false, so this does not touch the socket.
    delete m_stream;
    m_stream = nullptr;
  }
  if (m_socket != nullptr) {
    // Also removes the handler SecureSocket::connect() installed on its own target.
    m_events->removeHandlers(m_socket->getEventTarget());
    // Closes the connection -- unless it was detached, in which case the wrapper owns nothing and
    // this frees an empty shell.
    delete m_socket;
    m_socket = nullptr;
  }
}

void ClipboardLaneDialer::scheduleRetry()
{
  cancelRetry();
  if (m_token.empty()) {
    // No session to dial into. A new advert will start everything again.
    return;
  }

  m_retryTimer = m_events->newOneShotTimer(m_retryDelay, nullptr);
  // ONE member call and nothing after it. This handler destroys itself (via cancelRetry) part way
  // through, so the lambda's captured `this` must not be needed again once the call has started --
  // the same shape every self-cancelling handler in this code base uses.
  m_events->addHandler(EventTypes::Timer, m_retryTimer, [this](const auto &) { handleRetryTimer(); });

  // Explicit template argument, as everywhere else in this tree: <windows.h> arrives with a
  // function-like min() macro, and only a `min<T>(` spelling escapes it.
  m_retryDelay = std::min<double>(m_retryDelay * 2.0, kRetryMax);
}

void ClipboardLaneDialer::handleRetryTimer()
{
  cancelRetry();
  startDial();
}

void ClipboardLaneDialer::cancelRetry()
{
  if (m_retryTimer != nullptr) {
    m_events->removeHandler(EventTypes::Timer, m_retryTimer);
    m_events->deleteTimer(m_retryTimer);
    m_retryTimer = nullptr;
  }
}

void ClipboardLaneDialer::cancelDialTimeout()
{
  if (m_dialTimer != nullptr) {
    m_events->removeHandler(EventTypes::Timer, m_dialTimer);
    m_events->deleteTimer(m_dialTimer);
    m_dialTimer = nullptr;
  }
}
