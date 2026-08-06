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

#include <memory>
#include <utility>

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

    // MouseTransfer, stage 5: SHARE the server's marshalling rather than producing an identical one.
    // The copy above is proxy STATE (what this client is believed to hold) and stays; what does not
    // need to be per-proxy is the marshalled payload, which is a pure function of the clipboard the
    // server just handed us. Both callers pass the server's own clipboard object, so
    // marshalledClipboard() answers with the buffer built in onClipboardChanged -- one allocation
    // and one memcpy of the whole clipboard per client, gone, plus one retained copy per client for
    // as long as the lane holds it. The fallback marshalls locally, so an unrecognised source object
    // still works exactly as before.
    std::shared_ptr<const std::string> data = getServer()->marshalledClipboard(id, clipboard);
    if (!data) {
      data = std::make_shared<const std::string>(m_clipboard[id].m_clipboard.marshall());
    }
    const size_t size = data->size();

    // The transport swap. This used to be StreamChunker::sendClipboard(), which queued the ENTIRE
    // train onto the main connection as one event per chunk -- in front of the keepalives the
    // client's 9-second death timer depends on. That is the wall-off loop this whole change exists
    // to end, so there is deliberately NO in-stream fallback: a clipboard that cannot go by lane is
    // dropped, loudly but harmlessly, rather than being allowed back onto the path that kills the
    // KVM link.
    //
    // Clearing m_dirty above without waiting for delivery is intentional and safe ONLY because the
    // lane manager retains the LATEST payload per clipboard id whether or not a lane exists yet --
    // see ClipboardLaneManager::send(). Nothing re-offers this clipboard: Server only marks a
    // client dirty again when the clipboard CHANGES, so a payload dropped here is gone until the
    // user copies something else.
    // Sequence number 0, exactly as the StreamChunker call this replaced passed it. A client
    // ignores the sequence number on a clipboard the server sends (ServerProxy::setClipboard reads
    // it and never uses it); it is the OTHER direction where the number is load-bearing.
    //
    // The CANONICAL screen name, never this proxy's own. Server::getName() runs every client name
    // through Config::getCanonicalName -- a CASELESS lookup that also resolves aliases -- and keys
    // m_clients, the lane sessions and the lane teardown by the result. getName() here is what the
    // client announced about ITSELF, so on any screen whose configured spelling differs by case or
    // is an alias the two are different strings, the session lookup misses, and clipboard sync dies
    // silently in both directions.
    using SendResult = deskflow::ClipboardLaneManager::SendResult;
    const std::string peer = getServer()->canonicalName(getName());
    switch (getServer()->clipboardLane().send(peer, id, 0, std::move(data))) {
    case SendResult::Queued:
      LOG_DEBUG("sending clipboard %d to \"%s\" over the lane (%u bytes)", id, peer.c_str(),
                static_cast<uint32_t>(size));
      break;

    case SendResult::Held:
      // The client speaks 1.9 and has a session, but its lane is not up (yet, or any more). The
      // payload is kept and goes out when the client's next dial lands.
      //
      // INFO, not DEBUG: on a deployed fleet the core logs at the default level, and "the clipboard
      // is waiting for a lane that never came up" is the shape of every delivery complaint this
      // feature can produce. Paired with the lane up/down lines it says whether the wait ended.
      LOG_INFO("clipboard %d for \"%s\" is waiting for the lane (%u bytes)", id, peer.c_str(),
               static_cast<uint32_t>(size));
      break;

    case SendResult::TooLarge:
      LOG_NOTE(
          "not sending clipboard %d to \"%s\": %u bytes is over the configured limit", id, peer.c_str(),
          static_cast<uint32_t>(size)
      );
      break;

    case SendResult::NoLane:
      // No SESSION at all, which on the server means exactly one thing: this client is older than
      // protocol 1.9, so it was never offered a lane. Rate limited: see kNoLaneWarningInterval.
      if (const double now = ARCH->time();
          !m_noLaneWarned || now - m_lastNoLaneWarning >= kNoLaneWarningInterval) {
        m_lastNoLaneWarning = now;
        m_noLaneWarned = true;
        LOG_WARN(
            "clipboard %d not delivered to \"%s\": the client is older than protocol 1.9 and has no "
            "clipboard lane",
            id, peer.c_str()
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
