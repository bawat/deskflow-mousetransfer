/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Symless Ltd.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "SecureSocket.h"
#include "SecureUtils.h"

#include "arch/ArchException.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "common/Settings.h"
#include "io/StreamFilter.h"
#include "mt/Lock.h"
#include "net/FingerprintDatabase.h"
#include "net/TCPSocket.h"
#include "net/TSocketMultiplexerMethodJob.h"
#include <net/SslLogger.h>

#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <openssl/err.h>
#include <openssl/ssl.h>

//
// SecureSocket
//
static const std::size_t s_maxInputBufferSize = 1024 * 1024;

static const float s_retryDelay = 0.01f;

struct Ssl
{
  SSL_CTX *m_context = nullptr;
  SSL *m_ssl = nullptr;
};

static int verifyIgnoreCertCallback(X509_STORE_CTX *, void *)
{
  return 1;
}

SecureSocket::SecureSocket(
    IEventQueue *events, SocketMultiplexer *socketMultiplexer, IArchNetwork::AddressFamily family,
    SecurityLevel securityLevel
)
    : TCPSocket(events, socketMultiplexer, family),
      m_securityLevel{securityLevel}
{
  // do nothing
}

SecureSocket::SecureSocket(
    IEventQueue *events, SocketMultiplexer *socketMultiplexer, ArchSocket socket, SecurityLevel securityLevel
)
    : TCPSocket(events, socketMultiplexer, socket),
      m_securityLevel{securityLevel}
{
  // do nothing
}

SecureSocket::~SecureSocket()
{
  freeSSL();
}

void SecureSocket::close()
{
  freeSSL();
  TCPSocket::close();
}

void SecureSocket::connect(const NetworkAddress &addr)
{
  getEvents()->addHandler(EventTypes::DataSocketConnected, getEventTarget(), [this](const auto &e) {
    handleTCPConnected(e);
  });
  TCPSocket::connect(addr);
}

ISocketMultiplexerJob *SecureSocket::newJob()
{
  // after TCP connection is established, SecureSocket will pick up
  // connected event and do secureConnect
  if (isConnected() && !m_secureReady) {
    return nullptr;
  }

  return TCPSocket::newJob();
}

void SecureSocket::secureConnect()
{
  setJob(new TSocketMultiplexerMethodJob<SecureSocket>(
      this, &SecureSocket::serviceConnect, getSocket(), isReadable(), isWritable()
  ));
}

void SecureSocket::secureAccept()
{
  setJob(new TSocketMultiplexerMethodJob<SecureSocket>(
      this, &SecureSocket::serviceAccept, getSocket(), isReadable(), isWritable()
  ));
}

