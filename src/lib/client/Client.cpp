/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016, 2026 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "client/Client.h"

#include "arch/Arch.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "base/NetworkProtocol.h"
#include "client/ClipboardLaneDialer.h"
#include "client/ServerProxy.h"
#include "common/Settings.h"
#include "deskflow/Clipboard.h"
#include "deskflow/IPlatformScreen.h"
#include "deskflow/PacketStreamFilter.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "deskflow/Screen.h"
#include "net/IDataSocket.h"
#include "net/ISocketFactory.h"
#include "net/LaneConn.h"
#include "net/SecureSocket.h"
#include "net/TCPSocket.h"

#include <cstdlib>
#include <cstring>
#include <utility>

namespace {

//! The name the client's lane manager keys its ONE session by
/*!
The manager is keyed by peer screen name because a SERVER has many peers. A client has exactly one,
and the protocol never tells it what that peer is called -- kMsgHello carries a product name and a
version, nothing else -- so there is no real name available to use. A fixed key is therefore both
correct and unambiguous: every lane log line on a client refers to this one connection.
*/
constexpr const char *kLaneServerPeer = "server";

} // namespace

//
// Client
//

Client::Client(
    IEventQueue *events, const std::string &name, const NetworkAddress &address, ISocketFactory *socketFactory,
    deskflow::Screen *screen
)
    : m_name(name),
      m_serverAddress(address),
      m_socketFactory(socketFactory),
      m_screen(screen),
      m_events(events),
      m_useSecureNetwork(Settings::value(Settings::Security::TlsEnabled).toBool())
{
  assert(m_socketFactory != nullptr);
  assert(m_screen != nullptr);

  // MouseTransfer: the clipboard lane. Built here rather than on demand so that clipboardLane() is
  // always valid, and destroyed with this object -- its destructor stops and joins the lane worker.
  // The cap starts at whatever the default clipboard limit is and is kept in step by setOptions().
  m_clipboardLane = std::make_unique<deskflow::ClipboardLaneManager>(m_events, maxClipboardBytes());
  m_clipboardLane->setReceiveHandler([this](const deskflow::LaneClipboardInfo &info) {
    handleLaneClipboard(info);
  });

  // register suspend/resume event handlers
  m_events->addHandler(EventTypes::ScreenSuspend, getEventTarget(), [this](const auto &) { handleSuspend(); });
  m_events->addHandler(EventTypes::ScreenResume, getEventTarget(), [this](const auto &) { handleResume(); });
}

Client::~Client()
{
  m_events->removeHandler(EventTypes::ScreenSuspend, getEventTarget());
  m_events->removeHandler(EventTypes::ScreenResume, getEventTarget());

  cleanupTimer();
  cleanupScreen();
  cleanupConnecting();
  cleanupConnection();
  // The dialer holds the socket factory, so it must be gone before the factory is. cleanupConnection
  // has already destroyed it; this is the guarantee rather than the mechanism.
  m_laneDialer.reset();
  delete m_socketFactory;
}

void Client::setServerAddress(const NetworkAddress &address)
{
  m_serverAddress = address;
}

void Client::connect(size_t addressIndex)
{
  if (m_stream != nullptr) {
    return;
  }
  if (m_suspended) {
    m_connectOnResume = true;
    return;
  }

  auto securityLevel = m_useSecureNetwork ? SecurityLevel::PeerAuth : SecurityLevel::PlainText;

  try {
    // resolve the server hostname.  do this every time we connect
    // in case we couldn't resolve the address earlier or the address
    // has changed (which can happen frequently if this is a laptop
    // being shuttled between various networks).  patch by Brent
    // Priddy.
    m_resolvedAddressesCount = m_serverAddress.resolve(addressIndex);

    // m_serverAddress will be null if the hostname address is not reolved
    if (m_serverAddress.getAddress() != nullptr) {
      // to help users troubleshoot, show server host name (issue: 60)
      LOG_IPC(
          "connecting to '%s': %s:%i", m_serverAddress.getHostname().c_str(),
          ARCH->addrToString(m_serverAddress.getAddress()).c_str(), m_serverAddress.getPort()
      );
    }

    // create the socket
    IDataSocket *socket = m_socketFactory->create(ARCH->getAddrFamily(m_serverAddress.getAddress()), securityLevel);
    bindNetworkInterface(socket);

    // filter socket messages, including a packetizing filter
    m_stream = new PacketStreamFilter(m_events, socket, true);

    // connect
    LOG_DEBUG1("connecting to server");
    setupConnecting();
    setupTimer();
    socket->connect(m_serverAddress);
  } catch (BaseException &e) {
    cleanupTimer();
    cleanupConnecting();
    cleanupStream();
    LOG_DEBUG1("connection failed");
    sendConnectionFailedEvent(e.what());
    return;
  }
}

