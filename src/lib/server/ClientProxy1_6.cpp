/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Symless Ltd.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/ClientProxy1_6.h"

#include "arch/Arch.h"
#include "base/Log.h"
#include "deskflow/ClipboardChunk.h"
#include "deskflow/ClipboardLane.h"
#include "deskflow/ProtocolUtil.h"
#include "io/IStream.h"
#include "server/Server.h"

namespace {

//! Seconds between "clipboard dropped, this client has no lane" warnings, per client
/*!
Not a tuning knob so much as a floor on usefulness. Server::onScreenSwitch re-publishes the
clipboard on every crossing, so the un-limited version of this warning fires several times a second
while a pre-1.9 client is connected -- and the line that matters is the FIRST one. 30 seconds keeps
the situation visible in a log without the first occurrence being buried under its own repetitions.
*/
constexpr double kNoLaneWarningInterval = 30.0;

} // namespace

//
// ClientProxy1_6
//

ClientProxy1_6::ClientProxy1_6(const std::string &name, deskflow::IStream *stream, Server *server, IEventQueue *events)
    : ClientProxy1_5(name, stream, server, events),
      m_events(events)
{
  // The ClipboardSending handler that used to be registered here is GONE, with the whole
  // StreamChunker path it served (see setClipboard below). That also retires the documented
  // handler-leak concern: the constructor registered a handler keyed on `this`, the destructor did
  // not remove it, and Server::removeClient cleared every clipboard handler except that one. It was
  // measured NOT to be the cause of the 2026-07-28 storm -- an instrumented repro logged zero
  // ClipboardChunk::send calls throughout the failure -- but with no registration there is nothing
  // left to reason about.
  if (ClipboardChunk::diagEnabled()) {
    LOG_NOTE(
        "clipdiag: clientproxy1.6 CONSTRUCTED: proxy=%p stream=%p name=\"%s\"", static_cast<void *>(this),
        static_cast<void *>(getStream()), getName().c_str()
    );
  }
}

ClientProxy1_6::~ClientProxy1_6()
{
  if (ClipboardChunk::diagEnabled()) {
    LOG_NOTE(
        "clipdiag: clientproxy1.6 DESTROYED: proxy=%p stream=%p name=\"%s\"", static_cast<void *>(this),
        static_cast<void *>(getStream()), getName().c_str()
    );
  }
}

void ClientProxy1_6::setClipboard(ClipboardID id, const IClipboard *clipboard)
{
  // Every DECISION about whether to send is unchanged and stays exactly here: the dirty check, the
  // dirty-clearing, the copy, the marshalling. Only the TRANSPORT below is different.
  //
  // ignore if this clipboard is already clean
  if (m_clipboard[id].m_dirty) {
    // this clipboard is now clean
    m_clipboard[id].m_dirty = false;
    Clipboard::copy(&m_clipboard[id].m_clipboard, clipboard);

    std::string data = m_clipboard[id].m_clipboard.marshall();
    const size_t size = data.size();

    // The transport swap. This used to be StreamChunker::sendClipboard(), which queued the ENTIRE
    // train onto the main connection as one event per chunk -- in front of the keepalives the
    // client's 9-second death timer depends on. That is the wall-off loop this whole change exists
    // to end, so there is deliberately NO in-stream fallback: a clipboard that cannot go by lane is
    // dropped, loudly but harmlessly, rather than being allowed back onto the path that kills the
    // KVM link.
    //
    // Clearing m_dirty above without waiting for delivery is intentional and safe: the lane keeps
    // the LATEST payload per clipboard id until it goes out or is superseded, so a lane that comes
    // up late still delivers current state rather than replaying history.
    // Sequence number 0, exactly as the StreamChunker call this replaced passed it. A client
    // ignores the sequence number on a clipboard the server sends (ServerProxy::setClipboard reads
    // it and never uses it); it is the OTHER direction where the number is load-bearing.
    using SendResult = deskflow::ClipboardLaneManager::SendResult;
    switch (getServer()->clipboardLane().send(getName(), id, 0, std::move(data))) {
    case SendResult::Queued:
      LOG_DEBUG("sending clipboard %d to \"%s\" over the lane (%u bytes)", id, getName().c_str(),
                static_cast<uint32_t>(size));
      break;

    case SendResult::TooLarge:
      LOG_NOTE(
          "not sending clipboard %d to \"%s\": %u bytes is over the configured limit", id, getName().c_str(),
          static_cast<uint32_t>(size)
      );
      break;

    case SendResult::NoLane:
      // An older client, or a lane that is down or has not come up yet. Rate limited: see
      // kNoLaneWarningInterval.
      if (const double now = ARCH->time(); now - m_lastNoLaneWarning >= kNoLaneWarningInterval) {
        m_lastNoLaneWarning = now;
        LOG_WARN(
            "clipboard %d not delivered to \"%s\": no clipboard lane (client is older than protocol "
            "1.9, or its lane is down)",
            id, getName().c_str()
        );
      }
      break;
    }
  }
}

void ClientProxy1_6::applyLaneClipboard(ClipboardID id, uint32_t seqNum, const std::string &data)
{
  if (id >= kClipboardEnd) {
    return;
  }

  LOG_DEBUG("received client \"%s\" clipboard %d over the lane, seqnum=%d, size=%d", getName().c_str(), id, seqNum,
            static_cast<int>(data.size()));

  // Identical to recvClipboard()'s completion branch, on purpose -- same state, same event, so
  // nothing downstream can behave differently depending on which transport carried the bytes.
  m_clipboard[id].m_clipboard.unmarshall(data, 0);
  m_clipboard[id].m_sequenceNumber = seqNum;

  auto *info = new ClipboardInfo;
  info->m_id = id;
  info->m_sequenceNumber = seqNum;
  m_events->addEvent(Event(EventTypes::ClipboardChanged, getEventTarget(), info));
}

bool ClientProxy1_6::recvClipboard()
{
  // LEGACY RECEIVE -- deliberately untouched. A pre-1.9 client still sends its clipboard in-stream,
  // and a new server must keep understanding it; this is the half of the mixed-mesh story that
  // continues to work in both directions.
  //
  // parse message
  ClipboardID id;
  uint32_t seq;

  if (auto r = ClipboardChunk::assemble(getStream(), m_clipboardAssembly, id, seq); r == TransferState::Started) {
    LOG_DEBUG("receiving clipboard %d size=%d", id, m_clipboardAssembly[id].expectedSize);
  } else if (r == TransferState::Finished) {
    LOG(
        (CLOG_DEBUG "received client \"%s\" clipboard %d seqnum=%d, size=%d", getName().c_str(), id, seq,
         m_clipboardAssembly[id].data.size())
    );
    // save clipboard
    m_clipboard[id].m_clipboard.unmarshall(m_clipboardAssembly[id].data, 0);
    m_clipboard[id].m_sequenceNumber = seq;

    // notify
    auto *info = new ClipboardInfo;
    info->m_id = id;
    info->m_sequenceNumber = seq;
    m_events->addEvent(Event(EventTypes::ClipboardChanged, getEventTarget(), info));
  }

  return true;
}