TCPSocket::JobResult SecureSocket::doRead()
{
  using enum JobResult;
  // Deliberately a plain LOCAL, matching TCPSocket::doRead. This was `static uint8_t buffer[4096]`
  // -- one receive buffer shared by every TLS socket in the process. It was not the cause of the
  // 2026-07-28 outage (each call memsets and refills it before use), but it is the same hazard
  // class as the write-side statics that WERE, and there is no reason for it: 4 KB of stack costs
  // nothing and the plaintext path has always done it this way.
  uint8_t buffer[4096];
  const auto bufferSize = std::size(buffer);
  memset(buffer, 0, bufferSize);
  int bytesRead = 0;
  int status = 0;

  if (isSecureReady()) {
    status = secureRead(buffer, bufferSize, bytesRead);
    if (status < 0) {
      return Break;
    } else if (status == 0) {
      return New;
    }
  } else {
    return Retry;
  }

  if (bytesRead > 0) {
    bool wasEmpty = (m_inputBuffer.getSize() == 0);

    // MouseTransfer diagnostic: what did this socket ACTUALLY receive first? This is the trace
    // that caught the 2026-07-28 outage -- a freshly connected socket whose FIRST read began
    // `00 08 00 0e 44 43 4c 50` (a 524,302-byte clipboard chunk) instead of the 15-byte greeting
    // the server had written to it. Keep the buffer address in the line: it is what makes a
    // shared-buffer bug visible at a glance.
    if (++m_diagReads <= 3 && mtDiagEnabled()) {
      char hex[3 * 12 + 1] = {0};
      const int show = bytesRead < 12 ? bytesRead : 12;
      for (int i = 0; i < show; ++i) {
        snprintf(hex + i * 3, 4, "%02x ", buffer[i]);
      }
      LOG_NOTE(
          "clipdiag: TLS READ #%u: sock=%p bytes=%d bufaddr=%p first: %s", m_diagReads,
          static_cast<void *>(getSocket()), bytesRead, static_cast<void *>(buffer), hex
      );
    }

    // slurp up as much as possible
    do {
      m_inputBuffer.write(buffer, bytesRead);

      if (m_inputBuffer.getSize() > s_maxInputBufferSize) {
        break;
      }

      status = secureRead(buffer, bufferSize, bytesRead);
      if (status < 0) {
        return Break;
      }
    } while (bytesRead > 0 || status > 0);

    // send input ready if input buffer was empty
    if (wasEmpty) {
      sendEvent(EventTypes::StreamInputReady);
    }
  } else {
    // remote write end of stream hungup.  our input side
    // has therefore shutdown but don't flush our buffer
    // since there's still data to be read.
    sendEvent(EventTypes::StreamInputShutdown);
    if (!isWritable() && m_inputBuffer.getSize() == 0) {
      sendEvent(EventTypes::SocketDisconnected);
      setConnected(false);
    }
    setReadable(false);
    return New;
  }

  return Retry;
}

TCPSocket::JobResult SecureSocket::doWrite()
{
  using enum JobResult;

  // NOTE: the pending-write state below used to be four FUNCTION-LOCAL STATICS shared by every TLS
  // socket in the process. See the members' declaration in SecureSocket.h for the outage that
  // caused. They are per-socket now; do NOT "simplify" them back.
  //
  // OpenSSL additionally requires that a retried SSL_write be handed the SAME buffer contents at
  // the same address as the call that returned WANT_WRITE, which is why the retry path must not
  // re-peek the output buffer -- and precisely why sharing one buffer between sockets was fatal
  // rather than merely untidy.

  // write data
  int bufferSize = 0;
  int bytesWrote = 0;
  int status = 0;

  if (m_writeRetry) {
    bufferSize = m_writeRetrySize;
  } else {
    bufferSize = m_outputBuffer.getSize();
    if (bufferSize != 0) {
      if (static_cast<size_t>(bufferSize) > m_writeBuffer.size()) {
        m_writeBuffer.resize(static_cast<size_t>(bufferSize));
      }
      memcpy(m_writeBuffer.data(), m_outputBuffer.peek(bufferSize), bufferSize);
    }
  }

  if (bufferSize == 0) {
    return Retry;
  }

  if (isSecureReady()) {
    status = secureWrite(m_writeBuffer.data(), bufferSize, bytesWrote);
    if (status > 0) {
      m_writeRetry = false;
    } else if (status < 0) {
      return Break;
    } else if (status == 0) {
      m_writeRetry = true;
      m_writeRetrySize = bufferSize;
      return New;
    }
  } else {
    return Retry;
  }

  if (bytesWrote > 0) {
    discardWrittenData(bytesWrote);
    return New;
  }

  return Retry;
}

SecureSocket *SecureSocket::fromStream(deskflow::IStream *stream)
{
  auto *filter = dynamic_cast<StreamFilter *>(stream);
  if (filter == nullptr) {
    return nullptr;
  }
  // An adopted stream is owned (and deleted) by the filter; ClientListener builds its filters with
  // adopt=false and keeps the socket itself, so this also rejects any chain that is not the one the
  // handoff was designed around.
  if (filter->adoptedStream()) {
    return nullptr;
  }
  return dynamic_cast<SecureSocket *>(filter->getStream());
}

