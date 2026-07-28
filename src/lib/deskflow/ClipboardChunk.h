/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Symless Ltd.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/Chunk.h"
#include "deskflow/ClipboardTypes.h"
#include "deskflow/ProtocolTypes.h"

#include <string>

constexpr static auto s_clipboardChunkMetaSize = 7;

namespace deskflow {
class IStream;
}

class ClipboardChunk : public Chunk
{
public:
  explicit ClipboardChunk(size_t size);

  static ClipboardChunk *start(ClipboardID id, uint32_t sequence, const std::string &size);
  static ClipboardChunk *data(ClipboardID id, uint32_t sequence, const std::string &data);
  static ClipboardChunk *end(ClipboardID id, uint32_t sequence);

  //! In-progress reassembly state for ONE clipboard of ONE connection.
  /*!
  Callers hold an array of these, one per clipboard id, as a MEMBER of the connection object.
  This replaces two pieces of shared mutable state that between them caused the 2026-07-28
  outage's data corruption: the caller-side `static std::string dataCached` (one buffer for every
  clipboard id AND every connection) and `ClipboardChunk::s_expectedSize` (a file-scope static,
  likewise process-wide). With both shared, two overlapping transfers appended into one buffer and
  were validated against one expected size -- producing the observed
  `expected size=8294452 actual size=17113192`, i.e. exactly two whole transfers plus one chunk.
  */
  struct Assembly
  {
    std::string data;
    size_t expectedSize = 0;

    void reset()
    {
      data.clear();
      expectedSize = 0;
    }
  };

  //! Reassemble one incoming chunk into the per-clipboard state of ONE connection
  /*!
  \p state is indexed by the clipboard id read off the wire, so a transfer for clipboard 0 can
  never disturb clipboard 1, and one connection can never disturb another.
  */
  static TransferState
  assemble(deskflow::IStream *stream, Assembly (&state)[kClipboardEnd], ClipboardID &id, uint32_t &sequence);

  static void send(deskflow::IStream *stream, void *data);


  // MouseTransfer diagnostic gate (2026-07-28 clipboard-chunk storm). The chunk-level tracing
  // this enables is per 512 KB chunk, not per frame, so it is cheap -- but it is OFF unless
  // switched on, because the core runs at its DEFAULT log level in the field. That matters:
  // logging these at LOG_DEBUG1 alone made them invisible on exactly the deployed cores we need
  // to observe, which would have wasted a whole repro.
  //
  // Switched on by a `clipdiag` SENTINEL FILE in the core's working directory (the bundle dir,
  // alongside `switchreq` / `coremode` / `layoutreload`), or by MOUSETRANSFER_CLIPDIAG=1. Prefer
  // the file: it needs no restart of anything -- see the rationale in the definition.
  // Evaluated once and cached.
  static bool diagEnabled();

};
