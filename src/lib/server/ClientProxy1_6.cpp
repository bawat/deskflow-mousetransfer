/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Symless Ltd.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/ClientProxy1_6.h"

#include "base/Log.h"
#include "deskflow/ClipboardChunk.h"
#include "deskflow/ProtocolUtil.h"
#include "deskflow/StreamChunker.h"
#include "io/IStream.h"
#include "server/Server.h"

//
// ClientProxy1_6
//

ClientProxy1_6::ClientProxy1_6(const std::string &name, deskflow::IStream *stream, Server *server, IEventQueue *events)
    : ClientProxy1_5(name, stream, server, events),
      m_events(events)
{
  m_events->addHandler(EventTypes::ClipboardSending, this, [this](const auto &e) {
    ClipboardChunk::send(getStream(), e.getDataObject());
  });
  // MouseTransfer diagnostic: pair this with the destructor line below and with
  // ClipboardChunk::send's stream= field. Together they answer the question the logs could not:
  // is a ClipboardSending event still being delivered through a proxy that has already been
  // destroyed, and onto which stream?
  if (ClipboardChunk::diagEnabled()) {
    LOG_NOTE(
        "clipdiag: clientproxy1.6 CONSTRUCTED: proxy=%p stream=%p name=\"%s\"", static_cast<void *>(this),
        static_cast<void *>(getStream()), getName().c_str()
    );
  }
}

ClientProxy1_6::~ClientProxy1_6()
{
  // DELIBERATELY only logging -- this destructor does NOT remove the ClipboardSending handler
  // registered on `this` in the constructor. Adding that removal is the leading candidate FIX
  // for the 2026-07-28 clipboard-chunk storm (Server::removeClient clears ScreenShapeChanged /
  // ClipboardGrabbed / ClipboardChanged but not ClipboardSending, and EventQueue::dispatchEvent
  // resolves handlers by raw void*), and it is being kept OUT of this instrumentation on
  // purpose: fixing and measuring in the same build would destroy the evidence that the fix is
  // the right one. Restore the removal only as a reviewed change, not as a drive-by.
  if (ClipboardChunk::diagEnabled()) {
    LOG_NOTE(
        "clipdiag: clientproxy1.6 DESTROYED: proxy=%p stream=%p name=\"%s\" (ClipboardSending handler NOT removed)",
        static_cast<void *>(this), static_cast<void *>(getStream()), getName().c_str()
    );
  }
}

void ClientProxy1_6::setClipboard(ClipboardID id, const IClipboard *clipboard)
{
  // ignore if this clipboard is already clean
  if (m_clipboard[id].m_dirty) {
    // this clipboard is now clean
    m_clipboard[id].m_dirty = false;
    Clipboard::copy(&m_clipboard[id].m_clipboard, clipboard);

    std::string data = m_clipboard[id].m_clipboard.marshall();

    size_t size = data.size();
    LOG_DEBUG("sending clipboard %d to \"%s\"", id, getName().c_str());

    StreamChunker::sendClipboard(data, size, id, 0, m_events, this);
  }
}

bool ClientProxy1_6::recvClipboard()
{
  // parse message
  static std::string dataCached;
  ClipboardID id;
  uint32_t seq;

  if (auto r = ClipboardChunk::assemble(getStream(), dataCached, id, seq); r == TransferState::Started) {
    size_t size = ClipboardChunk::getExpectedSize();
    LOG_DEBUG("receiving clipboard %d size=%d", id, size);
  } else if (r == TransferState::Finished) {
    LOG(
        (CLOG_DEBUG "received client \"%s\" clipboard %d seqnum=%d, size=%d", getName().c_str(), id, seq,
         dataCached.size())
    );
    // save clipboard
    m_clipboard[id].m_clipboard.unmarshall(dataCached, 0);
    m_clipboard[id].m_sequenceNumber = seq;

    // notify
    auto *info = new ClipboardInfo;
    info->m_id = id;
    info->m_sequenceNumber = seq;
    m_events->addEvent(Event(EventTypes::ClipboardChanged, getEventTarget(), info));
  }

  return true;
}