SecureSocket::DetachedTls SecureSocket::detachTls()
{
  DetachedTls out;

  // Leave the multiplexer FIRST, exactly as close() does. removeSocket() breaks the service thread
  // out of its poll and takes the job list lock, so once it returns no job can be running against
  // this socket -- which is what makes it safe to take the object apart from the event-queue
  // thread while the service thread is alive. It also means the checks below read buffers that
  // nothing else can be mutating, which is the whole reason the removal cannot come after them.
  setJob(nullptr);

  // ...and that is exactly why a REFUSAL has to put the job back. Leaving early with the socket
  // out of the multiplexer would silently strand a live connection: no reads, no writes, no
  // disconnect event, just silence. Today's two callers both close after a refusal so they never
  // notice, but "this method half-destroys the socket when it says no" is a trap laid for the
  // third one, and this fork's rule is to close a trap rather than document it and hope. Recomputed
  // under the lock (newJob() requires it) and applied after it (setJob() must not be called under
  // it -- close() takes the same care).
  ISocketMultiplexerJob *restore = nullptr;
  bool restoreJob = false;
  {
    Lock lock(&getMutex());
    std::scoped_lock ssl_lock{ssl_mutex_};

    // Refuse anything that is not a quiescent, live, secured connection. Each of these would be a
    // stream-corrupting detach rather than a clean transfer, and the caller's fallback (close the
    // connection normally) is cheap, so refusing is always the better answer:
    //
    //  * no socket / no SSL / not secure  -- there is nothing to hand over.
    //  * m_writeRetry                     -- a partial SSL_write is latched, and OpenSSL requires the
    //                                        retry to present the SAME buffer. Handing the SSL object
    //                                        to code that knows nothing about that pending write is
    //                                        how the 2026-07-28 outage's corruption looked.
    //  * unwritten output                 -- bytes the peer is still waiting for.
    //
    // Unread INPUT is deliberately NOT in that list. It used to be, and it was wrong: the peer may
    // legitimately speak as soon as IT has finished its own handoff, which can happen before this
    // end has run the event-queue hop that gets it here -- so refusing meant a perfectly good
    // connection was dropped for a race that gets MORE likely the more the peer had to say. Those
    // bytes are already decrypted and already ours; they are handed over with everything else and
    // the new owner presents them before its first read.
    bool refused = true;
    if (getSocket() == nullptr || !m_ssl || m_ssl->m_ssl == nullptr || !m_secureReady) {
      LOG_DEBUG("tls detach refused: connection is not live/secure");
    } else if (m_writeRetry) {
      LOG_DEBUG("tls detach refused: a partial tls write is pending");
    } else if (m_outputBuffer.getSize() != 0) {
      LOG_DEBUG("tls detach refused: %u byte(s) still buffered for the peer", m_outputBuffer.getSize());
    } else {
      refused = false;

      // Carry across anything already read but not consumed. Emptied rather than discarded, so
      // that releaseSocket()'s "both buffers are empty" precondition still holds by the time it
      // runs and nothing can silently drop these.
      if (const uint32_t buffered = m_inputBuffer.getSize(); buffered != 0) {
        out.pending.assign(static_cast<const char *>(m_inputBuffer.peek(buffered)), buffered);
        m_inputBuffer.pop(buffered);
        LOG_DEBUG("tls detach: carrying %u byte(s) the peer had already sent", buffered);
      }

      // Hand over the SSL objects and stop pointing at them. m_ssl (the holder) stays alive but
      // empty, so freeSSL() -- which the destructor and close() both call -- finds nothing to free.
      out.ssl = m_ssl->m_ssl;
      out.sslContext = m_ssl->m_context;
      m_ssl->m_ssl = nullptr;
      m_ssl->m_context = nullptr;
      m_secureReady = false;

      // Hand over the socket handle and stop pointing at that too. releaseSocket() also puts the
      // wrapper into the disconnected state, so close() will not announce a SocketDisconnected for
      // a connection that is very much alive in someone else's hands.
      out.socket = releaseSocket();
    }

    if (refused) {
      restore = newJob();
      restoreJob = true;
    }
  }

  if (restoreJob) {
    // newJob() may legitimately be null (nothing to wait for); setJob handles that as a removal,
    // which is where we already are, so this is safe either way.
    setJob(restore);
    return out;
  }

  LOG_DEBUG("tls connection detached for the clipboard lane");
  return out;
}

