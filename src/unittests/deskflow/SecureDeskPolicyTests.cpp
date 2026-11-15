/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 MouseTransfer fork
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

// Unit tests for deskflow::securedesk (MouseTransfer fork): the return-to-server trigger on an input
// desktop switch, the "route cursor calls via the input-desk thread" rule, and the rate-limited
// failure-streak reporter. See SecureDeskPolicy.h.

#include "SecureDeskPolicyTests.h"

#include "deskflow/SecureDeskPolicy.h"

using namespace deskflow::securedesk;

namespace {
constexpr int64_t kInterval = 30000;
const std::wstring_view kWinlogon = L"Winlogon";
} // namespace

void SecureDeskPolicyTests::ctrlAltDelFromDefaultReturns()
{
  QVERIFY(isReturnTrigger(kInteractiveDesktop, kWinlogon, false)); // C-A-D, Win+L, UAC consent
  QVERIFY(isReturnTrigger(kInteractiveDesktop, L"Disconnect", false));
}

void SecureDeskPolicyTests::unlockBackToDefaultDoesNotReturn()
{
  QVERIFY(!isReturnTrigger(kWinlogon, kInteractiveDesktop, false));
}

void SecureDeskPolicyTests::inaccessibleDesktopReturns()
{
  // A core that cannot open the new input desktop sees an empty name: still a switch away from the
  // user's desktop, and relayed motion cannot work there either.
  QVERIFY(isReturnTrigger(kInteractiveDesktop, L"", false));
}

void SecureDeskPolicyTests::initialDiscoveryNeverReturns()
{
  // A core started while the machine sits at the lock screen discovers "Winlogon" first: no
  // transition happened, nothing to return from.
  QVERIFY(!isReturnTrigger(L"", kWinlogon, true));
}

void SecureDeskPolicyTests::sameDesktopNeverReturns()
{
  QVERIFY(!isReturnTrigger(kWinlogon, kWinlogon, false));
  QVERIFY(!isReturnTrigger(kInteractiveDesktop, kInteractiveDesktop, false));
}

void SecureDeskPolicyTests::alreadyOnLockScreenCrossingIsNotATransition()
{
  // Already on the server's lock screen, crossing to a peer: the input desktop stays Winlogon, so the
  // only "switch" the check could ever see is Winlogon -> Winlogon, which never fires. This is what
  // keeps a deliberate crossing from the lock screen possible.
  QVERIFY(!isReturnTrigger(kWinlogon, kWinlogon, false));
}

void SecureDeskPolicyTests::routeOnlyWhenBothKnownAndDifferent()
{
  QVERIFY(mustRouteViaInputDeskThread(kInteractiveDesktop, kWinlogon));
  QVERIFY(mustRouteViaInputDeskThread(kWinlogon, kInteractiveDesktop)); // the stranded-pin case
  QVERIFY(!mustRouteViaInputDeskThread(kInteractiveDesktop, kInteractiveDesktop));
  QVERIFY(!mustRouteViaInputDeskThread(L"", kWinlogon));
  QVERIFY(!mustRouteViaInputDeskThread(kInteractiveDesktop, L""));
}

void SecureDeskPolicyTests::reporterFirstFailureThenRateLimited()
{
  StreakReporter r(kInterval);
  QCOMPARE(r.onFailure(1000), StreakReporter::Action::ReportFailure);
  for (int64_t t = 1001; t < 1000 + kInterval; t += 997) {
    QCOMPARE(r.onFailure(t), StreakReporter::Action::None);
  }
  QCOMPARE(r.onFailure(1000 + kInterval), StreakReporter::Action::ReportFailure);
  QVERIFY(r.streak() > 2);
}

void SecureDeskPolicyTests::reporterRecoveryOnlyAfterReportedStreak()
{
  StreakReporter r(kInterval);
  uint64_t last = 99;
  QCOMPARE(r.onSuccess(last), StreakReporter::Action::None); // nothing failed
  QCOMPARE(last, uint64_t{0});
  r.onFailure(5);
  r.onFailure(6);
  QCOMPARE(r.onSuccess(last), StreakReporter::Action::ReportRecovered);
  QCOMPARE(last, uint64_t{2});
  QCOMPARE(r.streak(), uint64_t{0});
}

void SecureDeskPolicyTests::reporterRateLimitIsGlobalAcrossStreaks()
{
  // One missed park per crossing (the harmless lagging-warp case) must not print a line per crossing.
  StreakReporter r(kInterval);
  uint64_t last = 0;
  QCOMPARE(r.onFailure(0), StreakReporter::Action::ReportFailure);
  QCOMPARE(r.onSuccess(last), StreakReporter::Action::ReportRecovered);
  QCOMPARE(r.onFailure(100), StreakReporter::Action::None); // a new streak inside the interval
  QCOMPARE(r.onSuccess(last), StreakReporter::Action::None); // and its end is silent too
  QCOMPARE(r.onFailure(kInterval), StreakReporter::Action::ReportFailure);
}

QTEST_MAIN(SecureDeskPolicyTests)
