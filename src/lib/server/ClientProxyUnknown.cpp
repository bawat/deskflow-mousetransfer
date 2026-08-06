/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/ClientProxyUnknown.h"

#include "base/IEventQueue.h"
#include "base/Log.h"
#include "deskflow/ClipboardLane.h"
#include "deskflow/DeskflowException.h"
#include "deskflow/PacketStreamFilter.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "io/IStream.h"
#include "net/LaneConn.h"
#include "net/SecureSocket.h"
#include "server/ClientProxy1_0.h"
#include "server/ClientProxy1_1.h"
#include "server/ClientProxy1_2.h"
#include "server/ClientProxy1_3.h"
#include "server/ClientProxy1_4.h"
#include "server/ClientProxy1_5.h"
#include "server/ClientProxy1_6.h"
#include "server/ClientProxy1_7.h"
#include "server/ClientProxy1_8.h"
#include "server/ClientProxy1_9.h"
#include "server/Server.h"

//
// ClientProxyUnknown
//

ClientProxyUnknown::ClientProxyUnknown(deskflow::IStream *stream, double timeout, Server *server, IEventQueue *events)
    : m_stream(stream),
      m_server(server),
      m_events(events)
{
  assert(m_server != nullptr);

  m_events->addHandler(EventTypes::Timer, this, [this](const auto &) { handleTimeout(); });
  m_timer = m_events->newOneShotTimer(timeout, this);
  addStreamHandlers();

  const auto protocol = m_server->protocolString();
  const auto helloMessage = protocol + kMsgHelloArgs;

  LOG_INFO("saying hello as %s, protocol v%d.%d", protocol.c_str(), kProtocolMajorVersion, kProtocolMinorVersion);
  // MouseTransfer diagnostic: name the STREAM the greeting is written to. Pair with the
  // "ACCEPTED chain" line to confirm it is the stream of the connection just accepted, and with
  // the socket-level WRITE line to confirm it reaches that connection's socket.
  if (mtDiagEnabled()) {
    LOG_NOTE("clipdiag: writing HELLO to stream=%p (proxy=%p)", static_cast<void *>(m_stream), static_cast<void *>(this));
  }
  ProtocolUtil::writef(m_stream, helloMessage.c_str(), kProtocolMajorVersion, kProtocolMinorVersion);
}

ClientProxyUnknown::~ClientProxyUnknown()
{
  removeHandlers();
  removeTimer();
  delete m_stream;
  delete m_proxy;
}

ClientProxy *ClientProxyUnknown::orphanClientProxy()
{
  if (m_ready) {
    removeHandlers();
    ClientProxy *proxy = m_proxy;
    m_proxy = nullptr;
    return proxy;
  } else {
    return nullptr;
  }
}

void ClientProxyUnknown::sendSuccess()
{
  m_ready = true;
  removeTimer();
  m_events->addEvent(Event(EventTypes::ClientProxyUnknownSuccess, this));
}

void ClientProxyUnknown::sendFailure()
{
  delete m_proxy;
  m_proxy = nullptr;
  m_ready = false;
  removeHandlers();
  removeTimer();
  m_events->addEvent(Event(EventTypes::ClientProxyUnknownFailure, this));
}

void ClientProxyUnknown::addStreamHandlers()
{
  assert(m_stream != nullptr);
  m_events->addHandler(EventTypes::StreamInputReady, m_stream->getEventTarget(), [this](const auto &) {
    handleData();
  });
  m_events->addHandler(EventTypes::StreamOutputError, m_stream->getEventTarget(), [this](const auto &) {
    handleWriteError();
  });
  m_events->addHandler(EventTypes::StreamInputShutdown, m_stream->getEventTarget(), [this](const auto &) {
    handleDisconnect();
  });
  m_events->addHandler(EventTypes::StreamInputFormatError, m_stream->getEventTarget(), [this](const auto &) {
    handleDisconnect();
  });
  m_events->addHandler(EventTypes::StreamOutputShutdown, m_stream->getEventTarget(), [this](const auto &) {
    handleWriteError();
  });
}

