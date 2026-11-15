/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 MouseTransfer fork
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

// Unit tests for deskflow::WarpGrabTracker (MouseTransfer fork) -- the client-side state machine
// that stops an application's own cursor warps (Blender continuous grab, value scrubbing) being
// snapped back by the server's absolute moves. See WarpGrabTracker.h for the model.
//
// The simulation below plays the OS: `cursor` is the true cursor, every injection lands there
// synchronously (unless a test says otherwise), and an "application warp" is a direct write to it.

#include "WarpGrabTrackerTests.h"

#include "deskflow/WarpGrabTracker.h"

using deskflow::WarpGrabTracker;
using Point = WarpGrabTracker::Point;
using Mode = WarpGrabTracker::Mode;
using ResyncKind = WarpGrabTracker::ResyncKind;
using EndReason = WarpGrabTracker::EndReason;

namespace {

// Test values standing in for what the Windows layer derives: 2x the default 4 px drag rectangle,
// the 1 px normalisation round-trip, the 1 s session timeout.
constexpr int32_t kTestEntryMin = 8;
constexpr int32_t kTestRounding = 1;
constexpr int64_t kTestTimeoutMs = 1000;
constexpr WarpGrabTracker::Token kGrab = 42;

WarpGrabTracker::Config testConfig()
{
  WarpGrabTracker::Config c;
  c.entryMinDx = kTestEntryMin;
  c.entryMinDy = kTestEntryMin;
  c.roundingPx = kTestRounding;
  c.warpTimeoutMs = kTestTimeoutMs;
  return c;
}

WarpGrabTracker::Token grabToken()
{
  return kGrab;
}

struct Sim
{
  WarpGrabTracker tr{testConfig()};
  WarpGrabTracker::Shape shape{0, 0, 1920, 1080};
  Point cursor{0, 0};
  WarpGrabTracker::Token token = WarpGrabTracker::kNoToken;
  int tokenCalls = 0;
  int64_t now = 0;

  WarpGrabTracker::Decision move(int32_t x, int32_t y, int64_t advanceMs = 8)
  {
    now += advanceMs;
    auto provider = [this] {
      ++tokenCalls;
      return token;
    };
    auto d = tr.onServerMove({x, y}, cursor, provider, now, shape);
    cursor = d.inject; // the injection lands...
    tr.noteLanded(cursor); // ...and the read-back sees it
    return d;
  }

  //! Seed at (x,y), have the app warp the cursor right by warpDx, then enter with token `tok` on a
  //! +1 px server move.
  bool enterSession(int32_t x, int32_t y, int32_t warpDx, WarpGrabTracker::Token tok)
  {
    move(x, y);
    cursor.x += warpDx;
    token = tok;
    return move(x + 1, y).entered;
  }

  bool takeResync(Point &p, uint32_t &id)
  {
    return tr.takeResyncRequest(p, id);
  }
};

} // namespace

#define COMPARE_POINT(actual, ex, ey)                                                                                  \
  do {                                                                                                                 \
    QCOMPARE((actual).x, (ex));                                                                                        \
    QCOMPARE((actual).y, (ey));                                                                                        \
  } while (0)

void WarpGrabTrackerTests::firstMoveIsStockAndSeeds()
{
  Sim s;
  s.cursor = {5, 5}; // wherever the cursor happened to be; irrelevant to the first move
  s.token = kGrab;
  auto d = s.move(500, 500);
  COMPARE_POINT(d.inject, 500, 500);
  QVERIFY(!d.tracked);
  QCOMPARE(s.tokenCalls, 0);
  QCOMPARE(s.tr.mode(), Mode::Idle);
}

void WarpGrabTrackerTests::noTriggerWithoutToken()
{
  Sim s;
  s.move(500, 500);
  s.cursor = {900, 500}; // something warped it...
  auto d = s.move(510, 500);
  // ...but nobody holds a grab, so stock behaviour: the server position wins.
  COMPARE_POINT(d.inject, 510, 500);
  QVERIFY(!d.tracked);
  QCOMPARE(s.tokenCalls, 1);
  QCOMPARE(s.tr.mode(), Mode::Idle);
  Point p;
  uint32_t id = 0;
  QVERIFY(!s.takeResync(p, id));
}

void WarpGrabTrackerTests::noTriggerBelowMinDisplacement()
{
  Sim s;
  s.token = kGrab;
  s.move(500, 500);
  s.cursor = {500 + kTestEntryMin, 500 - kTestEntryMin}; // exactly AT the threshold: not beyond it
  auto d = s.move(510, 500);
  COMPARE_POINT(d.inject, 510, 500);
  QCOMPARE(s.tokenCalls, 0); // the token is not even evaluated
  QCOMPARE(s.tr.mode(), Mode::Idle);
}

