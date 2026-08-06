/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 MouseTransfer fork
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "arch/IArchNetwork.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace deskflow {

namespace lane {

//! Drop the calling thread to background/idle scheduling, for its whole life
/*!
The OS seam for the clipboard lane's workers. They must never compete with input relay, and this
core runs at REALTIME priority class, where RELATIVE thread priorities cannot help: everything in
the process, including the lowest relative priority, still outranks ordinary work. Windows'
background mode is the escape hatch that does apply.

Returns TRUE when the thread really was demoted, FALSE when the OS refused. A refusal is not fatal
and the lane still works, but it is NOT cosmetic: an undemoted worker streaming a large clipboard
inside a REALTIME-class process competes directly with the input-relay thread, which is the exact
outcome this design exists to prevent. The caller must therefore make a refusal visible at WARN --
it is the first thing to look for behind "the whole machine went unresponsive during a transfer".
This function itself logs the OS error at WARN for the same reason.

Platforms that offer nothing sensible (macOS/BSD, where nice() is process-wide) report success:
there is nothing to fail, and a permanent warning would train the reader to ignore the line.
*/
bool demoteCurrentThreadToBackground();

//! Fill \p out with \p count cryptographically strong random bytes; false if none are available
/*!
Lives here because this is the layer that already links OpenSSL, and because a caller must be able
to treat "no secure randomness" as a hard refusal rather than quietly falling back to something
predictable. A lane token that can be guessed is worth less than no lane at all.
*/
bool secureRandomBytes(void *out, size_t count);

//! Compare two secrets without leaking where they first differ
/*!
Unequal lengths are reported unequal immediately -- that much is public, since the length of a token
this code issues is a constant. The bytes themselves are compared in constant time.
*/
bool secretsEqual(const std::string &a, const std::string &b);

} // namespace lane

//! A TLS connection owned outright, driven without the SocketMultiplexer
/*!
Wraps the three things SecureSocket::detachTls() hands over -- the socket handle, the SSL object and
its context -- and offers just enough to run a connection from a dedicated thread: readiness
waiting, partial reads, partial writes, and a shutdown that any thread may call.

**Threading contract.** Every method except shutdown() belongs to ONE thread for the object's whole
life, and the SSL object is touched by nobody else, ever. That is the entire concurrency design and
it is deliberate: OpenSSL forbids two threads inside one SSL object, and the obvious alternative --
a reader thread and a writer thread sharing a lock -- deadlocks the moment both endpoints send a
large clipboard at once, because progress then requires the reader to run WHILE the writer is
blocked inside SSL_write, which a lock cannot allow. See MT-CLIPBOARD-LANE-DESIGN.md §6.1.

**The socket stays non-blocking**, as the multiplexer left it. readSome()/writeSome() therefore
never block; waitReady() is where the waiting happens, with a caller-chosen timeout, so a worker can
always get back to its stop flag.

There is no static state of any kind in here. That is not an accident -- the 2026-07-28 outage was
per-connection TLS state living in function-local statics shared by every socket in the process.
*/
class LaneConn
{
public:
  LaneConn(const LaneConn &) = delete;
  LaneConn(LaneConn &&) = delete;
  LaneConn &operator=(const LaneConn &) = delete;
  LaneConn &operator=(LaneConn &&) = delete;
  ~LaneConn();

  //! Take ownership of a detached TLS connection
  /*!
  \p socket is one owned ArchSocket reference, \p ssl an SSL* and \p sslContext an SSL_CTX* (both
  void* so callers need no OpenSSL headers). On success the returned object owns all three and frees
  them in its destructor.

  \p pending is plaintext the previous owner had already decrypted but not consumed (usually
  empty). It is served by readSome() before anything is taken from the SSL object, so the stream
  the worker sees is continuous; waitReady() reports readable while any of it remains, because the
  socket itself will not -- those bytes are past the kernel already.

  **Ownership transfers unconditionally.** If the arguments do not describe a usable connection this
  returns nullptr, having already freed whatever it was given -- so a caller never has to unwind a
  half-handed-over connection, which it could not do anyway without the OpenSSL headers this
  interface exists to spare it. The alternative (nullptr, caller cleans up) left both call sites with
  a dead branch that leaked an SSL object each.
  */
  static std::unique_ptr<LaneConn> adopt(ArchSocket socket, void *ssl, void *sslContext, std::string pending = {});

