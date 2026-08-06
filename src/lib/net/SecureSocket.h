/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Symless Ltd.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "net/SecurityLevel.h"
#include "net/TCPSocket.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Event;
class IEventQueue;
class SocketMultiplexer;
class ISocketMultiplexerJob;
class QString;

struct Ssl;

//! Secure socket
/*!
A secure socket using SSL.
*/
class SecureSocket : public TCPSocket
{
public:
  SecureSocket(
      IEventQueue *events, SocketMultiplexer *socketMultiplexer, IArchNetwork::AddressFamily family,
      SecurityLevel securityLevel = SecurityLevel::Encrypted
  );
  SecureSocket(
      IEventQueue *events, SocketMultiplexer *socketMultiplexer, ArchSocket socket,
      SecurityLevel securityLevel = SecurityLevel::Encrypted
  );
  SecureSocket(SecureSocket const &) = delete;
  SecureSocket(SecureSocket &&) = delete;
  ~SecureSocket() override;

  SecureSocket &operator=(SecureSocket const &) = delete;
  SecureSocket &operator=(SecureSocket &&) = delete;

  // ISocket overrides
  void close() override;

  // IDataSocket overrides
  void connect(const NetworkAddress &) override;

  ISocketMultiplexerJob *newJob() override;
  bool isFatal() const override
  {
    return m_fatal;
  }
  void isFatal(bool b)
  {
    m_fatal = b;
  }
  bool isSecureReady() const;
  void secureConnect();
  void secureAccept();
  int secureRead(void *buffer, int size, int &read);
  int secureWrite(const void *buffer, int size, int &wrote);
  JobResult doRead() override;
  JobResult doWrite() override;
  void initSsl(bool server);
  bool loadCertificate(const QString &filename);

  //! The peer's certificate fingerprint, as lowercase hex of the SHA-256
  /*!
  Empty unless this socket was created at SecurityLevel::PeerAuth AND completed its handshake --
  those are the only circumstances in which a peer certificate is requested at all. Captured during
  verifyCertFingerprint() so that a caller can later ask "is this second connection from the same
  peer as that first one?" without re-reading the certificate off a socket it no longer owns.
  */
  const std::string &peerFingerprint() const
  {
    return m_peerFingerprint;
  }

  //! The pieces of a TLS connection, handed out by detachTls()
  struct DetachedTls
  {
    ArchSocket socket = nullptr; ///< one owned reference; release with ARCH->closeSocket
    void *ssl = nullptr;         ///< SSL*, opaque here so callers need no OpenSSL headers
    void *sslContext = nullptr;  ///< SSL_CTX*, likewise

    bool valid() const
    {
      return socket != nullptr && ssl != nullptr;
    }
  };

  //! Surrender the live TLS connection and become inert
  /*!
  Removes this socket from the SocketMultiplexer, then hands the caller the socket handle, the SSL
  object and its context, leaving this wrapper owning NOTHING: its destructor, close() and freeSSL()
  all become no-ops, and it reports itself disconnected. The connection stays open and usable
  through the returned pieces -- this is a transfer of ownership, not a shutdown.

  Returns an invalid DetachedTls (and changes nothing) unless every precondition holds: the socket
  is open, TLS is up, no partial SSL_write is latched, and neither the input nor the output buffer
  holds bytes. Those last three are the important ones -- a detach that stranded a half-written
  record or dropped buffered bytes would corrupt the stream in a way that looks exactly like the
  2026-07-28 outage, so it is refused instead. The caller's failure path is simply to close the
  connection normally.

  Callable from any thread, but in practice from the event-queue thread while the multiplexer
  thread may still be servicing this socket; the multiplexer removal is what makes that safe, and it
  happens first, exactly as close() does it.
  */
  DetachedTls detachTls();

private:
  // SSL
  void initContext(bool server);
  void createSSL();
  void freeSSL();
  int secureAccept(int s);
  int secureConnect(int s);
  bool showCertificate() const;
  void checkResult(int n, int &retry);
  void disconnect();
  // Not const any more: it is the one place that has the peer certificate in hand, so it is where
  // m_peerFingerprint gets filled in.
  bool verifyCertFingerprint(const QString &FingerprintDatabasePath);

  ISocketMultiplexerJob *serviceConnect(ISocketMultiplexerJob *const socket, bool, bool, bool);

  ISocketMultiplexerJob *serviceAccept(ISocketMultiplexerJob *const socket, bool, bool, bool);

  void handleTCPConnected(const Event &event);

private:
  // all accesses to m_ssl must be protected by this mutex. The only function that is called
  // from outside SocketMultiplexer thread is close(), so we mostly care about things accessed
  // by it.
  std::mutex ssl_mutex_;

  std::unique_ptr<Ssl> m_ssl;
  bool m_secureReady = false;
  bool m_fatal = false;

  // Pending-write state for doWrite(). These were FUNCTION-LOCAL STATICS (s_retry / s_retrySize /
  // s_staticBuffer / s_staticBufferSize), i.e. ONE set shared by every TLS socket in the process,
  // which is what caused the 2026-07-28 outage: a socket that got SSL_ERROR_WANT_WRITE latched
  // s_retry with its own payload still in the shared buffer, and if that connection then died
  // nothing cleared them -- so the NEXT socket's doWrite took the retry branch, used the DEAD
  // connection's size, never copied its own output buffer, and transmitted the dead connection's
  // data down the new connection. Per-socket members make that structurally impossible.
  //
  // A vector rather than the old realloc'd raw pointer: it is released with the socket, so the
  // buffer cannot outlive its owner and there is nothing to leak (the static version had already
  // needed one leak fix upstream, "#6488 Fixed a memory leak in the TLS socket code").
  bool m_writeRetry = false;
  int m_writeRetrySize = 0;
  std::vector<uint8_t> m_writeBuffer;

  // OpenSSL "want read/write/connect/accept" retry counters, one per operation. These were also
  // FUNCTION-LOCAL STATICS (four separate `static int retry;`) shared by every TLS socket, and they
  // are NOT merely untidy: checkResult() does `retry++` for every "want" case rather than assigning,
  // so the count accumulated across unrelated sockets, and ssl_mutex_ is a per-INSTANCE member so
  // nothing serialised them either. secureAccept/secureConnect then branch on `retry == 0` to decide
  // a handshake is complete -- meaning one socket finishing could make another socket mid-handshake
  // conclude it was secure, or a socket whose accept succeeded could stall because someone else had
  // left the counter positive. Per-operation so their independent semantics are preserved.
  int m_sslReadRetry = 0;
  int m_sslWriteRetry = 0;
  int m_sslAcceptRetry = 0;
  int m_sslConnectRetry = 0;

  // MouseTransfer diagnostic: count reads so the FIRST few on any TLS socket can be traced. The
  // first bytes a freshly connected socket delivers are the whole question in the 2026-07-28
  // clipboard-chunk storm -- the server is proven to write only a 15-byte greeting, yet the client
  // parses a 524,291-byte packet length. Counter only; no behaviour.
  uint32_t m_diagReads = 0;
  SecurityLevel m_securityLevel = SecurityLevel::Encrypted;

  // Lowercase hex SHA-256 of the peer's certificate, captured at handshake time. See
  // peerFingerprint(). Empty when no peer certificate was requested (SecurityLevel::Encrypted).
  std::string m_peerFingerprint;
};