int SecureSocket::secureRead(void *buffer, int size, int &read)
{
  std::scoped_lock ssl_lock{ssl_mutex_};

  if (m_ssl->m_ssl != nullptr) {
    LOG_DEBUG2("reading secure socket");
    read = SSL_read(m_ssl->m_ssl, buffer, size);

    // Per-socket, was `static int retry;` -- see the member declaration for why that mattered.
    int &retry = m_sslReadRetry;

    // Check result will cleanup the connection in the case of a fatal
    checkResult(read, retry);

    if (retry) {
      return 0;
    }

    if (isFatal()) {
      return -1;
    }
  }
  // According to SSL spec, the number of bytes read must not be negative and
  // not have an error code from SSL_get_error(). If this happens, it is
  // itself an error. Let the parent handle the case
  return read;
}

int SecureSocket::secureWrite(const void *buffer, int size, int &wrote)
{
  std::scoped_lock ssl_lock{ssl_mutex_};

  if (m_ssl->m_ssl != nullptr) {
    LOG_DEBUG2("writing secure socket: %p", this);

    wrote = SSL_write(m_ssl->m_ssl, buffer, size);

    // Per-socket, was `static int retry;` -- see the member declaration for why that mattered.
    int &retry = m_sslWriteRetry;

    // Check result will cleanup the connection in the case of a fatal
    checkResult(wrote, retry);

    if (retry) {
      return 0;
    }

    if (isFatal()) {
      return -1;
    }
  }
  // According to SSL spec, r must not be negative and not have an error code
  // from SSL_get_error(). If this happens, it is itself an error. Let the
  // parent handle the case
  return wrote;
}

bool SecureSocket::isSecureReady() const
{
  return m_secureReady;
}

void SecureSocket::initSsl(bool server)
{
  std::scoped_lock ssl_lock{ssl_mutex_};

  m_ssl = std::make_unique<Ssl>();

  initContext(server);
}

bool SecureSocket::loadCertificate(const QString &filename)
{
  std::scoped_lock ssl_lock{ssl_mutex_};

  if (filename.isEmpty()) {
    SslLogger::logError("tls certificate is not specified");
    return false;
  }

  if (!QFile::exists(filename)) {
    std::string errorMsg("tls certificate doesn't exist: ");
    errorMsg.append(filename.toStdString());
    SslLogger::logError(errorMsg.c_str());
    return false;
  }

  const auto fName = filename.toStdString();

  if (SSL_CTX_use_certificate_file(m_ssl->m_context, fName.c_str(), SSL_FILETYPE_PEM) <= 0) {
    SslLogger::logError("could not use tls certificate");
    return false;
  }

  if (SSL_CTX_use_PrivateKey_file(m_ssl->m_context, fName.c_str(), SSL_FILETYPE_PEM) <= 0) {
    SslLogger::logError("could not use tls private key");
    return false;
  }

  if (!SSL_CTX_check_private_key(m_ssl->m_context)) {
    SslLogger::logError("could not verify tls private key");
    return false;
  }

  return true;
}

