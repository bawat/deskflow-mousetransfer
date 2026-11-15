/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 MouseTransfer fork
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class WarpGrabTrackerTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void firstMoveIsStockAndSeeds();
  void noTriggerWithoutToken();
  void noTriggerBelowMinDisplacement();
  void ownNudgeImmunity();
  void clampedTargetIsNotAWarp();
  void immediateEntryWithZeroSnapBack();
  void deltaFidelityAcrossCoalescedMoves();
  void appWarpsAreHonouredInActive();
  void recentrePreAckMovesStayOnOldChain();
  void recentrePostAckStragglersThenNewChain();
  void recentreNewChainFirstAfterAck();
  void endOnTokenChange();
  void endOnTimeout();
  void endDeferredWhileRecentreSettles();
  void endWithoutResyncWhenAlreadyAligned();
  void staleLandedReadDoesNotEatADelta();
  void resetOnLeaveEnterOrShapeChange();
  void staleAckAfterResetIsIgnored();
};