void WarpGrabTrackerTests::ownNudgeImmunity()
{
  // crossdrag_helper injects 1-2 px relative nudges of its own; with a grab held (token != 0) they
  // must still never start a session.
  Sim s;
  s.token = kGrab;
  s.move(500, 500);
  for (int32_t i = 1; i <= 20; ++i) {
    s.cursor.x += (i % 2) ? 2 : -2;
    s.cursor.y += 1;
    auto d = s.move(500 + i, 500);
    COMPARE_POINT(d.inject, 500 + i, 500);
    QCOMPARE(s.tr.mode(), Mode::Idle);
  }
  QCOMPARE(s.tokenCalls, 0);
}

void WarpGrabTrackerTests::clampedTargetIsNotAWarp()
{
  // The OS refuses part of a request (monitor gap / edge): landed != target, and the next read
  // matches LANDED. That is not someone else moving the cursor.
  WarpGrabTracker tr{testConfig()};
  WarpGrabTracker::Shape shape{0, 0, 1920, 1080};
  int calls = 0;
  auto provider = [&calls] {
    ++calls;
    return kGrab;
  };
  auto d = tr.onServerMove({500, 500}, {0, 0}, provider, 0, shape);
  tr.noteLanded({500, 470}); // clamped 30 px short
  d = tr.onServerMove({505, 500}, {500, 470}, provider, 8, shape);
  COMPARE_POINT(d.inject, 505, 500);
  QCOMPARE(tr.mode(), Mode::Idle);
  QCOMPARE(calls, 0);
}

void WarpGrabTrackerTests::immediateEntryWithZeroSnapBack()
{
  Sim s;
  s.move(500, 500);
  s.cursor = {900, 300}; // the app warped (e.g. Blender wrapped the cursor)
  s.token = kGrab;
  auto d = s.move(510, 504);
  QVERIFY(d.entered);
  QVERIFY(d.tracked);
  QCOMPARE(s.tr.mode(), Mode::Active);
  // The server position (510,504) is NEVER injected: the user's delta (+10,+4) lands on the warp.
  COMPARE_POINT(d.inject, 910, 304);
  COMPARE_POINT(s.cursor, 910, 304);
}

void WarpGrabTrackerTests::deltaFidelityAcrossCoalescedMoves()
{
  Sim s;
  QVERIFY(s.enterSession(960, 540, 200, kGrab));
  COMPARE_POINT(s.cursor, 1161, 540); // 960 + 200 warp + 1 entry delta
  // ServerProxy compression coalesces several moves into one; the diff of absolutes still carries
  // their full sum.
  auto d = s.move(961 + 37, 540 - 5);
  COMPARE_POINT(d.delta, 37, -5);
  COMPARE_POINT(d.inject, 1161 + 37, 540 - 5);
  d = s.move(961 + 37 - 12, 540 - 5 + 3);
  COMPARE_POINT(d.inject, 1161 + 37 - 12, 540 - 5 + 3);
  QCOMPARE(s.tr.mode(), Mode::Active);
}

void WarpGrabTrackerTests::appWarpsAreHonouredInActive()
{
  Sim s;
  QVERIFY(s.enterSession(960, 540, 200, kGrab));
  // Re-centre-every-frame app: it warps back to (960,540) after every move, however small.
  for (int32_t i = 1; i <= 10; ++i) {
    s.cursor = {960, 540};
    auto d = s.move(961 + i * 3, 540);
    COMPARE_POINT(d.inject, 963, 540); // exactly the +3 the user moved, never accumulated drift
    QVERIFY(d.warpObserved);
  }
  QCOMPARE(s.tr.mode(), Mode::Active);
}

void WarpGrabTrackerTests::recentrePreAckMovesStayOnOldChain()
{
  Sim s;
  QVERIFY(s.enterSession(960, 540, 200, kGrab));
  // Server cursor drifts left to within a quarter screen of the edge (1920/4 = 480).
  auto d = s.move(479, 540);
  QCOMPARE(d.resync, ResyncKind::Recentre);
  Point r;
  uint32_t id = 0;
  QVERIFY(s.takeResync(r, id));
  COMPARE_POINT(r, 960, 540);
  QVERIFY(s.tr.resyncAwaitingAck());
  // Not acked yet: the server is still on the old chain.
  Point before = s.cursor;
  d = s.move(474, 540);
  COMPARE_POINT(d.delta, -5, 0);
  COMPARE_POINT(d.inject, before.x - 5, before.y);
  // No second request while one is outstanding.
  d = s.move(470, 540);
  QCOMPARE(d.resync, ResyncKind::None);
  Point p;
  uint32_t id2 = 0;
  QVERIFY(!s.takeResync(p, id2));
}