void SecureSocket::initContext(bool server)
{
  SSL_library_init();

  const SSL_METHOD *method;

  // load & register all cryptos, etc.
  OpenSSL_add_all_algorithms();

  // load all error messages
  SSL_load_error_strings();
  SslLogger::logSecureLibInfo();

  if (server) {
    method = SSLv23_server_method();
  } else {
    method = SSLv23_client_method();
  }

  // create new context from method
  const auto *m = const_cast<SSL_METHOD *>(method);
  m_ssl->m_context = SSL_CTX_new(m);

  // Prevent the usage of of all version prior to TLSv1.2 as they are known to
  // be vulnerable
  SSL_CTX_set_options(
      m_ssl->m_context,
      SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1 | SSL_OP_IGNORE_UNEXPECTED_EOF
  );

  if (m_ssl->m_context == nullptr) {
    SslLogger::logError();
  }

  if (m_securityLevel == SecurityLevel::PeerAuth) {
    // We want to ask for peer certificate, but not verify it. If we don't ask for peer
    // certificate, e.g. client won't send it.
    SSL_CTX_set_verify(m_ssl->m_context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
    SSL_CTX_set_cert_verify_callback(m_ssl->m_context, verifyIgnoreCertCallback, nullptr);
  }
}

void SecureSocket::createSSL()
{
  // I assume just one instance is needed
  // get new SSL state with context
  if (m_ssl->m_ssl == nullptr) {
    assert(m_ssl->m_context != nullptr);
    m_ssl->m_ssl = SSL_new(m_ssl->m_context);
  }
}

void SecureSocket::freeSSL()
{
  std::scoped_lock ssl_lock{ssl_mutex_};

  isFatal(true);
  // take socket from multiplexer ASAP otherwise the race condition
  // could cause events to get called on a dead object. TCPSocket
  // will do this, too, but the double-call is harmless
  setJob(nullptr);
  if (m_ssl) {
    if (m_ssl->m_ssl != nullptr) {
      SSL_set_quiet_shutdown(m_ssl->m_ssl, 1);
      SSL_shutdown(m_ssl->m_ssl);

      SSL_free(m_ssl->m_ssl);
      m_ssl->m_ssl = nullptr;
    }
    if (m_ssl->m_context != nullptr) {
      SSL_CTX_free(m_ssl->m_context);
      m_ssl->m_context = nullptr;
    }
    m_ssl = nullptr;
  }
}

int SecureSocket::secureAccept(int socket)
{
  std::scoped_lock ssl_lock{ssl_mutex_};

  createSSL();

  // set connection socket to SSL state
  SSL_set_fd(m_ssl->m_ssl, socket);

  LOG_DEBUG2("accepting secure socket");
  int r = SSL_accept(m_ssl->m_ssl);

  // Per-socket, was `static int retry;`. This one was the most dangerous of the four: the
  // `retry == 0` branch below decides the handshake is COMPLETE and sets m_secureReady.
  int &retry = m_sslAcceptRetry;

  checkResult(r, retry);

  if (isFatal()) {
    // SEC-08b (MouseTransfer fork, 2026-08-03): this used to `Arch::sleep(1)` HERE, on the shared
    // SocketMultiplexer service thread — the ONE thread servicing every socket in the process,
    // including live KVM connections. So a single failed handshake froze all input relay for a full
    // second, and ~10 back-to-back reached the flatline window and dropped clients: an
    // unauthenticated ~1 packet/second DoS against the whole mesh, needing no certificate at all
    // (upstream deskflow#8214, closed without the mechanism ever named; the sleep survived to
    // v1.26.0). The sleep was pointless as well as harmful — serviceAccept returns nullptr on a
    // fatal accept (status < 0), so the multiplexer TEARS THIS SOCKET DOWN; there is no same-socket
    // "hammering" to throttle, and every new attacker connection is a fresh socket accepted, failed
    // and closed in microseconds. Removed: a fatal handshake must never block the shared thread.
    LOG_ERR("failed to accept secure socket");
    LOG_WARN("client connection may not be secure");
    m_secureReady = false;
    retry = 0;
    return -1; // Failed, error out
  }

  // If not fatal and no retry, state is good
  if (retry == 0) {
    if (m_securityLevel == SecurityLevel::PeerAuth && !verifyCertFingerprint(Settings::tlsTrustedClientsDb())) {
      retry = 0;
      disconnect();
      return -1; // Fail
    }
    m_secureReady = true;
    LOG_INFO("accepted secure socket");
    SslLogger::logSecureCipherInfo(m_ssl->m_ssl);
    SslLogger::logSecureConnectInfo(m_ssl->m_ssl);
    return 1;
  }

  // If not fatal and retry is set, not ready, and return retry
  if (retry > 0) {
    LOG_DEBUG2("retry accepting secure socket");
    m_secureReady = false;
    Arch::sleep(s_retryDelay);
    return 0;
  }

  // no good state exists here
  LOG_ERR("unexpected state attempting to accept connection");
  return -1;
}

int SecureSocket::secureConnect(int socket)
{
  if (!loadCertificate(Settings::value(Settings::Security::Certificate).toString())) {
    LOG_ERR("could not load client certificates");
    disconnect();
    return -1;
  }

  std::scoped_lock ssl_lock{ssl_mutex_};

  createSSL();

  // attach the socket descriptor
  SSL_set_fd(m_ssl->m_ssl, socket);

  LOG_DEBUG2("connecting secure socket");

  // enable hostname verification.
  const auto name = Settings::value(Settings::Core::ComputerName).toString().toStdString();
  SSL_set1_host(m_ssl->m_ssl, name.c_str());
  int r = SSL_connect(m_ssl->m_ssl);

  // Per-socket, was `static int retry;`. As with secureAccept, the branches below treat this
  // counter as this connection's handshake state.
  int &retry = m_sslConnectRetry;

  checkResult(r, retry);

  if (isFatal()) {
    LOG_ERR("failed to connect secure socket");
    retry = 0;
    return -1;
  }

  // If we should retry, not ready and return 0
  if (retry > 0) {
    LOG_DEBUG2("retry connect secure socket");
    m_secureReady = false;
    Arch::sleep(s_retryDelay);
    return 0;
  }

  retry = 0;
  // No error, set ready, process and return ok
  m_secureReady = true;
  if (verifyCertFingerprint(Settings::tlsTrustedServersDb())) {
    LOG_INFO("connected to secure socket");
    if (!showCertificate()) {
      disconnect();
      return -1; // Cert fail, error
    }
  } else {
    LOG_ERR("failed to verify server certificate fingerprint");
    disconnect();
    return -1; // Fingerprint failed, error
  }
  LOG_DEBUG2("connected secure socket");
  SslLogger::logSecureCipherInfo(m_ssl->m_ssl);
  SslLogger::logSecureConnectInfo(m_ssl->m_ssl);
  return 1;
}

bool SecureSocket::showCertificate() const
{
  X509 *cert;
  char *line;

  // get the server's certificate
  cert = SSL_get_peer_certificate(m_ssl->m_ssl);
  if (cert != nullptr) {
    line = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);
    LOG_INFO("server tls certificate info: %s", line);
    OPENSSL_free(line);
    X509_free(cert);
  } else {
    SslLogger::logError("server has no tls certificate");
    return false;
  }

  return true;
}