void Client::disconnect(const char *msg)
{
  cleanup();

  if (msg) {
    sendConnectionFailedEvent(msg);
  } else {
    sendEvent(EventTypes::ClientDisconnected);
  }
}

void Client::refuseConnection(const char *msg)
{
  cleanup();

  if (msg) {
    auto info = new FailInfo(msg);
    info->m_retry = true;
    Event event(EventTypes::ClientConnectionRefused, getEventTarget(), info, Event::EventFlags::DontFreeData);
    m_events->addEvent(std::move(event));
  }
}

void Client::handshakeComplete()
{
  m_ready = true;
  m_screen->enable();
  sendEvent(EventTypes::ClientConnected);
}

bool Client::isConnected() const
{
  return (m_server != nullptr);
}

bool Client::isConnecting() const
{
  return (m_timer != nullptr);
}

NetworkAddress Client::getServerAddress() const
{
  return m_serverAddress;
}

void *Client::getEventTarget() const
{
  return m_screen->getEventTarget();
}

bool Client::getClipboard(ClipboardID id, IClipboard *clipboard) const
{
  return m_screen->getClipboard(id, clipboard);
}

void Client::getShape(int32_t &x, int32_t &y, int32_t &w, int32_t &h) const
{
  m_screen->getShape(x, y, w, h);
}

void Client::getCursorPos(int32_t &x, int32_t &y) const
{
  m_screen->getCursorPos(x, y);
}

void Client::enter(int32_t xAbs, int32_t yAbs, uint32_t, KeyModifierMask mask, bool)
{
  m_active = true;
  m_screen->mouseMove(xAbs, yAbs);
  m_screen->enter(mask);

  // MouseTransfer: a crossing is the only moment a clipboard moves in either direction, so it is
  // exactly when a lane needs to exist -- and it is an event that already happens, which is why the
  // check lives here rather than on a timer of its own. It matters because a lane that dies WITHOUT
  // the main connection dying (an idle lane sends nothing, so a stateful firewall can reap it) would
  // otherwise go unnoticed until this machine next copied something: the server cannot re-dial, only
  // we can. Costs a lookup on a one-entry map.
  ensureClipboardLane();
}

bool Client::leave()
{
  m_active = false;

  m_screen->leave();

  if (m_enableClipboard) {
    // send clipboards that we own and that have changed
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
      if (m_ownClipboard[id]) {
        sendClipboard(id);
      }
    }
  }

  return true;
}

void Client::setClipboard(ClipboardID id, const IClipboard *clipboard)
{
  m_screen->setClipboard(id, clipboard);
  m_ownClipboard[id] = false;
  m_sentClipboard[id] = false;
}

void Client::grabClipboard(ClipboardID id)
{
  m_screen->grabClipboard(id);
  m_ownClipboard[id] = false;
  m_sentClipboard[id] = false;
}

void Client::setClipboardDirty(ClipboardID, bool)
{
  assert(0 && "shouldn't be called");
}

void Client::keyDown(KeyID id, KeyModifierMask mask, KeyButton button, const std::string &lang)
{
  m_screen->keyDown(id, mask, button, lang);
}

void Client::keyRepeat(KeyID id, KeyModifierMask mask, int32_t count, KeyButton button, const std::string &lang)
{
  m_screen->keyRepeat(id, mask, count, button, lang);
}

void Client::keyUp(KeyID id, KeyModifierMask mask, KeyButton button)
{
  m_screen->keyUp(id, mask, button);
}

void Client::mouseDown(ButtonID id)
{
  m_screen->mouseDown(id);
}

void Client::mouseUp(ButtonID id)
{
  m_screen->mouseUp(id);
}

