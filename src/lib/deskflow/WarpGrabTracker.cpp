/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 MouseTransfer fork
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/WarpGrabTracker.h"

#include <cstdlib>

namespace deskflow {

namespace {

//! Recentre once the server's virtual cursor is within 1/kRecentreEdgeDivisor of a dimension of any
//! edge. Policy, not a measurement: a quarter of the screen is far more travel than any single
//! round trip of moves can cover before the recentre lands, so the virtual cursor never reaches the
//! edge where the server would clamp it (losing the user's deltas) or switch screens mid-grab, while
//! still letting it roam the middle half freely (no resync storm).
constexpr int32_t kRecentreEdgeDivisor = 4;

int32_t l1(WarpGrabTracker::Point p)
{
  return std::abs(p.x) + std::abs(p.y);
}

} // namespace

WarpGrabTracker::WarpGrabTracker(const Config &config) : m_config(config)
{
  // nothing
}

bool WarpGrabTracker::isNear(Point a, Point b, int32_t tol)
{
  return std::abs(a.x - b.x) <= tol && std::abs(a.y - b.y) <= tol;
}

WarpGrabTracker::Point WarpGrabTracker::clampTo(Point p, const Shape &shape)
{
  if (shape.w <= 0 || shape.h <= 0) {
    return p;
  }
  if (p.x < shape.x) {
    p.x = shape.x;
  } else if (p.x > shape.x + shape.w - 1) {
    p.x = shape.x + shape.w - 1;
  }
  if (p.y < shape.y) {
    p.y = shape.y;
  } else if (p.y > shape.y + shape.h - 1) {
    p.y = shape.y + shape.h - 1;
  }
  return p;
}

bool WarpGrabTracker::differsFromBoth(Point cursor, int32_t minDx, int32_t minDy) const
{
  // "Something ELSE moved the cursor" needs the cursor to be away from BOTH the point we asked for
  // and the point we actually read back afterwards. Either alone lies: the OS clamps a request
  // into a monitor-gap or off-edge position (target != landed, and the next read matches landed),
  // and a read taken before the injection has been applied leaves landed stale (the next read
  // matches target). Neither of those is a warp, and comparing against both rejects them.
  auto away = [&](Point ref) { return std::abs(cursor.x - ref.x) > minDx || std::abs(cursor.y - ref.y) > minDy; };
  return away(m_lastLanded) && away(m_lastTarget);
}

WarpGrabTracker::Point WarpGrabTracker::chooseBase(Point cursor) const
{
  // Normally the base is the TRUE cursor: that is the whole point (honour whatever the app did).
  // One exception keeps a lagging read from eating a delta: if the read taken right after our last
  // injection did not show it landed (landed != target) and the cursor STILL reads as that stale
  // point, our previous injection is simply not applied yet, so build on where it is going to be.
  // A warp moves the cursor away from the stale point, so it is never mistaken for this.
  const int32_t tol = m_config.roundingPx;
  const bool landedFresh = isNear(m_lastLanded, m_lastTarget, tol);
  if (!landedFresh && isNear(cursor, m_lastLanded, tol) && !isNear(cursor, m_lastTarget, tol)) {
    return m_lastTarget;
  }
  return cursor;
}

WarpGrabTracker::Point WarpGrabTracker::chainDelta(Point server, Decision &d)
{
  if (m_dual) {
    const Point dOld{server.x - m_prevS.x, server.y - m_prevS.y};
    const Point dNew{server.x - m_newAnchor.x, server.y - m_newAnchor.y};
    // Ties go to the new chain: with equal magnitudes either attribution costs the same, and
    // resolving ends the ambiguity (anchors that coincide would otherwise tie forever).
    if (l1(dNew) <= l1(dOld)) {
      m_dual = false;
      m_prevS = server;
      d.newChainWon = true;
      return dNew;
    }
    d.oldChainStraggler = true;
    m_prevS = server;
    return dOld;
  }
  const Point delta{server.x - m_prevS.x, server.y - m_prevS.y};
  m_prevS = server;
  return delta;
}

void WarpGrabTracker::issueResync(Point pos, ResyncKind kind, Decision &d)
{
  m_pendingId = m_nextId++;
  if (m_nextId == 0) {
    m_nextId = 1; // 0 means "none"
  }
  m_pendingPos = pos;
  m_requestReady = true;
  m_inflightKind = kind;
  d.resync = kind;
}

void WarpGrabTracker::commit(Point target)
{
  m_lastTarget = target;
  // Provisional until noteLanded(): if the platform cannot read the cursor back, treat the request
  // as landed, which is what stock behaviour assumes about every injection.
  m_lastLanded = target;
  m_haveHistory = true;
}

void WarpGrabTracker::goIdle()
{
  m_mode = Mode::Idle;
  m_entryToken = kNoToken;
  m_endWanted = false;
  m_inflightKind = ResyncKind::None;
}

bool WarpGrabTracker::nearEdge(Point s, const Shape &shape) const
{
  if (shape.w <= 0 || shape.h <= 0) {
    return false;
  }
  const int32_t mx = shape.w / kRecentreEdgeDivisor;
  const int32_t my = shape.h / kRecentreEdgeDivisor;
  return s.x < shape.x + mx || s.x > shape.x + shape.w - 1 - mx || s.y < shape.y + my ||
         s.y > shape.y + shape.h - 1 - my;
}

WarpGrabTracker::Decision
WarpGrabTracker::onServerMove(Point server, Point cursor, const TokenProvider &token, int64_t nowMs, const Shape &shape)
{
  Decision d;

  if (!m_haveHistory) {
    // First move after a reset (e.g. the enter move): stock injection, and it seeds the chain.
    m_prevS = server;
    commit(server);
    d.inject = server;
    return d;
  }

  if (m_mode == Mode::Idle) {
    Token t = kNoToken;
    if (differsFromBoth(cursor, m_config.entryMinDx, m_config.entryMinDy)) {
      // Only now is the (comparatively expensive) token worth evaluating.
      t = token ? token() : kNoToken;
      d.token = t;
    }
    if (t == kNoToken) {
      // Stock behaviour, byte for byte: inject the server position.
      m_prevS = server;
      commit(server);
      d.inject = server;
      return d;
    }

    // ENTER without injecting the server position at all -- no snap-back, not even once.
    m_mode = Mode::Active;
    m_entryToken = t;
    m_lastWarpMs = nowMs;
    d.entered = true;
    d.warpObserved = true;
    d.tracked = true;
    d.base = cursor;
    d.delta = chainDelta(server, d);
    const Point target = clampTo({cursor.x + d.delta.x, cursor.y + d.delta.y}, shape);
    if (m_pendingId == 0 && !m_dual && nearEdge(server, shape)) {
      issueResync({shape.x + shape.w / 2, shape.y + shape.h / 2}, ResyncKind::Recentre, d);
    }
    commit(target);
    d.inject = target;
    return d;
  }

  // Active / Ending: delta mode.
  d.tracked = true;
  if (differsFromBoth(cursor, m_config.roundingPx, m_config.roundingPx)) {
    m_lastWarpMs = nowMs;
    d.warpObserved = true;
  }
  d.base = chooseBase(cursor);
  d.delta = chainDelta(server, d);
  const Point target = clampTo({d.base.x + d.delta.x, d.base.y + d.delta.y}, shape);

  if (m_mode == Mode::Ending) {
    if (d.newChainWon) {
      if (m_endWanted) {
        // The recentre that was in flight when the session ended has settled; NOW re-seat the
        // server on the true cursor.
        m_endWanted = false;
        issueResync(target, ResyncKind::End, d);
      } else {
        // The end resync has settled: the server's chain now tracks the true cursor.
        goIdle();
        d.returnedToIdle = true;
      }
    }
    commit(target);
    d.inject = target;
    return d;
  }

  // Active.
  const Token t = token ? token() : kNoToken;
  d.token = t;
  if (t != m_entryToken) {
    d.ended = EndReason::TokenChanged;
  } else if (nowMs - m_lastWarpMs > m_config.warpTimeoutMs) {
    d.ended = EndReason::WarpTimeout;
  }

  if (d.ended != EndReason::None) {
    if (m_pendingId != 0 || m_dual) {
      // A recentre is still settling; one resync at a time, so the end resync follows it.
      m_mode = Mode::Ending;
      m_endWanted = true;
    } else if (isNear(target, server, m_config.roundingPx)) {
      // The server's virtual cursor already sits on the true cursor: nothing to re-seat.
      goIdle();
      d.returnedToIdle = true;
    } else {
      m_mode = Mode::Ending;
      issueResync(target, ResyncKind::End, d);
    }
  } else if (m_pendingId == 0 && !m_dual && nearEdge(server, shape)) {
    issueResync({shape.x + shape.w / 2, shape.y + shape.h / 2}, ResyncKind::Recentre, d);
  }

  commit(target);
  d.inject = target;
  return d;
}

void WarpGrabTracker::noteLanded(Point landed)
{
  m_lastLanded = landed;
}

bool WarpGrabTracker::takeResyncRequest(Point &pos, uint32_t &id)
{
  if (!m_requestReady) {
    return false;
  }
  m_requestReady = false;
  pos = m_pendingPos;
  id = m_pendingId;
  return true;
}

void WarpGrabTracker::onResyncAcknowledged(uint32_t id)
{
  if (id == 0 || id != m_pendingId) {
    return; // stale (a reset happened since it was sent) or not ours
  }
  m_pendingId = 0;
  m_requestReady = false;
  m_dual = true;
  m_newAnchor = m_pendingPos;
}

void WarpGrabTracker::reset()
{
  goIdle();
  m_haveHistory = false;
  m_pendingId = 0;
  m_requestReady = false;
  m_dual = false;
}

} // namespace deskflow