void SecureSocket::checkResult(int status, int &retry)
{
  // ssl errors are a little quirky. the "want" errors are normal and
  // should result in a retry.
  switch (auto errorCode = SSL_get_error(m_ssl->m_ssl, status); errorCode) {
  case SSL_ERROR_NONE:
    retry = 0;
    // operation completed
    break;

  case SSL_ERROR_ZERO_RETURN:
    // connection closed
    isFatal(true);
    LOG_DEBUG("tls connection closed");
    break;

  case SSL_ERROR_WANT_READ:
    retry++;
    LOG_DEBUG2("want to read, error=%d, attempt=%d", errorCode, retry);
    break;

  case SSL_ERROR_WANT_WRITE:
    // Need to make sure the socket is known to be writable so the impending
    // select action actually triggers on a write. This isn't necessary for
    // m_readable because the socket logic is always readable
    setWritable(true);
    retry++;
    LOG_DEBUG2("want to write, error=%d, attempt=%d", errorCode, retry);
    break;

  case SSL_ERROR_WANT_CONNECT:
    retry++;
    LOG_DEBUG2("want to connect, error=%d, attempt=%d", errorCode, retry);
    break;

  case SSL_ERROR_WANT_ACCEPT:
    retry++;
    LOG_DEBUG2("want to accept, error=%d, attempt=%d", errorCode, retry);
    break;

  case SSL_ERROR_SYSCALL:
    LOG_ERR("tls error occurred (system call failure)");
    if (ERR_peek_error() == 0) {
      if (status == 0) {
        LOG_ERR("eof violates tls protocol");
      } else if (status == -1) {
        // underlying socket I/O reproted an error
        try {
          ARCH->throwErrorOnSocket(getSocket());
        } catch (ArchNetworkException &e) {
          LOG_ERR("%s", e.what());
        }
      }
    }

    isFatal(true);
    break;

  case SSL_ERROR_SSL:
    LOG_ERR("tls error occurred (generic failure)");
    isFatal(true);
    break;

  default:
    LOG_ERR("tls error occurred (unknown failure)");
    isFatal(true);
    break;
  }

  if (isFatal()) {
    retry = 0;
    SslLogger::logError();
    disconnect();
  }
}