void Client::mouseMove(int32_t x, int32_t y)
{
  m_screen->mouseMove(x, y);
}

void Client::mouseRelativeMove(int32_t dx, int32_t dy)
{
  m_screen->mouseRelativeMove(dx, dy);
}

void Client::mouseWheel(int32_t xDelta, int32_t yDelta)
{
  m_screen->mouseWheel(xDelta, yDelta);
}

void Client::screensaver(bool activate)
{
  m_screen->screensaver(activate);
}

void Client::resetOptions()
{
  m_screen->resetOptions();
}

void Client::setOptions(const OptionsList &options)
{
  for (auto index = options.begin(); index != options.end(); ++index) {
    const OptionID id = *index;
    if (id == kOptionClipboardSharing) {
      index++;
      if (index != options.end()) {
        if (!*index) {
          LOG_NOTE("clipboard sharing disabled by server");
        }
        m_enableClipboard = *index;
      }
    } else if (id == kOptionClipboardSharingSize) {
      index++;
      if (index != options.end()) {
        m_maximumClipboardSize = *index;
      }
    }
  }

  if (m_enableClipboard && !m_maximumClipboardSize) {
    m_enableClipboard = false;
    LOG_NOTE("clipboard sharing is disabled because the server set the maximum clipboard size to 0");
  }

  // MouseTransfer: keep the lane's ceiling in step with the limit the server just set, exactly as
  // the server keeps its own manager in step. The lane refuses to send or accept anything larger.
  m_clipboardLane->setMaxPayloadBytes(maxClipboardBytes());

  m_screen->setOptions(options);
}

size_t Client::maxClipboardBytes() const
{
  // m_maximumClipboardSize is KILOBYTES (kOptionClipboardSharingSize) and defaults to INT_MAX, i.e.
  // "no limit". Multiplying that by 1024 is fine on a 64-bit size_t and overflows on a 32-bit one,
  // which would silently turn "no limit" into a very small one -- so saturate instead of wrapping.
  // Same reasoning, same code, as Server::maxClipboardBytes().
  constexpr size_t kBytesPerKb = 1024;
  if (m_maximumClipboardSize > SIZE_MAX / kBytesPerKb) {
    return SIZE_MAX;
  }
  return m_maximumClipboardSize * kBytesPerKb;
}

std::string Client::getName() const
{
  return m_name;
}

void Client::sendClipboard(ClipboardID id)
{
  // note -- m_mutex must be locked on entry
  assert(m_screen != nullptr);
  assert(m_server != nullptr);

  // get clipboard data.  set the clipboard time to the last
  // clipboard time before getting the data from the screen
  // as the screen may detect an unchanged clipboard and
  // avoid copying the data.
  Clipboard clipboard;
  if (clipboard.open(m_timeClipboard[id])) {
    clipboard.close();
  }
  m_screen->getClipboard(id, &clipboard);

  // check time
  if (m_timeClipboard[id] == 0 || clipboard.getTime() != m_timeClipboard[id]) {
    // marshall the data
    //
    // MouseTransfer, stage 5: ONCE, into a shared immutable buffer that is then the dedup record,
    // the size check AND the payload handed to the lane. This function used to marshall for the
    // size/dedup checks, keep a private copy of the result, and then hand the CLIPBOARD to
    // ServerProxy::onClipboardChanged, which marshalled the very same object all over again. At
    // 64 MiB that was a second 134 MB of allocate-and-copy on the leave path -- the moment the user
    // is waiting for the cursor to appear on the next screen.
    auto marshalled = std::make_shared<const std::string>(clipboard.marshall());
    const std::string &data = *marshalled;
    if (data.size() >= m_maximumClipboardSize * 1024) {
      LOG(
          (CLOG_NOTE "skipping clipboard transfer because the clipboard"
                     " contents exceeds the %i MB size limit set by the server",
           m_maximumClipboardSize / 1024)
      );
      return;
    }

    // save new time
    m_timeClipboard[id] = clipboard.getTime();
    // save and send data if different or not yet sent
    if (!m_sentClipboard[id] || !m_dataClipboard[id] || data != *m_dataClipboard[id]) {
      m_sentClipboard[id] = true;
      m_dataClipboard[id] = marshalled;
      m_server->onClipboardChanged(id, std::move(marshalled));
    }
  }
}

