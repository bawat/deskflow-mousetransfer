/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 MouseTransfer fork
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>
#include <functional>

namespace deskflow {

//! Client-side cursor-warp grab tracker (MouseTransfer fork)
/*!
THE BUG THIS EXISTS FOR. The server keeps a VIRTUAL cursor for the active client (Server::
onMouseMoveSecondary, `m_x += dx`) and sends it ABSOLUTE positions (DMMV), which the client injects
absolutely. An application on the client that warps the cursor itself -- Blender's continuous grab,
value scrubbing, viewport wrap, a game re-centring every frame -- calls SetCursorPos and nobody tells
the server, so the very next absolute move snaps the cursor back to where the server thinks it is.
The application reads that whole snap-back distance as mouse travel, warps again, and the values it
is editing "skyrocket".

THE FIX, CLIENT-SIDE ONLY. While an application is observed to be warping a cursor it has GRABBED,
every server move is converted to a delta against the previous server position and injected at
(true cursor + delta) -- so the application's warp is honoured and only the user's motion is added.
To keep the server's virtual cursor useful (away from the edges, where it would clamp and lose
deltas, or cross a seam mid-grab) and to put it back where the cursor really is afterwards, the
tracker asks for a RESYNC: the platform layer reports a position to the server in an ordinary DINF,
which every Deskflow server already applies to `m_x,m_y` of the active client
(ClientProxy1_0::recvInfo + Server::handleShapeChanged). No protocol extension, no server change.

COORDINATE CHAINS. The server acknowledges a DINF (CIAK) synchronously inside recvInfo but applies
the new position in a QUEUED ScreenShapeChanged event, so moves can arrive both before AND just after
the CIAK that are still measured from the OLD server position. After an ack the tracker therefore
holds both anchors and attributes each move to whichever chain explains it with the smaller delta;
the first time the new chain wins, the old one is dropped for good. When the two anchors are close a
misattribution costs at most their distance, which is then small.

This class is PURE -- no OS calls, no logging, no clock -- so every rule above is unit-tested
(src/unittests/deskflow/WarpGrabTrackerTests.cpp). The platform layer supplies the true cursor read
immediately before injection, the post-injection read, a monotonic clock, the client shape and the
lazily-evaluated "grab token". NOT thread-safe: the owner serialises access.
*/
class WarpGrabTracker
{
public:
  //! Identity of "who has grabbed the cursor". 0 means nobody, which keeps the tracker inert.
  using Token = uint64_t;
  static constexpr Token kNoToken = 0;
  //! Evaluated lazily: in Idle only once a discrepancy has been seen, in Active once per move.
  using TokenProvider = std::function<Token()>;

  struct Point
  {
    int32_t x = 0;
    int32_t y = 0;
  };

  struct Shape
  {
    int32_t x = 0;
    int32_t y = 0;
    int32_t w = 0;
    int32_t h = 0;
  };

  struct Config
  {
    //! Idle -> Active needs the cursor to be MORE than this far (either axis) from BOTH the last
    //! position we landed and the last position we asked for. Policy threshold, chosen by the
    //! platform layer (Windows derives it from the system drag rectangle).
    int32_t entryMinDx = 0;
    int32_t entryMinDy = 0;
    //! Positions within this many pixels are "the same point" -- the round-trip error of the
    //! platform's absolute-injection coordinate normalisation. Used for warp liveness in Active.
    int32_t roundingPx = 0;
    //! Active ends once no warp has been observed for this long.
    int64_t warpTimeoutMs = 0;
  };

  enum class Mode
  {
    Idle,   //!< stock behaviour: inject the server position as-is
    Active, //!< grab session: inject true cursor + server delta
    Ending  //!< session over; still delta mode until the server's virtual cursor is re-seated
  };

  enum class EndReason
  {
    None,
    TokenChanged, //!< the grab owner went away or changed
    WarpTimeout   //!< the owner stopped warping
  };