  //! Readiness bits returned by waitReady()
  struct Ready
  {
    inline static const unsigned None = 0;
    inline static const unsigned Read = 1u << 0;
    inline static const unsigned Write = 1u << 1;
  };

  //! Wait up to \p seconds for readability and/or writability; returns the Ready bits (0 on timeout)
  unsigned waitReady(bool forRead, bool forWrite, double seconds);

  //! Read what is available now: >0 bytes read, 0 nothing available, -1 connection is over
  int readSome(void *buffer, uint32_t n);

  //! Write what can be written now: >0 bytes written, 0 not writable, -1 connection is over
  /*!
  A 0 return means "call again with THE SAME buffer and length" -- OpenSSL requires a retried
  SSL_write to present identical arguments, so the caller must keep its buffer alive and unchanged
  until a call reports progress.
  */
  int writeSome(const void *buffer, uint32_t n);

  //! Has this connection failed or been shut down?
  bool dead() const
  {
    return m_dead.load(std::memory_order_acquire);
  }

  //! Half-close the connection. Safe to call from any thread, and safe to call twice.
  /*!
  This is how a lane is torn down from the outside: it makes the peer see the end of the stream and
  makes any in-progress or subsequent wait return promptly. It does NOT free anything -- the owning
  thread still has to finish and the object still has to be destroyed.
  */
  void shutdown();

  //! Short identifier for log lines (never carries anything secret)
  std::string describe() const;

  //! Why this connection is dead, as a short literal; nullptr while it is still open
  /*!
  Set once, by whichever of the two ever happens first: the owning thread classifying a failed
  SSL_read/SSL_write, or another thread calling shutdown(). Readable from any thread (the value is
  always a string literal, so there is nothing to keep alive), which is the point -- the manager
  reports why a lane went down, and by then the worker is gone.
  */
  const char *deadReason() const
  {
    return m_deadReason.load(std::memory_order_acquire);
  }

  //! Plaintext OpenSSL has already decrypted but not handed over yet
  /*!
  Diagnostics only, and specifically the probe MT-CLIPBOARD-LANE-DESIGN.md §9b left for the rig: the
  read buffer is one TLS record's maximum plaintext and read_ahead is off, so this SHOULD always be
  zero after a read. If it ever is not, select() will not report the leftover -- the bytes are past
  the kernel -- and the lane stalls one poll interval per read. Owner thread only.
  */
  size_t pendingPlaintext() const;

private:
  LaneConn(ArchSocket socket, void *ssl, void *sslContext, std::string pending);

  //! Translate an SSL return code into our >0 / 0 / -1 convention, marking m_dead on a real failure
  int classify(int result, const char *what);

  //! Mark the connection dead, recording \p reason unless one was already recorded
  void markDead(const char *reason);

  ArchSocket m_socket = nullptr;
  void *m_ssl = nullptr;
  void *m_sslContext = nullptr;

  // Plaintext handed over with the connection, and how much of it has been served. Owner-thread
  // only, like everything else here except m_dead.
  std::string m_pending;
  size_t m_pendingAt = 0;

  // ATOMIC, and that is not decoration. shutdown() is documented as callable from any thread and
  // is how a lane is torn down from the outside, so this flag is WRITTEN by the main thread while
  // the owning worker READS it in its loop condition and in every readSome()/writeSome()/
  // waitReady(). A plain bool there is a data race -- formally UB, and on a weakly-ordered target
  // (this fork cross-builds for ARMv6) a worker really can go on not seeing the flag, leaving
  // teardown to depend entirely on the socket half-close waking select().
  std::atomic<bool> m_dead{false};

  // Always a string LITERAL, so publishing the pointer publishes the whole value and there is no
  // lifetime to reason about. Written by whichever thread kills the connection first, read by the
  // manager on the main thread after the worker has been joined.
  std::atomic<const char *> m_deadReason{nullptr};
};

} // namespace deskflow