void Client::onClipboardLaneAdvert(int16_t laneVersion, std::string token)
{
  // The dial needs the address the main connection actually succeeded on, already resolved: a lane
  // must never perform name resolution, because that blocks and this is the thread that delivers
  // input. m_serverAddress is exactly that by the time an advert can arrive.
  if (!m_laneDialer) {
    m_laneDialer = std::make_unique<ClipboardLaneDialer>(
        m_events, m_socketFactory, m_serverAddress, m_name,
        m_useSecureNetwork ? SecurityLevel::PeerAuth : SecurityLevel::PlainText,
        [this](IDataSocket *socket) { bindNetworkInterface(socket); },
        [this](std::unique_ptr<deskflow::LaneConn> conn) {
          m_clipboardLane->attach(kLaneServerPeer, std::move(conn));
        }
    );
  }

  // A fresh advert supersedes whatever came before: openLocalSession() replaces the session and
  // tears down any lane hanging off the old one, so a stale connection can never outlive the token
  // that authorised it. The token itself goes no further than the dialer, and is never logged.
  m_clipboardLane->openLocalSession(kLaneServerPeer);
  m_laneDialer->advertised(laneVersion, std::move(token));
}

deskflow::ClipboardLaneManager::SendResult
Client::sendClipboardOverLane(ClipboardID id, uint32_t sequenceNumber, std::shared_ptr<const std::string> payload)
{
  const auto result = m_clipboardLane->send(kLaneServerPeer, id, sequenceNumber, std::move(payload));

  // The lazy half of the retry policy (MT-CLIPBOARD-LANE-DESIGN.md §2): a clipboard we could not
  // deliver is the moment it becomes worth trying again. Cheap and idempotent -- it does nothing at
  // all unless the dialer is idle with a token, so it cannot interrupt an attempt in flight or pull
  // a backoff forward.
  //
  // Held is the case that matters (a session exists, so a token exists, but the lane is down and
  // the payload is being kept for it). NoLane is kept alongside it because it costs nothing and is
  // a no-op by construction: no session means no advert, which means the dialer has no token.
  using SendResult = deskflow::ClipboardLaneManager::SendResult;
  if ((result == SendResult::Held || result == SendResult::NoLane) && m_laneDialer) {
    m_laneDialer->nudge();
  }
  return result;
}

void Client::handleLaneClipboard(const deskflow::LaneClipboardInfo &info)
{
  if (m_server == nullptr) {
    // The main session ended between the worker posting this and us being called. Dropping it is
    // the whole recovery -- there is nowhere to apply a clipboard to.
    LOG_DEBUG("clipboard lane: dropping clipboard %d, the server connection is gone", static_cast<int>(info.m_id));
    return;
  }

  // Routed through the proxy rather than applied here, so that a lane clipboard lands in exactly
  // the same state, through exactly the same code, as one that arrived in-stream.
  m_server->applyLaneClipboard(info.m_id, info.m_sequenceNumber, info.m_data);
}

void Client::ensureClipboardLane()
{
  // hasLane() is the authority on "is there a live lane", and it also reaps a worker that has
  // already given up -- which is how a lane that died quietly gets noticed at all. nudge() is a
  // no-op unless the dialer is idle with a token, so this can neither interrupt a dial in flight
  // nor pull a backoff forward.
  if (m_laneDialer && !m_clipboardLane->hasLane(kLaneServerPeer)) {
    m_laneDialer->nudge();
  }
}

void Client::cleanupLane()
{
  // The dialer first: it is the only thing that could still be opening a connection, and there is
  // no session left for one to attach to. Destroying it abandons any attempt in flight.
  m_laneDialer.reset();
  // Stops and joins the lane worker if there is one. A no-op when there never was a lane.
  m_clipboardLane->closeSession(kLaneServerPeer);
}

void Client::sendEvent(EventTypes type)
{
  m_events->addEvent(Event(type, getEventTarget()));
}

void Client::sendConnectionFailedEvent(const char *msg)
{
  auto *info = new FailInfo(msg);
  info->m_retry = true;
  Event event(EventTypes::ClientConnectionFailed, getEventTarget(), info, Event::EventFlags::DontFreeData);
  m_events->addEvent(std::move(event));
}