void WarpGrabTrackerTests::recentrePostAckStragglersThenNewChain()
{
  Sim s;
  QVERIFY(s.enterSession(960, 540, 200, kGrab));
  s.move(479, 540);
  Point r;
  uint32_t id = 0;
  QVERIFY(s.takeResync(r, id));
  s.tr.onResyncAcknowledged(id);
  QVERIFY(s.tr.dualChain());

  // The CIAK came back synchronously but the server applies the position in a QUEUED event: these
  // moves are still old-chain.
  Point before = s.cursor;
  auto d = s.move(476, 541);
  QVERIFY(d.oldChainStraggler);
  COMPARE_POINT(d.inject, before.x - 3, before.y + 1);
  d = s.move(473, 541);
  QVERIFY(d.oldChainStraggler);
  COMPARE_POINT(d.inject, before.x - 6, before.y + 1);

  // Now the new chain (anchored at the reported centre) arrives.
  d = s.move(960 - 4, 540 + 2);
  QVERIFY(d.newChainWon);
  COMPARE_POINT(d.delta, -4, 2);
  COMPARE_POINT(d.inject, before.x - 10, before.y + 3);
  QVERIFY(!s.tr.dualChain());

  // Old chain dropped for good; ordinary deltas on the new chain.
  d = s.move(960 - 6, 540 + 2);
  QVERIFY(!d.oldChainStraggler);
  COMPARE_POINT(d.delta, -2, 0);
  COMPARE_POINT(d.inject, before.x - 12, before.y + 3);
  QCOMPARE(s.tr.mode(), Mode::Active);
}

void WarpGrabTrackerTests::recentreNewChainFirstAfterAck()
{
  Sim s;
  QVERIFY(s.enterSession(960, 540, 200, kGrab));
  s.move(479, 540);
  Point r;
  uint32_t id = 0;
  QVERIFY(s.takeResync(r, id));
  Point before = s.cursor;
  s.tr.onResyncAcknowledged(id);
  auto d = s.move(960 + 7, 540);
  QVERIFY(d.newChainWon);
  QVERIFY(!d.oldChainStraggler);
  COMPARE_POINT(d.inject, before.x + 7, before.y);
  QVERIFY(!s.tr.dualChain());
}

void WarpGrabTrackerTests::endOnTokenChange()
{
  Sim s;
  QVERIFY(s.enterSession(960, 540, 200, kGrab));
  s.cursor = {1300, 600};              // a last warp, e.g. the app restoring the cursor on release
  s.token = WarpGrabTracker::kNoToken; // capture released
  auto d = s.move(965, 540);
  QCOMPARE(d.ended, EndReason::TokenChanged);
  QCOMPARE(d.resync, ResyncKind::End);
  QCOMPARE(s.tr.mode(), Mode::Ending);
  COMPARE_POINT(d.inject, 1304, 600);
  Point r;
  uint32_t id = 0;
  QVERIFY(s.takeResync(r, id));
  COMPARE_POINT(r, 1304, 600); // report the TRUE cursor

  // Until acknowledged, still delta mode on the old chain.
  d = s.move(970, 540);
  QVERIFY(d.tracked);
  COMPARE_POINT(d.inject, 1309, 600);

  s.tr.onResyncAcknowledged(id);
  d = s.move(972, 540); // a post-ack straggler
  QVERIFY(d.oldChainStraggler);
  COMPARE_POINT(d.inject, 1311, 600);
  QCOMPARE(s.tr.mode(), Mode::Ending);

  // The new chain is anchored at 1304,600 (what we reported); it now carries the user's motion.
  d = s.move(1304 + 9, 600);
  QVERIFY(d.newChainWon);
  QVERIFY(d.returnedToIdle);
  QCOMPARE(s.tr.mode(), Mode::Idle);
  COMPARE_POINT(d.inject, 1320, 600);

  // Idle again: stock absolute injection, and the token is not consulted without a discrepancy.
  const int calls = s.tokenCalls;
  d = s.move(1322, 601);
  QVERIFY(!d.tracked);
  COMPARE_POINT(d.inject, 1322, 601);
  QCOMPARE(s.tokenCalls, calls);
}

void WarpGrabTrackerTests::endOnTimeout()
{
  Sim s;
  QVERIFY(s.enterSession(960, 540, 200, kGrab));
  // Grab still held, but the app has stopped warping: 100 ms moves, no warps.
  WarpGrabTracker::Decision d;
  int64_t elapsed = 0;
  int32_t x = 961;
  const int64_t step = 100;
  while (elapsed <= kTestTimeoutMs) {
    x += 1;
    d = s.move(x, 540, step);
    elapsed += step;
    if (elapsed <= kTestTimeoutMs) {
      QCOMPARE(d.ended, EndReason::None);
      QCOMPARE(s.tr.mode(), Mode::Active);
    }
  }
  QCOMPARE(d.ended, EndReason::WarpTimeout);
  QCOMPARE(s.tr.mode(), Mode::Ending);
  QCOMPARE(d.resync, ResyncKind::End);
}

