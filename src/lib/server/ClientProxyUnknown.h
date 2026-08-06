/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <string>

class ClientProxy;
class EventQueueTimer;
namespace deskflow {
class IStream;
}
class Server;
class IEventQueue;

class ClientProxyUnknown
{
public:
  ClientProxyUnknown(deskflow::IStream *stream, double timeout, Server *server, IEventQueue *events);
  ClientProxyUnknown(ClientProxyUnknown const &) = delete;
  ClientProxyUnknown(ClientProxyUnknown &&) = delete;
  ~ClientProxyUnknown();

  ClientProxyUnknown &operator=(ClientProxyUnknown const &) = delete;
  ClientProxyUnknown &operator=(ClientProxyUnknown &&) = delete;

  //! @name manipulators
  //@{

  //! Get the client proxy
  /*!
  Returns the client proxy created after a successful handshake
  (i.e. when this object sends a success event).  Returns nullptr
  if the handshake is unsuccessful or incomplete.
  */
  ClientProxy *orphanClientProxy();

  //! Get the stream
  deskflow::IStream *getStream()
  {
    return m_stream;
  }

  //@}

private:
  void sendSuccess();
  void sendFailure();
  void addStreamHandlers();
  void addProxyHandlers();
  void removeHandlers();
  void initProxy(const std::string &name, int major, int minor);
  void removeTimer();
  void handleData();

  //! Handle a connection that greeted us with kMsgMTLaneHello instead of kMsgHelloBack
  /*!
  Validates it against a live session, detaches the TLS connection and hands it to the clipboard
  lane. Returns true if it consumed the connection one way or the other (accepted OR rejected) --
  either way this object is finished with it and has already arranged its own teardown.

  Nothing in here may touch, delay or fail the peer's MAIN session. The worst outcome of a bad lane
  greeting is that one extra TCP connection is closed and a line appears in the debug log.
  */
  bool handleLaneHello();
  void handleWriteError();
  void handleTimeout();
  void handleDisconnect();

private:
  deskflow::IStream *m_stream = nullptr;
  EventQueueTimer *m_timer = nullptr;
  ClientProxy *m_proxy = nullptr;
  bool m_ready = false;
  Server *m_server = nullptr;
  IEventQueue *m_events = nullptr;
};
