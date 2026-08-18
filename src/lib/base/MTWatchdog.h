/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 *
 * ============================================================================
 * TEMPORARY MouseTransfer DIAGNOSTIC -- main-thread stall watchdog.
 * ============================================================================
 * Added to pin a ~12-16s freeze of the SERVER core's main thread that occurs
 * the instant the cursor crosses to the RDP viewer at a window-handoff COMMIT
 * (coreout.log goes silent; it recovers when the viewer window is Alt+F4'd).
 * Server-level MT-swtrace (switchScreen phases) did NOT fire, so the block is
 * UPSTREAM of Server::switchScreen -- in the hook / clipboard / injection path.
 *
 * This header is a self-contained, header-only (C++17 inline vars) heartbeat +
 * background watchdog. It changes NO behaviour; it only emits LOG_NOTE lines.
 * REMOVE after diagnosis.
 *
 * IMPORTANT threading note (verified in this fork): the Windows core runs the
 * low-level input hooks (mouseLLHook/keyboardLLHook) and their message pump on
 * a SEPARATE "desk thread" (MSWindowsDesks::deskThread), while the platform
 * message pump (MSWindowsEventQueueBuffer::getEvent), handleFixes and the
 * Server event dispatch (onMouseMovePrimary/Secondary) run on the MAIN
 * event-queue thread. Merging both into one heartbeat would let the desk
 * thread's beats MASK a main-thread stall -- so there are TWO heartbeats:
 *   - MT_BEAT(...)     -> g_mtHeartbeat  (MAIN thread; this is what the
 *                        watchdog trips on -- handleFixes' 1 Hz timer keeps it
 *                        beating at >= 1/s when healthy, so a >2000ms gap is a
 *                        genuine stall, never idle)
 *   - MT_HOOKBEAT(...) -> g_mtHookBeat   (DESK/hook thread; recorded for
 *                        context and printed when the main thread stalls, but
 *                        NOT itself a trip source -- its pump legitimately
 *                        blocks in GetMessage while idle)
 */
#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "base/Log.h"

namespace mtwd {

// MAIN-thread heartbeat (nanoseconds since steady_clock epoch) + last label.
// Labels are ALWAYS static string literals (never a temporary) so the atomic
// const char* stays valid for the watchdog thread to read at any time.
inline std::atomic<long long> g_mtHeartbeat{0};
inline std::atomic<const char *> g_mtLabel{"(init)"};

// DESK/hook-thread heartbeat + last label (separate var so it can't mask a
// main-thread stall; see the threading note above).
inline std::atomic<long long> g_mtHookBeat{0};
inline std::atomic<const char *> g_mtHookLabel{"(init)"};

inline long long mtNowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// MAIN-thread checkpoint.
inline void mtBeat(const char *label)
{
  g_mtLabel.store(label, std::memory_order_relaxed);
  g_mtHeartbeat.store(mtNowNs(), std::memory_order_relaxed);
}

// DESK/hook-thread checkpoint.
inline void mtHookBeat(const char *label)
{
  g_mtHookLabel.store(label, std::memory_order_relaxed);
  g_mtHookBeat.store(mtNowNs(), std::memory_order_relaxed);
}

// Start the background watchdog exactly once (idempotent across calls/threads).
// It samples every 250ms and, if the MAIN heartbeat has not advanced for
// >2000ms, LOG_NOTEs ONCE per stall episode (and once more when it resumes).
inline void mtStartWatchdog()
{
  static std::once_flag once;
  std::call_once(once, [] {
    mtBeat("(watchdog-start)");
    std::thread([] {
      bool stalled = false;
      long long stalledSinceNs = 0;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const long long now = mtNowNs();
        const long long last = g_mtHeartbeat.load(std::memory_order_relaxed);
        if (last == 0) {
          continue; // not armed yet
        }
        const long long gapMs = (now - last) / 1000000LL;
        if (!stalled && gapMs > 2000) {
          stalled = true;
          stalledSinceNs = last;
          const long long hookAgoMs = (now - g_mtHookBeat.load(std::memory_order_relaxed)) / 1000000LL;
          LOG_NOTE(
              "MT-watchdog: MAIN THREAD STALLED %lldms at '%s' (desk/hook last '%s' %lldms ago)", gapMs,
              g_mtLabel.load(std::memory_order_relaxed), g_mtHookLabel.load(std::memory_order_relaxed), hookAgoMs
          );
        } else if (stalled && gapMs <= 2000) {
          stalled = false;
          const long long totalMs = (last - stalledSinceNs) / 1000000LL;
          LOG_NOTE(
              "MT-watchdog: main thread resumed after %lldms (was at '%s')", totalMs,
              g_mtLabel.load(std::memory_order_relaxed)
          );
        }
      }
    }).detach();
  });
}

} // namespace mtwd

// Short, distinct static-literal labels at each frequently-passed site so the
// last one recorded before a stall pins WHERE the main thread went dark.
#define MT_BEAT(label) ::mtwd::mtBeat(label)
#define MT_HOOKBEAT(label) ::mtwd::mtHookBeat(label)
