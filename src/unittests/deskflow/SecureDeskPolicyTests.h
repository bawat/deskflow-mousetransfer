/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 MouseTransfer fork
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class SecureDeskPolicyTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void ctrlAltDelFromDefaultReturns();
  void unlockBackToDefaultDoesNotReturn();
  void inaccessibleDesktopReturns();
  void initialDiscoveryNeverReturns();
  void sameDesktopNeverReturns();
  void alreadyOnLockScreenCrossingIsNotATransition();
  void routeOnlyWhenBothKnownAndDifferent();
  void reporterFirstFailureThenRateLimited();
  void reporterRecoveryOnlyAfterReportedStreak();
  void reporterRateLimitIsGlobalAcrossStreaks();
};