void ClientProxyUnknown::addProxyHandlers()
{
  assert(m_proxy != nullptr);

  m_events->addHandler(EventTypes::ClientProxyReady, m_proxy, [this](const auto &) { sendSuccess(); });
  m_events->addHandler(EventTypes::ClientProxyDisconnected, m_proxy, [this](const auto &) { handleDisconnect(); });
}

void ClientProxyUnknown::removeHandlers()
{
  using enum EventTypes;
  if (m_stream != nullptr) {
    m_events->removeHandler(StreamInputReady, m_stream->getEventTarget());
    m_events->removeHandler(StreamOutputError, m_stream->getEventTarget());
    m_events->removeHandler(StreamInputShutdown, m_stream->getEventTarget());
    m_events->removeHandler(StreamInputFormatError, m_stream->getEventTarget());
    m_events->removeHandler(StreamOutputShutdown, m_stream->getEventTarget());
  }
  if (m_proxy != nullptr) {
    m_events->removeHandler(ClientProxyReady, m_proxy);
    m_events->removeHandler(ClientProxyDisconnected, m_proxy);
  }
}

void ClientProxyUnknown::removeTimer()
{
  if (m_timer != nullptr) {
    m_events->deleteTimer(m_timer);
    m_events->removeHandler(EventTypes::Timer, this);
    m_timer = nullptr;
  }
}

void ClientProxyUnknown::initProxy(const std::string &name, int major, int minor)
{
  if (major == 1) {
    switch (minor) {
    case 0:
      m_proxy = new ClientProxy1_0(name, m_stream, m_events);
      break;

    case 1:
      m_proxy = new ClientProxy1_1(name, m_stream, m_events);
      break;

    case 2:
      m_proxy = new ClientProxy1_2(name, m_stream, m_events);
      break;

    case 3:
      m_proxy = new ClientProxy1_3(name, m_stream, m_events);
      break;

    case 4:
      m_proxy = new ClientProxy1_4(name, m_stream, m_server, m_events);
      break;

    case 5:
      m_proxy = new ClientProxy1_5(name, m_stream, m_server, m_events);
      break;

    case 6:
      m_proxy = new ClientProxy1_6(name, m_stream, m_server, m_events);
      break;

    case 7:
      m_proxy = new ClientProxy1_7(name, m_stream, m_server, m_events);
      break;

    case 8:
      m_proxy = new ClientProxy1_8(name, m_stream, m_server, m_events);
      break;

    case 9:
      m_proxy = new ClientProxy1_9(name, m_stream, m_server, m_events);
      break;

    default:
      // MouseTransfer fork, 2026-08-06: an unknown minor that is HIGHER than anything we know is
      // now clamped to our newest proxy instead of refused.
      //
      // Stock code fell through to `break` here, leaving m_proxy null, which throws
      // IncompatibleClientException -> kMsgEIncompatible -> the client gives up. That is the trap
      // that made bumping this fork's minor to 9 dangerous: it means an OLDER server refuses a
      // NEWER client outright rather than negotiating down, so the first machine of a rolling
      // deploy loses its KVM link entirely, not just its clipboard. Client::handleHello now clamps
      // what it announces, which fixes it for builds that have that clamp -- but a server also
      // being permissive is what stops the same trap re-arming at the NEXT bump, when the clamped
      // client is the old binary in the field. A higher minor is by definition a superset, so a
      // client that speaks it can also speak ours.
      //
      // A minor BELOW everything we know (there is no such case today -- 0 is handled) still
      // falls through to the throw, because that really is an incompatible peer.
      //
      // 1_8, NOT 1_9, and that is the whole subtlety. Work out who can actually reach this branch:
      // a client of THIS fork clamps what it announces to min(ours, the server's), so it can never
      // announce more than 9 to us -- it lands on `case 9`. Everything left is a build that does
      // NOT clamp, i.e. a stock/upstream one, and 1.9 in this fork is not 1.9 anywhere else: it
      // means "understands kMsgDLaneAdvert", a message this fork invented. ClientProxy1_9 exists
      // for exactly one purpose, to SEND that advert, and an unknown 4-char code does not degrade
      // a client -- ServerProxy::handleData cannot know its length, so it drains the entire stream
      // and the connection is finished. Handing an unknown-but-higher peer a 1_9 proxy would
      // therefore kill precisely the connection this branch was written to save. 1_8 is the newest
      // dialect we can speak to a stranger, and the only thing it costs is the clipboard lane,
      // which we could not have offered safely in any case.
      if (minor > kProtocolMinorVersion) {
        LOG_WARN(
            "client \"%s\" announced protocol %d.%d, newer than ours (%d.%d); negotiating down to 1.8",
            name.c_str(), major, minor, kProtocolMajorVersion, kProtocolMinorVersion
        );
        m_proxy = new ClientProxy1_8(name, m_stream, m_server, m_events);
      }
      break;
    }
  }

  // hangup (with error) if version isn't supported
  if (m_proxy == nullptr) {
    throw IncompatibleClientException(major, minor);
  }
}

