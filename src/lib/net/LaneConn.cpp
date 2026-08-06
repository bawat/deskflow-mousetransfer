/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 MouseTransfer fork
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "net/LaneConn.h"

#include "arch/Arch.h"
#include "base/Log.h"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#if SYSAPI_WIN32
#include <winsock2.h>
// windows.h must follow winsock2.h, never precede it (it drags in the winsock 1.1 headers).
#include <windows.h>
#elif SYSAPI_UNIX
#include <cerrno>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <cstdio>

namespace deskflow {

namespace lane {

void demoteCurrentThreadToBackground()
{
#if SYSAPI_WIN32
  // THREAD_MODE_BACKGROUND_BEGIN (winbase.h) lowers scheduling priority AND memory/IO priority for
  // the calling thread until it ends or calls ..._END. It is the only knob that helps here: the
  // core deliberately runs in the REALTIME priority class, and thread priorities are RELATIVE to
  // the class, so even THREAD_PRIORITY_IDLE inside a realtime process still outranks ordinary
  // work. Background mode moves the thread out of that régime instead of within it.
  //
  // Documented to fail with ERROR_THREAD_MODE_ALREADY_BACKGROUND if applied twice; we apply it once
  // per thread at its start, and a failure is not worth acting on either way.
  if (!SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN)) {
    LOG_DEBUG1("clipboard lane: could not enter background thread mode (%lu)", GetLastError());
  }
#elif defined(__linux__)
  // On Linux the "process" a nice value applies to is the TASK, i.e. the calling thread, so
  // PRIO_PROCESS with who == 0 is a per-thread nice. (This is a documented Linux deviation from
  // POSIX, which specifies process-wide nice -- hence the __linux__ guard rather than a generic
  // SYSAPI_UNIX one: applying it on a platform with POSIX semantics would slow the whole core.)
  //
  // 10 rather than the maximum 19: enough to yield to everything that matters without inviting
  // priority inversion against a peer that is waiting on our TCP window.
  errno = 0;
  if (setpriority(PRIO_PROCESS, 0, 10) != 0 && errno != 0) {
    LOG_DEBUG1("clipboard lane: could not lower thread priority (%d)", errno);
  }
#else
  // macOS/BSD: nice() really is process-wide here, so doing it would be worse than doing nothing.
  // The lane stays at normal priority; it is polite by being small and infrequent instead.
#endif
}

bool secureRandomBytes(void *out, size_t count)
{
  // OpenSSL's CSPRNG -- the only source of randomness this code base already links, and the same
  // one the TLS handshakes use. RAND_bytes returns 1 only when the bytes really are strong; every
  // other value must be treated as a refusal, never as "close enough".
  return RAND_bytes(static_cast<unsigned char *>(out), static_cast<int>(count)) == 1;
}

bool secretsEqual(const std::string &a, const std::string &b)
{
  if (a.size() != b.size()) {
    return false;
  }
  if (a.empty()) {
    // Two empty secrets are not a match -- an unarmed session must never validate.
    return false;
  }
  return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace lane

namespace {

//! The raw OS socket behind an ArchSocket, in whatever type select() wants
#if SYSAPI_WIN32
using RawSocket = SOCKET;
inline RawSocket rawOf(ArchSocket s)
{
  return s->m_socket;
}
#elif SYSAPI_UNIX
using RawSocket = int;
inline RawSocket rawOf(ArchSocket s)
{
  return s->m_fd;
}
#endif

} // namespace

LaneConn::LaneConn(ArchSocket socket, void *ssl, void *sslContext)
    : m_socket(socket),
      m_ssl(ssl),
      m_sslContext(sslContext)
{
  // do nothing
}

LaneConn::~LaneConn()
{
  if (m_ssl != nullptr) {
    auto *ssl = static_cast<SSL *>(m_ssl);
    // Quiet shutdown: send close_notify without waiting for the peer's, which is what the rest of
    // this code base does and is all a clipboard connection warrants.
    SSL_set_quiet_shutdown(ssl, 1);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    m_ssl = nullptr;
  }
  if (m_sslContext != nullptr) {
    SSL_CTX_free(static_cast<SSL_CTX *>(m_sslContext));
    m_sslContext = nullptr;
  }
  if (m_socket != nullptr) {
    try {
      ARCH->closeSocket(m_socket);
    } catch (...) {
      // Nothing useful to do at this point, and this runs on a worker's way out.
    }
    m_socket = nullptr;
  }
}

std::unique_ptr<LaneConn> LaneConn::adopt(ArchSocket socket, void *ssl, void *sslContext)
{
  if (socket == nullptr || ssl == nullptr) {
    // Ownership transferred the moment we were called, so refusing still means cleaning up. The
    // caller cannot do it: it has no OpenSSL headers, which is the whole reason these are void*.
    if (ssl != nullptr) {
      SSL_free(static_cast<SSL *>(ssl));
    }
    if (sslContext != nullptr) {
      SSL_CTX_free(static_cast<SSL_CTX *>(sslContext));
    }
    if (socket != nullptr) {
      try {
        ARCH->closeSocket(socket);
      } catch (...) {
        // Nothing useful to do, and this is already the "cannot happen" path.
      }
    }
    return nullptr;
  }

#if SYSAPI_WIN32
  // The SocketMultiplexer drove this socket with WSAEventSelect, which leaves an event association
  // on it (and forced it non-blocking, which is what we want). Clear the association now that the
  // multiplexer is gone, so the handle is a plain non-blocking socket that select() owns outright
  // and no stale WSAEVENT is bound to it.
  WSAEventSelect(rawOf(socket), nullptr, 0);
#endif

  return std::unique_ptr<LaneConn>(new LaneConn(socket, ssl, sslContext));
}

unsigned LaneConn::waitReady(bool forRead, bool forWrite, double seconds)
{
  if (m_socket == nullptr || m_dead || (!forRead && !forWrite)) {
    return Ready::None;
  }

  const RawSocket raw = rawOf(m_socket);

  fd_set readSet;
  fd_set writeSet;
  FD_ZERO(&readSet);
  FD_ZERO(&writeSet);
  if (forRead) {
    FD_SET(raw, &readSet);
  }
  if (forWrite) {
    FD_SET(raw, &writeSet);
  }

  timeval timeout;
  timeout.tv_sec = static_cast<long>(seconds);
  timeout.tv_usec = static_cast<long>((seconds - static_cast<double>(timeout.tv_sec)) * 1.0e6);

  // First argument: ignored on Windows, "highest fd + 1" on POSIX.
#if SYSAPI_WIN32
  const int nfds = 0;
#else
  const int nfds = static_cast<int>(raw) + 1;
#endif

  const int n = ::select(nfds, forRead ? &readSet : nullptr, forWrite ? &writeSet : nullptr, nullptr, &timeout);
  if (n <= 0) {
    // 0 is a timeout. A negative result is either EINTR (retry next loop) or a broken fd, and the
    // subsequent read/write is what will tell the difference and mark the connection dead -- there
    // is nothing this function could usefully decide on its own.
    return Ready::None;
  }

  unsigned ready = Ready::None;
  if (forRead && FD_ISSET(raw, &readSet)) {
    ready |= Ready::Read;
  }
  if (forWrite && FD_ISSET(raw, &writeSet)) {
    ready |= Ready::Write;
  }
  return ready;
}

int LaneConn::classify(int result, const char *what)
{
  auto *ssl = static_cast<SSL *>(m_ssl);
  const int error = SSL_get_error(ssl, result);
  switch (error) {
  case SSL_ERROR_WANT_READ:
  case SSL_ERROR_WANT_WRITE:
    // Not an error: the caller retries, and (for a write) must present the same buffer again.
    // A read wanting WRITE and a write wanting READ are both normal on TLS 1.3 (key updates,
    // post-handshake tickets); both are handled by simply going round the select loop again, which
    // is why they are not distinguished here.
    return 0;

  case SSL_ERROR_ZERO_RETURN:
    // Clean close_notify from the peer.
    LOG_DEBUG("clipboard lane %s: peer closed the connection", describe().c_str());
    m_dead = true;
    return -1;

  default:
    LOG_DEBUG("clipboard lane %s: %s failed (tls error %d)", describe().c_str(), what, error);
    // Drain the error queue so a failure here cannot be mistaken for a failure elsewhere later on
    // -- OpenSSL's error queue is per-thread and sticky.
    ERR_clear_error();
    m_dead = true;
    return -1;
  }
}

int LaneConn::readSome(void *buffer, uint32_t n)
{
  if (m_dead || m_ssl == nullptr || n == 0) {
    return m_dead ? -1 : 0;
  }

  ERR_clear_error();
  const int got = SSL_read(static_cast<SSL *>(m_ssl), buffer, static_cast<int>(n));
  if (got > 0) {
    return got;
  }
  return classify(got, "read");
}

int LaneConn::writeSome(const void *buffer, uint32_t n)
{
  if (m_dead || m_ssl == nullptr || n == 0) {
    return m_dead ? -1 : 0;
  }

  ERR_clear_error();
  const int put = SSL_write(static_cast<SSL *>(m_ssl), buffer, static_cast<int>(n));
  if (put > 0) {
    return put;
  }
  return classify(put, "write");
}

void LaneConn::shutdown()
{
  // Deliberately touches only the OS socket, never the SSL object: this is the one method other
  // threads may call, and SSL is single-thread-owned. A half-close is enough to make the peer see
  // end-of-stream and to make the owner's select() return at once.
  if (m_socket == nullptr) {
    return;
  }
  m_dead = true;
#if SYSAPI_WIN32
  ::shutdown(rawOf(m_socket), SD_BOTH);
#elif SYSAPI_UNIX
  ::shutdown(rawOf(m_socket), SHUT_RDWR);
#endif
}

std::string LaneConn::describe() const
{
  if (m_socket == nullptr) {
    return "[closed]";
  }
  char text[32];
  // The socket number only -- a lane log line must never carry a token, and there is nothing else
  // here worth printing.
  snprintf(text, sizeof(text), "[sock %lld]", static_cast<long long>(rawOf(m_socket)));
  return text;
}

} // namespace deskflow
