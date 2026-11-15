/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 MouseTransfer fork
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

// MouseTransfer fork: the pure decisions behind "the server's input desktop switched" handling, kept
// free of Win32 so they are unit-tested (src/unittests/deskflow/SecureDeskPolicyTests.cpp). The
// Windows layer (MSWindowsDesks / MSWindowsScreen) supplies desktop names and a clock.
//
// Background (2026-11-15, FANTASY): Ctrl+Alt+Del on the SERVER while the cursor was on a peer left
// relayed motion dead. The core's MAIN thread -- where MSWindowsScreen warps the cursor to its park
// and reads it back as the delta anchor -- is attached for life to the desktop the process was
// spawned on (it owns windows, so SetThreadDesktop refuses to move it). SetCursorPos/GetCursorPos
// only work on the INPUT desktop, so once the input desktop is the secure one the park never landed
// and the anchor fell back to the park point while the low-level hook stamped every motion at the
// real cursor: every delta was ~-700 px and the mis-stamp gate dropped all of them.

#include <cstdint>
#include <string_view>

namespace deskflow::securedesk {

//! Name of the interactive user desktop in the interactive window station ("winsta0\Default", the
//! documented STARTUPINFO.lpDesktop value for the user's desktop). Every other desktop a session's
//! input can switch to -- "Winlogon" (Ctrl+Alt+Del, lock, the UAC consent prompt), "Disconnect", or one
//! this process cannot open at all (empty name) -- is a system/secure desktop for our purposes.
inline constexpr std::wstring_view kInteractiveDesktop = L"Default";

//! Should a switch of the input desktop from \p from to \p to return the shared cursor to the server?
/*!
Owner decision: when the server's input desktop switches AWAY to a secure desktop while the cursor is
on a peer, bring mouse and keyboard home so the user can use the screen that just appeared. It is a
TRANSITION rule, never a state rule: a user who is already on the server's lock/logon screen must
still be able to cross to a peer deliberately, and that crossing changes no desktop, so it can never
fire this. \p initialDiscovery is the first desktop seen after start-up (no transition happened).
Whether the cursor is actually away is the server's call (it ignores the request when it is home).
*/
inline bool isReturnTrigger(std::wstring_view from, std::wstring_view to, bool initialDiscovery)
{
  return !initialDiscovery && to != from && to != kInteractiveDesktop;
}

//! Must cursor reads/warps for a thread on \p callerDesk go through a thread attached to \p inputDesk?
/*!
SetCursorPos/GetCursorPos require the calling thread's desktop to BE the input desktop. When either
name is unknown (empty) we cannot tell, so the caller keeps its own thread and relies on the
failure fallback instead.
*/
inline bool mustRouteViaInputDeskThread(std::wstring_view callerDesk, std::wstring_view inputDesk)
{
  return !callerDesk.empty() && !inputDesk.empty() && callerDesk != inputDesk;
}

//! Rate-limited reporter for a STREAK of failures (park warp not landing, cursor unreadable).
/*!
At most one failure report per interval, GLOBALLY (not per streak): a single missed park under load
is the known, harmless "warp lagging" case of the seam cross-bounce fix and happens at crossings, so
a per-streak report would print a line per crossing. A PERSISTENT failure is still reported on its
first event if nothing was reported for an interval, and then once per interval with the running
count. The end of a streak is reported only if that streak was. Pure: the caller passes a monotonic
millisecond clock and does the logging.
*/
class StreakReporter
{
public:
  enum class Action
  {
    None,
    ReportFailure,  //!< log "failing" with streak()
    ReportRecovered //!< log "recovered after <lastStreak> failures"
  };

  explicit StreakReporter(int64_t intervalMs) : m_intervalMs(intervalMs)
  {
  }

  Action onFailure(int64_t nowMs)
  {
    ++m_streak;
    if (!m_everReported || nowMs - m_lastReportMs >= m_intervalMs) {
      m_everReported = true;
      m_streakReported = true;
      m_lastReportMs = nowMs;
      return Action::ReportFailure;
    }
    return Action::None;
  }

  //! \p lastStreak receives the length of the streak that just ended (0 if none).
  Action onSuccess(uint64_t &lastStreak)
  {
    lastStreak = m_streak;
    const bool report = m_streakReported && m_streak != 0;
    m_streak = 0;
    m_streakReported = false;
    return report ? Action::ReportRecovered : Action::None;
  }

  uint64_t streak() const
  {
    return m_streak;
  }

private:
  int64_t m_intervalMs;
  uint64_t m_streak = 0;
  bool m_streakReported = false;
  bool m_everReported = false;
  int64_t m_lastReportMs = 0;
};

} // namespace deskflow::securedesk
