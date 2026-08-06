/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 MouseTransfer fork
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/ClientProxy1_9.h"

#include "base/Log.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"

ClientProxy1_9::ClientProxy1_9(
    const std::string &name, deskflow::IStream *adoptedStream, Server *server, IEventQueue *events
)
    : ClientProxy1_8(name, adoptedStream, server, events)
{
  // Deliberately nothing here. The advert is NOT sent from the constructor: at this point the
  // proxy has not been adopted by the Server, so no lane session exists yet and a client that
  // dialled immediately would be rejected with "no session for that name".
}

void ClientProxy1_9::sendClipboardLaneAdvert(const std::string &token) const
{
  // Length-prefixed (%s), so the token's raw bytes -- NULs included -- survive the wire intact.
  // Log the fact, never the value.
  LOG_DEBUG("advertising clipboard lane to \"%s\" (lane wire v%d)", getName().c_str(), kLaneWireVersion);
  ProtocolUtil::writef(getStream(), kMsgDLaneAdvert, kLaneWireVersion, &token);
}
