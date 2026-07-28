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
  bool verifyCertFingerprint(const QString &FingerprintDatabasePath) const;

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

  // MouseTransfer diagnostic: count reads so the FIRST few on any TLS socket can be traced. The
  // first bytes a freshly connected socket delivers are the whole question in the 2026-07-28
  // clipboard-chunk storm -- the server is proven to write only a 15-byte greeting, yet the client
  // parses a 524,291-byte packet length. Counter only; no behaviour.
  uint32_t m_diagReads = 0;
  SecurityLevel m_securityLevel = SecurityLevel::Encrypted;
};