void WarpGrabTrackerTests::endDeferredWhileRecentreSettles()
{
  Sim s;
  QVERIFY(s.enterSession(960, 540, 200, kGrab));
  s.move(479, 540); // recentre requested
  Point rr;
  uint32_t rid = 0;
  QVERIFY(s.takeResync(rr, rid));
  s.token = 99; // grab owner changed while the recentre is in flight
  auto d = s.move(478, 540);
  QCOMPARE(d.ended, EndReason::TokenChanged);
  QCOMPARE(d.resync, ResyncKind::None); // one resync at a time
  QCOMPARE(s.tr.mode(), Mode::Ending);

  s.tr.onResyncAcknowledged(rid);
  d = s.move(960 - 2, 540); // the recentre settles...
  QVERIFY(d.newChainWon);
  QCOMPARE(d.resync, ResyncKind::End); // ...and only now is the end resync sent
  QVERIFY(!d.returnedToIdle);
  Point r;
  uint32_t eid = 0;
  QVERIFY(s.takeResync(r, eid));
  COMPARE_POINT(r, d.inject.x, d.inject.y);
  QVERIFY(eid != rid);

  s.tr.onResyncAcknowledged(eid);
  d = s.move(r.x + 1, r.y);
  QVERIFY(d.returnedToIdle);
  QCOMPARE(s.tr.mode(), Mode::Idle);
}

void WarpGrabTrackerTests::endWithoutResyncWhenAlreadyAligned()
{
  Sim s;
  s.move(500, 500);
  s.cursor = {600, 500};
  s.token = kGrab;
  QVERIFY(s.move(510, 500).entered); // cursor 610,500
  // The app warps the cursor exactly back onto the server's chain and releases.
  s.cursor = {510, 500};
  s.token = WarpGrabTracker::kNoToken;
  auto d = s.move(515, 500);
  QCOMPARE(d.ended, EndReason::TokenChanged);
  QVERIFY(d.returnedToIdle);
  QCOMPARE(d.resync, ResyncKind::None);
  QCOMPARE(s.tr.mode(), Mode::Idle);
  COMPARE_POINT(d.inject, 515, 500);
}

void WarpGrabTrackerTests::staleLandedReadDoesNotEatADelta()
{
  Sim s;
  QVERIFY(s.enterSession(960, 540, 200, kGrab)); // cursor 1161,540
  // This time the post-injection read is taken before the injection is applied...
  auto d = s.tr.onServerMove({971, 540}, s.cursor, grabToken, s.now += 8, s.shape);
  COMPARE_POINT(d.inject, 1171, 540);
  s.tr.noteLanded(s.cursor); // stale: still 1161,540
  // ...and the next pre-injection read is STILL stale.
  d = s.tr.onServerMove({981, 540}, s.cursor, grabToken, s.now += 8, s.shape);
  QVERIFY(!d.warpObserved);
  COMPARE_POINT(d.inject, 1181, 540); // built on where the previous injection is going, not lost
}

void WarpGrabTrackerTests::resetOnLeaveEnterOrShapeChange()
{
  Sim s;
  QVERIFY(s.enterSession(960, 540, 200, kGrab));
  s.move(479, 540); // a recentre is pending too
  s.tr.reset();     // leave / enter / genuine shape change / disconnect / DMRM
  QCOMPARE(s.tr.mode(), Mode::Idle);
  QVERIFY(!s.tr.resyncAwaitingAck());
  Point p;
  uint32_t id = 0;
  QVERIFY(!s.takeResync(p, id));
  // The next move (the enter move) is stock and re-seeds: even though the cursor is far away and
  // the grab is still held, nothing is tracked.
  s.cursor = {10, 10};
  auto d = s.move(1900, 300);
  QVERIFY(!d.tracked);
  COMPARE_POINT(d.inject, 1900, 300);
  d = s.move(1890, 300);
  COMPARE_POINT(d.inject, 1890, 300);
  QCOMPARE(s.tr.mode(), Mode::Idle);
}

void WarpGrabTrackerTests::staleAckAfterResetIsIgnored()
{
  Sim s;
  QVERIFY(s.enterSession(960, 540, 200, kGrab));
  s.move(479, 540);
  Point r;
  uint32_t id = 0;
  QVERIFY(s.takeResync(r, id));
  s.tr.reset();
  s.tr.onResyncAcknowledged(id); // the CIAK for a resync sent before the reset
  QVERIFY(!s.tr.dualChain());
  s.move(500, 500);
  auto d = s.move(505, 500);
  COMPARE_POINT(d.inject, 505, 500);
  QCOMPARE(s.tr.mode(), Mode::Idle);
}

QTEST_MAIN(WarpGrabTrackerTests)
