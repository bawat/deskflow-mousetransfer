/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016, 2026 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/IClient.h"

#include "base/EventTypes.h"
#include "deskflow/ClipboardLane.h"
#include "deskflow/IClipboard.h"
#include "net/NetworkAddress.h"

#include <climits>
#include <memory>

class ClipboardLaneDialer;
class Event;
class EventQueueTimer;
namespace deskflow {
class Screen;
}
class ServerProxy;
class IDataSocket;
class ISocketFactory;
namespace deskflow {
class IStream;
}
class IEventQueue;
class Thread;
class TCPSocket;

//! Deskflow client
/*!
This class implements the top-level client algorithms for deskflow.
*/
class Client : public IClient
{
public:
  class FailInfo
  {
  public:
    explicit FailInfo(const char *what) : m_what(what)
    {
      // do nothing
    }
    bool m_retry = false;
    std::string m_what;
  };

public:
  /*!
  This client will attempt to connect to the server using \p name
  as its name and \p address as the server's address and \p factory
  to create the socket.  \p screen is    the local screen.
  */
  Client(
      IEventQueue *events, const std::string &name, const NetworkAddress &address, ISocketFactory *socketFactory,
      deskflow::Screen *screen
  );
  Client(Client const &) = delete;
  Client(Client &&) = delete;
  ~Client() override;

  Client &operator=(Client const &) = delete;
  Client &operator=(Client &&) = delete;

  //! @name manipulators
  //@{

  //! Connect to server
  /*!
  Starts an attempt to connect to the server.  This is ignored if
  the client is trying to connect or is already connected.
  */
  void connect(size_t addressIndex = 0);
  void setServerAddress(const NetworkAddress &address);

  //! Disconnect
  /*!
  Disconnects from the server with an optional error message.
  */
  void disconnect(const char *msg);

  //! Refuse connection
  /*!
  Disconnects from the server with an optional error message.
  Unlike disconnect this function doesn't try to use other ip addresses
  */
  void refuseConnection(const char *msg);

  //! Notify of handshake complete
  /*!
  Notifies the client that the connection handshake has completed.
  */
  virtual void handshakeComplete();

  //! The server offered a dedicated clipboard connection (MouseTransfer fork)
  /*!
  Opens the local lane session and starts the dial. \p token authorises exactly one lane onto this
  session and is a SECRET -- it is not logged here or anywhere below this call.

  A second advert (the server re-armed the session, or we reconnected) supersedes the first: the old
  session and its lane go, and the dial starts again with the new token.
  */
  void onClipboardLaneAdvert(int16_t laneVersion, std::string token);

  //! Hand a marshalled clipboard to the lane for the server (MouseTransfer fork)
  /*!
  The transport half of ServerProxy::onClipboardChanged. Every DECISION about whether to send stays
  with the caller; this only routes the bytes, and reports what became of them. There is
  deliberately no in-stream fallback -- see MT-CLIPBOARD-LANE-DESIGN.md §5.

  \p sequenceNumber is the client's clipboard sequence number, which the server compares against
  what it last saw. It is carried end to end.

  \p payload is SHARED and immutable -- the one buffer sendClipboard() marshalled, which is also the
  dedup record it keeps. Nothing between here and the wire copies it again.
  */
  deskflow::ClipboardLaneManager::SendResult
  sendClipboardOverLane(ClipboardID id, uint32_t sequenceNumber, std::shared_ptr<const std::string> payload);

  //@}
  //! @name accessors
  //@{

  //! Test if connected
  /*!
  Returns true iff the client is successfully connected to the server.
  */
  bool isConnected() const;

  //! Test if connecting
  /*!
  Returns true iff the client is currently attempting to connect to
  the server.
  */
  bool isConnecting() const;

  //! Get address of server
  /*!
  Returns the address of the server the client is connected (or wants
  to connect) to.
  */
  NetworkAddress getServerAddress() const;

  //! Return last resolved adresses count
  size_t getLastResolvedAddressesCount() const
  {
    return m_resolvedAddressesCount;
  }

  //@}

  // IScreen overrides
  void *getEventTarget() const final;
  bool getClipboard(ClipboardID id, IClipboard *) const override;
  void getShape(int32_t &x, int32_t &y, int32_t &width, int32_t &height) const override;
  void getCursorPos(int32_t &x, int32_t &y) const override;

