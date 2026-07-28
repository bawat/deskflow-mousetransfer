/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Symless Ltd.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "server/ClientProxy1_5.h"

class Server;
class IEventQueue;

//! Proxy for client implementing protocol version 1.6
class ClientProxy1_6 : public ClientProxy1_5
{
public:
  ClientProxy1_6(const std::string &name, deskflow::IStream *adoptedStream, Server *server, IEventQueue *events);
  // Not "= default": the destructor is defined out-of-line so proxy teardown can be TRACED.
  // See the note in ClientProxy1_6.cpp -- this object registers a ClipboardSending handler
  // keyed on `this`, and whether that registration outlives the object is exactly what the
  // 2026-07-28 investigation is trying to observe.
  ~ClientProxy1_6() override;

  void setClipboard(ClipboardID id, const IClipboard *clipboard) override;
  bool recvClipboard() override;

private:
  IEventQueue *m_events;
};