  enum class ResyncKind
  {
    None,
    Recentre, //!< keep the server's virtual cursor away from the edges
    End       //!< put the server's virtual cursor back on the true cursor
  };

  //! What to do with one server move. Everything except `inject` is for tracing.
  struct Decision
  {
    Point inject;                         //!< where to inject (absolute)
    bool tracked = false;                 //!< true => delta mode produced `inject`
    bool entered = false;                 //!< Idle -> Active on this move
    EndReason ended = EndReason::None;    //!< Active -> Ending on this move
    bool returnedToIdle = false;          //!< Ending -> Idle on this move
    ResyncKind resync = ResyncKind::None; //!< a resync was requested on this move
    bool warpObserved = false;            //!< Active/Ending: the cursor was moved by someone else
    bool newChainWon = false;             //!< the post-ack ambiguity resolved on this move
    bool oldChainStraggler = false;       //!< attributed to the old chain while both were live
    Token token = kNoToken;               //!< the token as evaluated on this move (if it was)
    Point base;                           //!< delta mode: the point the delta was added to
    Point delta;                          //!< delta mode: the server delta applied
  };

  explicit WarpGrabTracker(const Config &config);

  //! Decide one incoming absolute server move.
  /*!
  \p server is the move's position, \p cursor the TRUE cursor read immediately before injecting,
  \p nowMs a monotonic clock, \p shape the client screen shape (the same one reported to the
  server). The caller injects Decision::inject and should then call noteLanded().
  */
  Decision onServerMove(Point server, Point cursor, const TokenProvider &token, int64_t nowMs, const Shape &shape);

  //! The true cursor read immediately after injecting the last decision.
  void noteLanded(Point landed);

  //! Hand a pending resync request to the sender (main thread). One-shot.
  bool takeResyncRequest(Point &pos, uint32_t &id);

  //! The server acknowledged resync \p id. A stale id (from before a reset) is ignored.
  void onResyncAcknowledged(uint32_t id);

  //! Forget everything, back to Idle, no resync. Screen enter/leave, shape change, disconnect,
  //! relative (DMRM) moves -- anything after which the old coordinate chain means nothing.
  void reset();

  Mode mode() const
  {
    return m_mode;
  }

  //! A resync has been requested and its ack has not yet arrived.
  bool resyncAwaitingAck() const
  {
    return m_pendingId != 0;
  }

  //! Both chains are live (acked, not yet resolved).
  bool dualChain() const
  {
    return m_dual;
  }

  const Config &config() const
  {
    return m_config;
  }

private:
  bool differsFromBoth(Point cursor, int32_t minDx, int32_t minDy) const;
  Point chooseBase(Point cursor) const;
  Point chainDelta(Point server, Decision &d);
  void issueResync(Point pos, ResyncKind kind, Decision &d);
  void commit(Point target);
  void goIdle();
  static Point clampTo(Point p, const Shape &shape);
  static bool near(Point a, Point b, int32_t tol);
  bool nearEdge(Point s, const Shape &shape) const;

  Config m_config;
  Mode m_mode = Mode::Idle;

  // History. m_haveHistory == false => the next move is injected stock and seeds everything.
  bool m_haveHistory = false;
  Point m_prevS;      // last server position on the current (old) chain
  Point m_lastTarget; // last position we asked the OS for
  Point m_lastLanded; // true cursor read right after that injection

  // Session.
  Token m_entryToken = kNoToken;
  int64_t m_lastWarpMs = 0;
  bool m_endWanted = false; // Ending, but a recentre resync is still settling first

  // Resync bookkeeping. At most ONE resync is outstanding (requested..settled) at any time.
  uint32_t m_nextId = 1;
  uint32_t m_pendingId = 0;  // requested, awaiting ack (0 = none)
  bool m_requestReady = false; // requested, not yet taken by the sender
  Point m_pendingPos;
  ResyncKind m_inflightKind = ResyncKind::None;
  bool m_dual = false; // acked; both chains live
  Point m_newAnchor;   // last position on the new chain (starts at the reported position)
};

} // namespace deskflow
