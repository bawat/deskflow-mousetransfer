/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Symless Ltd.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/ClipboardChunk.h"

#include "base/Log.h"
#include "base/String.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "io/IStream.h"
#include <cstdlib>
#include <cstring>
#include <fstream>

size_t ClipboardChunk::s_expectedSize = 0;

bool ClipboardChunk::diagEnabled()
{
  // Function-local static: computed on first use, thread-safe initialisation guaranteed by the
  // standard, and no dependency on static init order across translation units.
  //
  // A SENTINEL FILE is the primary switch, not just the environment variable, and that is
  // operational rather than stylistic. The elevated core inherits its environment from the
  // LocalSystem launcher, and that launcher SURVIVES a core swap (the swap works by bumping the
  // `coremode` nonce and letting the launcher respawn the core) -- so setting a machine
  // environment variable does NOT reach the core without also restarting the launcher chain,
  // which is exactly the kind of extra step that duplicates processes. A file needs no restart
  // at all: first use is the first chunk send, long after startup, so dropping the file in
  // enables tracing on a core that is already running. It also matches the fork's existing
  // file-driven controls (`switchreq`, `coremode`, `layoutreload`), which live in the same
  // directory -- the core's CWD, set by the launcher to the bundle dir.
  static const bool on = [] {
    if (const char *env = std::getenv("MOUSETRANSFER_CLIPDIAG"); env != nullptr && *env == '1') {
      return true;
    }
    return static_cast<bool>(std::ifstream("clipdiag"));
  }();
  return on;
}

ClipboardChunk::ClipboardChunk(size_t size) : Chunk(size)
{
  m_dataSize = size - s_clipboardChunkMetaSize;
}

ClipboardChunk *ClipboardChunk::start(ClipboardID id, uint32_t sequence, const std::string &size)
{
  size_t sizeLength = size.size();
  auto *start = new ClipboardChunk(sizeLength + s_clipboardChunkMetaSize);
  char *chunk = start->m_chunk;

  chunk[0] = id;
  std::memcpy(&chunk[1], &sequence, 4);
  chunk[5] = ChunkType::DataStart;
  memcpy(&chunk[6], size.c_str(), sizeLength);
  chunk[sizeLength + s_clipboardChunkMetaSize - 1] = '\0';

  return start;
}

ClipboardChunk *ClipboardChunk::data(ClipboardID id, uint32_t sequence, const std::string &data)
{
  size_t dataSize = data.size();
  auto *chunk = new ClipboardChunk(dataSize + s_clipboardChunkMetaSize);
  char *chunkData = chunk->m_chunk;

  chunkData[0] = id;
  std::memcpy(&chunkData[1], &sequence, 4);
  chunkData[5] = ChunkType::DataChunk;
  memcpy(&chunkData[6], data.c_str(), dataSize);
  chunkData[dataSize + s_clipboardChunkMetaSize - 1] = '\0';

  return chunk;
}

ClipboardChunk *ClipboardChunk::end(ClipboardID id, uint32_t sequence)
{
  auto *end = new ClipboardChunk(s_clipboardChunkMetaSize);
  char *chunk = end->m_chunk;

  chunk[0] = id;
  std::memcpy(&chunk[1], &sequence, 4);
  chunk[5] = ChunkType::DataEnd;
  chunk[s_clipboardChunkMetaSize - 1] = '\0';
  return end;
}

TransferState
ClipboardChunk::assemble(deskflow::IStream *stream, std::string &dataCached, ClipboardID &id, uint32_t &sequence)
{
  using enum TransferState;
  uint8_t mark;
  std::string data;

  if (!ProtocolUtil::readf(stream, kMsgDClipboard + 4, &id, &sequence, &mark, &data)) {
    return Error;
  }

  if (mark == ChunkType::DataStart) {
    s_expectedSize = QString::fromStdString(data).toULong();
    LOG_DEBUG("start receiving clipboard data");
    dataCached.clear();
    return Started;
  } else if (mark == ChunkType::DataChunk) {
    dataCached.append(data);
    return TransferState::InProgress;
  } else if (mark == ChunkType::DataEnd) {
    // validate
    if (id >= kClipboardEnd) {
      return Error;
    } else if (s_expectedSize != dataCached.size()) {
      LOG_ERR("corrupted clipboard data, expected size=%d actual size=%d", s_expectedSize, dataCached.size());
      return Error;
    }
    return Finished;
  }

  LOG_ERR("clipboard transmission failed: unknown error");
  return Error;
}

void ClipboardChunk::send(deskflow::IStream *stream, void *data)
{
  const auto *clipboardData = static_cast<ClipboardChunk *>(data);

  const char *chunk = clipboardData->m_chunk;
  ClipboardID id = chunk[0];
  uint32_t sequence;
  std::memcpy(&sequence, &chunk[1], 4);
  uint8_t mark = chunk[5];
  std::string dataChunk(&chunk[6], clipboardData->m_dataSize);

  // MouseTransfer diagnostic: log the DESTINATION STREAM alongside the chunk header, not just
  // "sending clipboard chunk". These events are queued against a raw proxy pointer
  // (StreamChunker::sendClipboard posts the whole train up front, EventQueue dispatches by
  // address), so the question that matters when clipboard data turns up on a connection that
  // never asked for it is *which stream did this chunk go to* -- which the old line could not
  // answer. A chunk whose mark is DataChunk arriving on a stream that has seen no DataStart is
  // a resumed, orphaned transfer.
  if (diagEnabled()) {
    LOG_NOTE(
        "clipdiag: sending clipboard chunk: stream=%p clipboard=%d sequence=%u mark=%d size=%d",
        static_cast<void *>(stream), static_cast<int>(id), sequence, static_cast<int>(mark),
        static_cast<int>(dataChunk.size())
    );
  } else {
    LOG_DEBUG1(
        "sending clipboard chunk: stream=%p clipboard=%d sequence=%u mark=%d size=%d", static_cast<void *>(stream),
        static_cast<int>(id), sequence, static_cast<int>(mark), static_cast<int>(dataChunk.size())
    );
  }

  switch (mark) {
  case ChunkType::DataStart:
    LOG_DEBUG2("sending clipboard chunk start: size=%s", dataChunk.c_str());
    break;

  case ChunkType::DataChunk:
    LOG_DEBUG2("sending clipboard chunk data: size=%i", dataChunk.size());
    break;

  case ChunkType::DataEnd:
    LOG_DEBUG2("sending clipboard finished");
    break;

  default:
    break;
  }

  ProtocolUtil::writef(stream, kMsgDClipboard, id, sequence, mark, &dataChunk);
}