void Client::setupConnecting()
{
  assert(m_stream != nullptr);

  if (Settings::value(Settings::Security::TlsEnabled).toBool()) {
    m_events->addHandler(EventTypes::DataSocketSecureConnected, m_stream->getEventTarget(), [this](const auto &) {
      handleConnected();
    });
  } else {
    m_events->addHandler(EventTypes::DataSocketConnected, m_stream->getEventTarget(), [this](const auto &) {
      handleConnected();
    });
  }
  m_events->addHandler(EventTypes::DataSocketConnectionFailed, m_stream->getEventTarget(), [this](const auto &e) {
    handleConnectionFailed(e);
  });
}

void Client::setupConnection()
{
  assert(m_stream != nullptr);

  m_events->addHandler(EventTypes::SocketDisconnected, m_stream->getEventTarget(), [this](const auto &) {
    handleDisconnected();
  });
  m_events->addHandler(EventTypes::StreamInputReady, m_stream->getEventTarget(), [this](const auto &) {
    handleHello();
  });
  m_events->addHandler(EventTypes::StreamOutputError, m_stream->getEventTarget(), [this](const auto &) {
    handleOutputError();
  });
  m_events->addHandler(EventTypes::StreamInputShutdown, m_stream->getEventTarget(), [this](const auto &) {
    handleDisconnected();
  });
  m_events->addHandler(EventTypes::StreamOutputShutdown, m_stream->getEventTarget(), [this](const auto &) {
    handleDisconnected();
  });
}

void Client::setupScreen()
{
  assert(m_server == nullptr);

  m_ready = false;
  m_server = new ServerProxy(this, m_stream, m_events);
  m_events->addHandler(EventTypes::ScreenShapeChanged, getEventTarget(), [this](const auto &) {
    handleShapeChanged();
  });
  m_events->addHandler(EventTypes::ClipboardGrabbed, getEventTarget(), [this](const auto &e) {
    handleClipboardGrabbed(e);
  });
}

void Client::setupTimer()
{
  assert(m_timer == nullptr);
  m_timer = m_events->newOneShotTimer(2.0, nullptr);
  m_events->addHandler(EventTypes::Timer, m_timer, [this](const auto &) { handleConnectTimeout(); });
}

void Client::cleanup()
{
  m_connectOnResume = false;
  cleanupTimer();
  cleanupScreen();
  cleanupConnecting();
  cleanupConnection();
}

void Client::cleanupConnecting()
{
  if (m_stream != nullptr) {
    m_events->removeHandler(EventTypes::DataSocketConnected, m_stream->getEventTarget());
    m_events->removeHandler(EventTypes::DataSocketConnectionFailed, m_stream->getEventTarget());
  }
}

void Client::cleanupConnection()
{
  // MouseTransfer: the lane belongs to the main session and dies with it. Deliberately OUTSIDE the
  // stream check below -- every route that ends a connection passes through here, and some of them
  // arrive with the stream already gone. A reconnect brings its own advert and its own token, and
  // that is what opens the next lane.
  cleanupLane();

  if (m_stream != nullptr) {
    using enum EventTypes;
    m_events->removeHandler(StreamInputReady, m_stream->getEventTarget());
    m_events->removeHandler(StreamOutputError, m_stream->getEventTarget());
    m_events->removeHandler(StreamInputShutdown, m_stream->getEventTarget());
    m_events->removeHandler(StreamOutputShutdown, m_stream->getEventTarget());
    m_events->removeHandler(SocketDisconnected, m_stream->getEventTarget());
    cleanupStream();
  }
}

void Client::cleanupScreen()
{
  if (m_server != nullptr) {
    if (m_ready) {
      m_screen->disable();
      m_ready = false;
    }
    m_events->removeHandler(EventTypes::ScreenShapeChanged, getEventTarget());
    m_events->removeHandler(EventTypes::ClipboardGrabbed, getEventTarget());
    delete m_server;
    m_server = nullptr;
  }
}

void Client::cleanupTimer()
{
  if (m_timer != nullptr) {
    m_events->removeHandler(EventTypes::Timer, m_timer);
    m_events->deleteTimer(m_timer);
    m_timer = nullptr;
  }
}

