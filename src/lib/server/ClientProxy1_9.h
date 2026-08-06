/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 MouseTransfer fork
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "server/ClientProxy1_8.h"

//! Proxy for client implementing protocol version 1.9
/*!
1.9 adds exactly one thing to 1.8: the client can be told about the dedicated clipboard lane
(see MT-CLIPBOARD-LANE-DESIGN.md). The class exists so that capability is expressed by a TYPE the
server only ever constructs for a client that announced 1.9 -- which is what makes it structurally
impossible to send kMsgDLaneAdvert to an older client. That matters more than it looks: an unknown
4-char code is not something a Deskflow client can skip (it cannot know the message's length), so
`ServerProxy::handleData` drains the entire stream and the connection is over. A runtime "if the
version is high enough" check would put that hazard one edited condition away; a type cannot be
edited by accident.
*/
class ClientProxy1_9 : public ClientProxy1_8
{
public:
  ClientProxy1_9(const std::string &name, deskflow::IStream *adoptedStream, Server *server, IEventQueue *events);
  ~ClientProxy1_9() override = default;

  //! Advertise the clipboard lane to this client
  /*!
  Sends kMsgDLaneAdvert carrying the lane wire version and \p token, the per-session secret that
  authorises exactly one lane connection for this session. The caller must have REGISTERED the
  session with the lane manager first, otherwise a fast client could dial a lane the server has not
  yet armed and be rejected for no reason.

  \p token is never logged, here or anywhere else.
  */
  void sendClipboardLaneAdvert(const std::string &token) const;
};