void ClientProxyUnknown::handleLaneHello()
{
  // Everything here is best-effort and self-contained. The connection this runs on is a SECOND
  // connection from a peer whose main session is already up; nothing we do or fail to do may reach
  // that session.
  int16_t laneVersion = 0;
  std::string announced;
  std::string token;
  if (!ProtocolUtil::readf(m_stream, kMsgMTLaneHello + 4, &laneVersion, &announced, &token)) {
    LOG_DEBUG("clipboard lane refused: malformed lane greeting");
    sendFailure();
    return;
  }

  // The name off the wire is what the CLIENT calls itself. Every session was opened under the
  // CONFIGURATION's spelling (Server::getName -> Config::getCanonicalName: a caseless lookup that
  // also resolves aliases), and so are m_clients and the teardown in removeClient. Matching the
  // announced string directly would therefore refuse the lane of any screen the config spells
  // differently or knows by an alias -- silently, at DEBUG, with clipboard sync dead in both
  // directions and nothing else wrong. Canonicalise once, here, and use only that below.
  const std::string name = m_server->canonicalName(announced);

  auto &lane = m_server->clipboardLane();

  // Validate BEFORE detaching anything: a refusal must leave the connection in the ordinary state
  // so the ordinary teardown can close it.
  std::string fingerprint;
  auto *secure = SecureSocket::fromStream(m_stream);
  if (secure != nullptr) {
    fingerprint = secure->peerFingerprint();
  }

  if (std::string reason; !lane.validate(name, token, fingerprint, laneVersion, reason)) {
    // At DEBUG and naming the reason: a rejected lane is not an alarm (it is what an attacker or a
    // stale client looks like), but "which check failed" is the only useful thing to know when a
    // lane that SHOULD work does not.
    LOG_DEBUG("clipboard lane refused for \"%s\": %s", name.c_str(), reason.c_str());
    sendFailure();
    return;
  }

  // Only a TLS connection can become a lane: the fingerprint binding is half the authorisation, and
  // the plaintext path has no SSL object to hand over in the first place.
  if (secure == nullptr) {
    LOG_DEBUG("clipboard lane refused for \"%s\": not a tls connection", name.c_str());
    sendFailure();
    return;
  }

  // Anything still sitting in the packet filter would be lost by the handoff -- the lane takes the
  // raw connection, not the filter. The client contract is "write the lane greeting and nothing
  // else until the ack", so this should never fire; if it does, refusing is the honest answer.
  if (auto *filter = dynamic_cast<PacketStreamFilter *>(m_stream); filter != nullptr && filter->hasBufferedInput()) {
    LOG_DEBUG("clipboard lane refused for \"%s\": the peer pipelined data behind its greeting", name.c_str());
    sendFailure();
    return;
  }

  auto detached = secure->detachTls();
  if (!detached.valid()) {
    // detachTls() refuses anything that is not quiescent, and says why at DEBUG. Falling back to a
    // normal close costs the peer one retry.
    LOG_DEBUG("clipboard lane refused for \"%s\": the connection could not be detached", name.c_str());
    sendFailure();
    return;
  }

  auto conn = deskflow::LaneConn::adopt(detached.socket, detached.ssl, detached.sslContext, std::move(detached.pending));
  if (!conn) {
    // Cannot happen with a valid DetachedTls, and handled rather than asserted. adopt() takes
    // ownership unconditionally, so the connection is already freed by the time we get here --
    // nothing below can reach it any more, the socket wrapper having given it away.
    LOG_DEBUG("clipboard lane refused for \"%s\": could not adopt the detached connection", name.c_str());
    sendFailure();
    return;
  }

  try {
    lane.attach(name, std::move(conn));
  } catch (const std::exception &e) {
    // attach() starts a thread, and thread creation is the one thing here that can throw something
    // that is not a BaseException -- which would escape the event dispatch entirely rather than be
    // caught by handleData. The connection is already gone with the unique_ptr by this point; all
    // that is left to do is not take the process with it.
    LOG_WARN("clipboard lane for \"%s\" could not be started: %s", name.c_str(), e.what());
  }

  // The socket wrapper, the stream and this object are all inert now: the wrapper owns nothing, and
  // the ordinary failure route deletes the three of them in the right order without touching the
  // connection the lane just took. Reusing that route rather than inventing a second teardown is
  // the point -- there is exactly one way these objects get cleaned up.
  sendFailure();
}