void Client::cleanupStream()
{
  delete m_stream;
  m_stream = nullptr;
}

void Client::handleConnected()
{
  LOG_DEBUG1("connected, waiting for hello");
  cleanupConnecting();
  setupConnection();

  // reset clipboard state
  for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
    m_ownClipboard[id] = false;
    m_sentClipboard[id] = false;
    m_timeClipboard[id] = 0;
  }
}

void Client::handleConnectionFailed(const Event &event)
{
  auto *info = static_cast<IDataSocket::ConnectionFailedInfo *>(event.getData());

  cleanupTimer();
  cleanupConnecting();
  cleanupStream();
  LOG_DEBUG1("connection failed");
  sendConnectionFailedEvent(info->m_what.c_str());
  delete info;
}

void Client::handleConnectTimeout()
{
  cleanupTimer();
  cleanupConnecting();
  cleanupConnection();
  cleanupStream();
  LOG_DEBUG1("connection timed out");
  sendConnectionFailedEvent("Timed out");
}

void Client::handleOutputError()
{
  cleanupTimer();
  cleanupScreen();
  cleanupConnection();
  LOG_WARN("error sending to server");
  sendEvent(EventTypes::ClientDisconnected);
}

void Client::handleDisconnected()
{
  cleanupTimer();
  cleanupScreen();
  cleanupConnection();
  LOG_DEBUG1("disconnected");
  sendEvent(EventTypes::ClientDisconnected);
}

void Client::handleShapeChanged()
{
  LOG_DEBUG("resolution changed");
  m_server->onInfoChanged();
}

void Client::handleClipboardGrabbed(const Event &event)
{
  if (!m_enableClipboard || (m_maximumClipboardSize == 0)) {
    return;
  }

  const auto *info = static_cast<const IScreen::ClipboardInfo *>(event.getData());

  // grab ownership
  m_server->onGrabClipboard(info->m_id);

  // we now own the clipboard and it has not been sent to the server
  m_ownClipboard[info->m_id] = true;
  m_sentClipboard[info->m_id] = false;
  m_timeClipboard[info->m_id] = 0;

  // if we're not the active screen then send the clipboard now,
  // otherwise we'll wait until we leave.
  if (!m_active) {
    sendClipboard(info->m_id);
  }
}