void SecureSocket::disconnect()
{
  using enum EventTypes;
  sendEvent(SocketDisconnected);
  sendEvent(StreamInputShutdown);
}

bool SecureSocket::verifyCertFingerprint(const QString &FingerprintDatabasePath)
{
  const auto cert = SSL_get_peer_certificate(m_ssl->m_ssl);
  const auto sha256 = deskflow::sslCertFingerprint(cert, QCryptographicHash::Sha256);

  if (cert)
    X509_free(cert);

  if (!sha256.isValid())
    return false;

  // Remember it. This is the only moment the peer's certificate is in hand, and the clipboard lane
  // needs to answer "is this SECOND connection from the same peer as the main session?" long after
  // the certificate is gone. Stored raw-hex (not the colon-separated display form) because it is
  // compared, not shown.
  m_peerFingerprint = sha256.data.toHex().toStdString();

  // Gui Must Parse this line, DO NOT CHANGE
  LOG_IPC("peer fingerprint: %s", qPrintable(deskflow::formatSSLFingerprint(sha256.data, false)));

  QFile file(FingerprintDatabasePath);

  FingerprintDatabase db;
  db.read(FingerprintDatabasePath);
  const bool emptyDB = db.fingerprints().empty();

  const auto &path = FingerprintDatabasePath;
  if (file.exists() && emptyDB) {
    LOG_ERR("failed to open trusted fingerprints file: %s", qPrintable(path));
    return false;
  }

  if (!emptyDB) {
    LOG_DEBUG("read %d fingerprint(s) from file: %s", db.fingerprints().size(), qPrintable(path));
  }

  if (!db.isTrusted(sha256)) {
    LOG_WARN("fingerprint does not match trusted fingerprint");
    return false;
  }

  LOG_DEBUG("fingerprint matches trusted fingerprint");
  return true;
}

ISocketMultiplexerJob *SecureSocket::serviceConnect(ISocketMultiplexerJob *const, bool, bool, bool)
{
  Lock lock(&getMutex());

  int status = 0;
#ifdef SYSAPI_WIN32
  status = secureConnect(static_cast<int>(getSocket()->m_socket));
#elif SYSAPI_UNIX
  status = secureConnect(getSocket()->m_fd);
#endif

  // If status < 0, error happened
  if (status < 0) {
    return nullptr;
  }

  // If status > 0, success
  if (status > 0) {
    sendEvent(EventTypes::DataSocketSecureConnected);
    return newJob();
  }

  // Retry case
  return new TSocketMultiplexerMethodJob<SecureSocket>(
      this, &SecureSocket::serviceConnect, getSocket(), isReadable(), isWritable()
  );
}

ISocketMultiplexerJob *SecureSocket::serviceAccept(ISocketMultiplexerJob *const, bool, bool, bool)
{
  Lock lock(&getMutex());

  int status = 0;
#ifdef SYSAPI_WIN32
  status = secureAccept(static_cast<int>(getSocket()->m_socket));
#elif SYSAPI_UNIX
  status = secureAccept(getSocket()->m_fd);
#endif
  // If status < 0, error happened
  if (status < 0) {
    sendEvent(EventTypes::ClientListenerDisconnectedOnAccept);
    return nullptr;
  }

  // If status > 0, success
  if (status > 0) {
    sendEvent(EventTypes::ClientListenerAccepted);
    return newJob();
  }

  // Retry case
  return new TSocketMultiplexerMethodJob<SecureSocket>(
      this, &SecureSocket::serviceAccept, getSocket(), isReadable(), isWritable()
  );
}

void SecureSocket::handleTCPConnected(const Event &)
{
  if (getSocket() == nullptr) {
    LOG_DEBUG("disregarding stale connect event");
    return;
  }
  secureConnect();
}