void ClientProxyUnknown::handleData()
{
  LOG_DEBUG1("parsing hello reply");

  std::string name("<unknown>");

  try {
    // limit the maximum length of the hello
    if (uint32_t n = m_stream->getSize(); n > kMaxHelloLength) {
      LOG_DEBUG1("hello reply too long");
      throw BadClientException();
    }

    // MouseTransfer: a clipboard lane greets us here instead of replying to the hello, so the first
    // four bytes decide which conversation this is. They are read separately rather than by readf
    // because readf CONSUMES what it parses and there is no way to put it back -- and because the
    // hello reply's protocol name is a fixed 7-byte field (kMsgHelloBack is "%7s..."), those four
    // bytes plus the next three reconstruct it exactly, which is what the non-lane path does below.
    uint8_t head[4];
    if (m_stream->read(head, sizeof(head)) != sizeof(head)) {
      throw BadClientException();
    }
    if (memcmp(head, kMsgMTLaneHello, sizeof(head)) == 0) {
      handleLaneHello();
      return;
    }

    // parse the reply to hello
    int16_t major;
    int16_t minor;
    uint8_t restOfProtocolName[3];
    if (m_stream->read(restOfProtocolName, sizeof(restOfProtocolName)) != sizeof(restOfProtocolName)) {
      throw BadClientException();
    }
    // The protocol name itself is read but not checked, exactly as before -- version compatibility
    // is decided by the numbers, and both accepted names ("Synergy", "Barrier") are 7 chars.
    if (!ProtocolUtil::readf(m_stream, kMsgHelloBackArgs, &major, &minor, &name)) {
      throw BadClientException();
    }

    // disallow invalid version numbers
    if (major <= 0 || minor < 0) {
      throw IncompatibleClientException(major, minor);
    }

    // remove stream event handlers.  the proxy we're about to create
    // may install its own handlers and we don't want to accidentally
    // remove those later.
    removeHandlers();

    // create client proxy for highest version supported by the client
    initProxy(name, major, minor);

    // the proxy is created and now proxy now owns the stream
    LOG_DEBUG1("created proxy for client \"%s\" version %d.%d", name.c_str(), major, minor);
    m_stream = nullptr;

    // wait until the proxy signals that it's ready or has disconnected
    addProxyHandlers();
    return;
  } catch (IncompatibleClientException &e) {
    // client is incompatible
    LOG_WARN("client \"%s\" has incompatible version %d.%d)", name.c_str(), e.getMajor(), e.getMinor());
    ProtocolUtil::writef(m_stream, kMsgEIncompatible, kProtocolMajorVersion, kProtocolMinorVersion);
  } catch (BadClientException &) {
    // client not behaving
    LOG_WARN("protocol error from client \"%s\"", name.c_str());
    ProtocolUtil::writef(m_stream, kMsgEBad);
  } catch (BaseException &e) {
    // misc error
    LOG_WARN("error communicating with client \"%s\": %s", name.c_str(), e.what());
  }
  sendFailure();
}

void ClientProxyUnknown::handleWriteError()
{
  LOG_NOTE("error communicating with new client");
  sendFailure();
}

void ClientProxyUnknown::handleTimeout()
{
  LOG_NOTE("new client is unresponsive");
  sendFailure();
}

void ClientProxyUnknown::handleDisconnect()
{
  LOG_NOTE("new client disconnected");
  sendFailure();
}