void Client::handleHello()
{
  int16_t serverMajor;
  int16_t serverMinor;

  // as luck would have it, both "Synergy" and "Barrier" are 7 chars,
  // so we eat 7 chars and then test for either protocol name.
  // we cannot re-use `readf` to check for various hello messages,
  // as `readf` eats bytes (advances the stream position reference).
  std::string protocolName;
  ProtocolUtil::readf(m_stream, kMsgHello, &protocolName, &serverMajor, &serverMinor);

  if (const auto proto = networkProtocolFromString(QString::fromStdString(protocolName));
      proto == NetworkProtocol::Unknown) {
    LOG_WARN("hello back received with protocol: '%s'", protocolName.c_str());
    // MouseTransfer diagnostic (2026-07-28 clipboard-chunk storm). The protocol name is a FIXED
    // 7-byte read (kMsgHello = "%7s%2i%2i"), so `protocolName` is literally the head of whatever
    // the server put on this stream. Printing it through %s stops at the first NUL, which is why
    // a clipboard chunk surfaces as just 'DCLP' -- the clipboard-id byte following the code is
    // zero. That single token cannot distinguish the two situations it might mean, and they call
    // for opposite actions:
    //
    //   * an incompatible PEER  -> the remote build is wrong; upgrading it is the fix.
    //   * a MIS-FRAMED stream   -> a message BODY arrived where the greeting belongs. Nothing is
    //                              wrong with either build, the server is mid-write of something
    //                              else, and no amount of restarting THIS core will help.
    //
    // The backlog size separates them: a greeting is 11 bytes, so a large queued size means the
    // server is streaming a message body at us. Dump both.
    {
      std::string hex;
      hex.reserve(protocolName.size() * 3);
      for (const unsigned char c : protocolName) {
        static const char *const digits = "0123456789abcdef";
        if (!hex.empty()) {
          hex += ' ';
        }
        hex += digits[(c >> 4) & 0x0f];
        hex += digits[c & 0x0f];
      }
      LOG_WARN(
          "  greeting bytes: %s | %u more byte(s) already queued behind them on this stream", hex.c_str(),
          m_stream->getSize()
      );
      // kMsgDClipboard is "DCLP%1i%4i%1i%s"; only the 4-char code is a literal, so compare that
      // much. Byte 4 is the clipboard id -- the rest of the header (sequence, chunk mark) lies
      // past the 7 bytes the hello read consumed, so it is deliberately not decoded here.
      if (protocolName.size() >= 5 && protocolName.compare(0, 4, kMsgDClipboard, 0, 4) == 0) {
        LOG_WARN(
            "  ==> this is CLIPBOARD DATA (kMsgDClipboard) for clipboard %d, not a greeting: the "
            "server is writing a clipboard chunk stream onto a fresh connection. Restart the SERVER's core.",
            static_cast<int>(static_cast<unsigned char>(protocolName[4]))
        );
      }
    }
    sendConnectionFailedEvent("got invalid hello message from server");
    cleanupTimer();
    cleanupConnection();
    return;
  }

  // MouseTransfer fork, 2026-08-06: announce min(ours, the server's) rather than ours flat.
  //
  // Stock code announced kProtocolMinorVersion unconditionally and threw the server's numbers away.
  // That is safe only while every build in a mesh shares one minor, because the SERVER's tolerance
  // runs one way only: ClientProxyUnknown::initProxy switches on the announced minor, and its
  // default case leaves the proxy null, which throws IncompatibleClientException -- so a client
  // announcing a minor the server has never heard of is REFUSED, not degraded. The moment this fork
  // bumped 1.8 -> 1.9 for the clipboard lane, the first machine of a rolling deploy would otherwise
  // have been unable to connect to any not-yet-updated server, and since the server role moves
  // around this fleet that is a random total KVM outage.
  //
  // Clamping costs nothing in the matched case (min(9,9) == 9) and cannot lose a capability: the
  // server decides what it sends from the proxy class it built for the version we announced, so
  // announcing less only makes it speak an older dialect that this client still fully understands.
  const int16_t announcedMinor = (serverMinor < kProtocolMinorVersion) ? serverMinor : kProtocolMinorVersion;

  LOG_DEBUG(
      "saying hello back with version %s %d.%d (server offered %d.%d)", protocolName.c_str(), kProtocolMajorVersion,
      announcedMinor, serverMajor, serverMinor
  );

  // dynamically build write format for hello back since `ProtocolUtil::writef`
  // doesn't support formatting fixed length strings yet.
  std::string helloBackMessage = protocolName + kMsgHelloBackArgs;
  ProtocolUtil::writef(m_stream, helloBackMessage.c_str(), kProtocolMajorVersion, announcedMinor, &m_name);

  // now connected but waiting to complete handshake
  setupScreen();
  cleanupTimer();

  // make sure we process any remaining messages later.  we won't
  // receive another event for already pending messages so we fake
  // one.
  if (m_stream->isReady()) {
    m_events->addEvent(Event(EventTypes::StreamInputReady, m_stream->getEventTarget()));
  }
}

void Client::handleSuspend()
{
  if (!m_suspended) {
    LOG_INFO("suspend");
    m_suspended = true;
    bool wasConnected = isConnected();
    disconnect(nullptr);
    m_connectOnResume = wasConnected;
  }
}

void Client::handleResume()
{
  if (m_suspended) {
    LOG_INFO("resume");
    m_suspended = false;
    if (m_connectOnResume) {
      m_connectOnResume = false;
      connect();
    }
  }
}

void Client::bindNetworkInterface(IDataSocket *socket) const
{
  try {
    if (const auto address = Settings::value(Settings::Core::Interface).toString(); !address.isEmpty()) {
      LOG_DEBUG1("bind to network interface: %s", qPrintable(address));

      NetworkAddress bindAddress(address.toStdString());
      bindAddress.resolve();

      socket->bind(bindAddress);
    }
  } catch (BaseException &e) {
    LOG_WARN("%s", e.what());
    LOG_WARN("operating system will select network interface automatically");
  }
}
