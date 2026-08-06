/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Symless Ltd.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardChunk.h"
#include "server/ClientProxy1_5.h"

class Server;
class IEventQueue;

//! Proxy for client implementing protocol version 1.6
class ClientProxy1_6 : public ClientProxy1_5
{
public:
  ClientProxy1_6(const std::string &name, deskflow::IStream *adoptedStream, Server *server, IEventQueue *events);
  // Not "= default": the destructor is defined out-of-line so proxy teardown can be TRACED.
  // See the note in ClientProxy1_6.cpp.
  ~ClientProxy1_6() override;

  void setClipboard(ClipboardID id, const IClipboard *clipboard) override;
  bool recvClipboard() override;

  //! Apply a clipboard that arrived over the dedicated lane
  /*!
  Does exactly what recvClipboard()'s completion branch does -- stores the payload and raises
  ClipboardChanged -- for data that came in over the lane instead of in-stream. Deliberately the
  same code path and the same event, so the server cannot tell (and must not care) which transport
  a clipboard used.

  Main thread only; the lane manager gets it here by posting an event.
  */
  void applyLaneClipboard(ClipboardID id, uint32_t seqNum, const std::string &data);

private:
  IEventQueue *m_events;

  // Rate limit for the "no lane, clipboard dropped" warning. A clipboard is re-published on every
  // screen switch, so an un-limited warning would produce a line per crossing for as long as an old
  // client stays connected -- which buries the first occurrence, the only one that carries news.
  double m_lastNoLaneWarning = 0.0;

  // Per-connection, per-clipboard reassembly state. Was a `static std::string dataCached` inside
  // recvClipboard() -- ONE buffer shared by both clipboard ids AND EVERY CONNECTED CLIENT, so two
  // clients sending clipboards at once interleaved into a single buffer. See
  // ClipboardChunk::Assembly.
  ClipboardChunk::Assembly m_clipboardAssembly[kClipboardEnd];
};