  // IClient overrides
  void enter(int32_t xAbs, int32_t yAbs, uint32_t seqNum, KeyModifierMask mask, bool forScreensaver) override;
  bool leave() override;
  void setClipboard(ClipboardID, const IClipboard *) override;
  void grabClipboard(ClipboardID) override;
  void setClipboardDirty(ClipboardID, bool) override;
  void keyDown(KeyID, KeyModifierMask, KeyButton, const std::string &) override;
  void keyRepeat(KeyID, KeyModifierMask, int32_t count, KeyButton, const std::string &lang) override;
  void keyUp(KeyID, KeyModifierMask, KeyButton) override;
  void mouseDown(ButtonID) override;
  void mouseUp(ButtonID) override;
  void mouseMove(int32_t xAbs, int32_t yAbs) override;
  void mouseRelativeMove(int32_t xRel, int32_t yRel) override;
  void mouseWheel(int32_t xDelta, int32_t yDelta) override;
  void screensaver(bool activate) override;
  void resetOptions() override;
  void setOptions(const OptionsList &options) override;
  std::string getName() const override;

private:
  void sendClipboard(ClipboardID);
  void sendEvent(deskflow::EventTypes);
  void sendConnectionFailedEvent(const char *msg);
  void setupConnecting();
  void setupConnection();
  void setupScreen();
  void setupTimer();
  void cleanup();
  void cleanupConnecting();
  void cleanupConnection();
  void cleanupScreen();
  void cleanupTimer();
  void cleanupStream();
  void handleConnected();
  void handleConnectionFailed(const Event &event);
  void handleConnectTimeout();
  void handleOutputError();
  void handleDisconnected();
  void handleShapeChanged();
  void handleClipboardGrabbed(const Event &event);
  void handleHello();
  void handleSuspend();
  void handleResume();
  void sendClipboardThread(void *);
  void bindNetworkInterface(IDataSocket *socket) const;

  // MouseTransfer clipboard lane. -------------------------------------------------------------
  //! A complete payload arrived from the server over the lane
  /*!
  On the MAIN thread -- a lane worker gets here by posting an event, precisely so that the apply may
  touch screen and proxy state.
  */
  void handleLaneClipboard(const deskflow::LaneClipboardInfo &info);
  //! Re-dial if the lane has quietly died. Cheap, idempotent, and a no-op with a lane up.
  void ensureClipboardLane();
  //! Drop the lane and anything dialling one. Idempotent; safe in any state.
  void cleanupLane();
  //! The configured clipboard limit in BYTES, saturating rather than overflowing
  size_t maxClipboardBytes() const;

private:
  std::string m_name;
  NetworkAddress m_serverAddress;
  ISocketFactory *m_socketFactory = nullptr;
  deskflow::Screen *m_screen = nullptr;
  deskflow::IStream *m_stream = nullptr;
  EventQueueTimer *m_timer = nullptr;
  ServerProxy *m_server = nullptr;
  bool m_ready = false;
  bool m_active = false;
  bool m_suspended = false;
  bool m_connectOnResume = false;
  bool m_ownClipboard[kClipboardEnd];
  bool m_sentClipboard[kClipboardEnd];
  IClipboard::Time m_timeClipboard[kClipboardEnd];
  // The last payload SENT for each clipboard id, kept only to answer "has this changed". Shared
  // with the lane rather than copied for it -- see sendClipboard().
  std::shared_ptr<const std::string> m_dataClipboard[kClipboardEnd];
  IEventQueue *m_events = nullptr;
  bool m_useSecureNetwork = false;
  bool m_enableClipboard = true;
  size_t m_maximumClipboardSize = INT_MAX;
  size_t m_resolvedAddressesCount = 0;

  // MouseTransfer clipboard lane. Built in the constructor and destroyed with this object, so it is
  // always valid; its destructor stops and joins the lane worker, which is why nothing else in the
  // client has to think about worker lifetimes. The dialer exists only between an advert and the
  // end of the main session.
  std::unique_ptr<deskflow::ClipboardLaneManager> m_clipboardLane;
  std::unique_ptr<ClipboardLaneDialer> m_laneDialer;
};
