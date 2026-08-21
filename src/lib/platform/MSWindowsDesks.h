/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/MouseTypes.h"
#include "deskflow/OptionTypes.h"
#include "mt/CondVar.h"
#include "mt/Mutex.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

class Event;
class EventQueueTimer;
class Thread;
class IJob;
class IScreenSaver;
class IEventQueue;

//! Microsoft Windows desk handling
/*!
Desks in Microsoft Windows are only remotely like desktops on X11
systems.  A desk is another virtual surface for windows but desks
impose serious restrictions:  a thread can interact with only one
desk at a time, you can't switch desks if the thread has any hooks
installed or owns any windows, windows cannot exist on multiple
desks at once, etc.  Basically, they're useless except for running
the login window or the screensaver, which is what they're used
for.  Deskflow must deal with them mainly because of the login
window and screensaver but users can create their own desks and
deskflow should work on those too.

This class encapsulates all the desk nastiness.  Clients of this
object don't have to know anything about desks.
*/
class MSWindowsDesks
{
public:
  //! Constructor
  /*!
  \p isPrimary is true iff the desk is for a primary screen.
  \p screensaver points to a screensaver object and it's used
  only to check if the screensaver is active.  The \p updateKeys
  job is adopted and is called when the key state should be
  updated in a thread attached to the current desk.
  \p hookLibrary must be a handle to the hook library.
  */
  MSWindowsDesks(bool isPrimary, bool useHooks, const IScreenSaver *screensaver, IEventQueue *events, IJob *updateKeys);
  ~MSWindowsDesks();

  //! @name manipulators
  //@{

  //! Enable desk tracking
  /*!
  Enables desk tracking.  While enabled, this object checks to see
  if the desk has changed and ensures that the hooks are installed
  on the new desk.  \c setShape should be called at least once
  before calling \c enable.
  */
  void enable();

  //! Disable desk tracking
  /*!
  Disables desk tracking.  \sa enable.
  */
  void disable();

  //! Notify of entering a desk
  /*!
  Prepares a desk for when the cursor enters it.
  */
  void enter();

  //! Notify of leaving a desk
  /*!
  Prepares a desk for when the cursor leaves it.
  */
  void leave(HKL keyLayout);

  //! Notify of options changes
  /*!
  Resets all options to their default values.
  */
  void resetOptions();

  //! Notify of options changes
  /*!
  Set options to given values.  Ignores unknown options and doesn't
  modify options that aren't given in \c options.
  */
  void setOptions(const OptionsList &options);

  //! Update the key state
  /*!
  Causes the key state to get updated to reflect the physical keyboard
  state and current keyboard mapping.
  */
  void updateKeys();

  //! Tell desk about new size
  /*!
  This tells the desks that the display size has changed.
  */
  void setShape(int32_t x, int32_t y, int32_t width, int32_t height, int32_t xCenter, int32_t yCenter, bool isMultimon);

  // MouseTransfer (RDP adaptive cursor park): hand the wrapper-published off-window rest point to
  // MSWindowsScreen's warp path. Returns false (x/y untouched) when no park is active, so the caller
  // keeps the screen centre. Public because MSWindowsScreen calls it; the poll that fills it
  // (checkRdpPark) stays private. Thread-safe (a single lock-free atomic).
  bool getRdpPark(int32_t &x, int32_t &y) const;

  //! Install/uninstall screensaver hooks
  /*!
  If \p install is true then the screensaver hooks are installed and,
  if desk tracking is enabled, updated whenever the desk changes.  If
  \p install is false then the screensaver hooks are uninstalled.
  */
  void installScreensaverHooks(bool install);

  //! Start ignoring user input
  /*!
  Starts ignoring user input so we don't pick up our own synthesized events.
  */
  void fakeInputBegin();

  //! Stop ignoring user input
  /*!
  Undoes whatever \c fakeInputBegin() did.
  */
  void fakeInputEnd();

  //@}
  //! @name accessors
  //@{

  //! Get cursor position
  /*!
  Return the current position of the cursor in \c x and \c y.
  */
  void getCursorPos(int32_t &x, int32_t &y) const;

  //! Fake key press/release
  /*!
  Synthesize a press or release of key \c button.
  */
  void fakeKeyEvent(WORD virtualKey, WORD scanCode, DWORD flags, bool isAutoRepeat) const;

  //! Fake mouse press/release
  /*!
  Synthesize a press or release of mouse button \c id.
  */
  void fakeMouseButton(ButtonID id, bool press);

  //! Fake mouse move
  /*!
  Synthesize a mouse move to the absolute coordinates \c x,y.
  */
  void fakeMouseMove(int32_t x, int32_t y) const;

  //! Fake mouse move
  /*!
  Synthesize a mouse move to the relative coordinates \c dx,dy.
  */
  void fakeMouseRelativeMove(int32_t dx, int32_t dy) const;

  //! Fake mouse wheel
  /*!
  Synthesize a mouse wheel event of amount \c delta in direction \c axis.
  */
  void fakeMouseWheel(int32_t xDelta, int32_t yDelta) const;

  //@}

private:
  class Desk
  {
  public:
    std::wstring m_name;
    Thread *m_thread;
    DWORD m_threadID;
    DWORD m_targetID;
    HDESK m_desk;
    HWND m_window;
    HWND m_foregroundWindow;
    bool m_lowLevel;
  };
  using Desks = std::map<std::wstring, Desk *>;

  // initialization and shutdown operations
  HCURSOR createBlankCursor() const;
  void destroyCursor(HCURSOR cursor) const;
  ATOM createDeskWindowClass(bool isPrimary) const;
  void destroyClass(ATOM windowClass) const;
  HWND createWindow(ATOM windowClass, const wchar_t *name) const;
  void destroyWindow(HWND) const;

  // message handlers
  void deskMouseMove(int32_t x, int32_t y) const;
  void deskMouseRelativeMove(int32_t dx, int32_t dy) const;
  void deskEnter(Desk *desk);
  void deskLeave(Desk *desk, HKL keyLayout);
  void deskThread(const void *vdesk);

  // desk switch checking and handling
  Desk *addDesk(const std::wstring &name, HDESK hdesk);
  void removeDesks();
  void checkDesk();
  bool isDeskAccessible(const Desk *desk) const;
  void handleCheckDesk();

  // MouseTransfer (RDP window handoff foreground hold): poll the wrapper's rdp-fg-hold file and
  // keep the window it names foreground while the shared cursor is away. checkRdpFgHold runs on
  // the event thread (piggybacked on handleCheckDesk's 0.2s timer, the same cadence the
  // switchreq poll blessed); deskRdpFgHold does the actual foreground work on the desk thread,
  // where deskLeave's own proven AttachThreadInput recipe lives. See deskRdpFgHold for why only
  // this process can do it at all.
  void checkRdpFgHold();
  void checkRdpFgHoldFast();
  void deskRdpFgHold(Desk *desk, HWND hwnd);

  // MouseTransfer (RDP window handoff — ADAPTIVE CURSOR PARK): poll the wrapper's rdp-park file and
  // publish the point the shared cursor should REST at while it is away on a client, so it does not
  // sit on top of the window the origin is streaming over RDP (which the wrapper positioned it off
  // of). Same file+liveness convention as checkRdpFgHold, same 0.2s handleCheckDesk cadence.
  // getRdpPark hands the current park (if any) to MSWindowsScreen, which substitutes it for the
  // screen centre in leave()/onMouseMove()'s warp-and-recentre — see MSWindowsScreen::rdpWarpCentre.
  // Storage is a single lock-free atomic (packed x:y, UINT64_MAX = unset) because checkRdpPark runs
  // on the event thread and getRdpPark is read from the primary hook path on another thread.
  void checkRdpPark();

  // EVENT_SYSTEM_DESKTOPSWITCH WinEvent callback: re-syncs key state on a desktop switch (the
  // event-driven fix for the Ctrl+Alt+Del / secure-desktop stuck-modifier). See enable().
  static void CALLBACK onDesktopSwitchEvent(
      HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
      DWORD idEventThread, DWORD dwmsEventTime
  );

  // communication with desk threads
  void waitForDesk() const;
  void sendMessage(UINT, WPARAM, LPARAM) const;

  // work around for messed up keyboard events from low-level hooks
  HWND getForegroundWindow() const;

  // desk API wrappers
  HDESK openInputDesktop();
  void closeDesktop(HDESK);
  std::wstring getDesktopName(HDESK);

  // our desk window procs
  static LRESULT CALLBACK primaryDeskProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK secondaryDeskProc(HWND, UINT, WPARAM, LPARAM);

private:
  // true if screen is being used as a primary screen, false otherwise
  bool m_isPrimary;

  // true if hooks are to be installed (when debugging, it can be useful to disable hooks)
  bool m_useHooks;

  // true if mouse has entered the screen
  bool m_isOnScreen;

  // our resources
  ATOM m_deskClass;
  HCURSOR m_cursor;

  // screen shape stuff
  int32_t m_x = 0;
  int32_t m_y = 9;
  int32_t m_w = 0;
  int32_t m_h = 0;
  int32_t m_xCenter = 0;
  int32_t m_yCenter = 0;

  // true if system appears to have multiple monitors
  bool m_multimon = false;

  // the timer used to check for desktop switching
  EventQueueTimer *m_timer = nullptr;

  // screen saver stuff
  DWORD m_threadID;
  const IScreenSaver *m_screensaver;
  bool m_screensaverNotify = false;

  // the current desk and it's name
  Desk *m_activeDesk = nullptr;
  std::wstring m_activeDeskName;

  // one desk per desktop and a cond var to communicate with it
  Mutex m_mutex;
  CondVar<bool> m_deskReady;
  Desks m_desks;

  // keyboard stuff
  IJob *m_updateKeys;
  HKL m_keyLayout;

  // EVENT_SYSTEM_DESKTOPSWITCH WinEvent hook (event-driven key resync on desktop switch) + the
  // single instance its static callback routes through. Installed/removed in enable()/disable().
  HWINEVENTHOOK m_desktopEventHook = nullptr;
  static MSWindowsDesks *s_instance;

  // options
  bool m_leaveForegroundOption;

  // MouseTransfer (RDP window handoff foreground hold) — see checkRdpFgHold/deskRdpFgHold.
  // m_fgHoldFile is the signal file's path (resolved once in enable()); m_fgHoldActive is the
  // EVENT thread's view ("a hold is in force"), used to send the release exactly once;
  // m_fgHoldHwnd is the DESK thread's state (the window currently held foreground), used to
  // restore the hider on release and to keep deskLeave from saving the held window as the
  // foreground-to-restore.
  std::string m_fgHoldFile;
  bool m_fgHoldActive = false;
  HWND m_fgHoldHwnd = nullptr;
  // MouseTransfer (History context-menu fix, 2026-08-21): the window the hold currently names, cached
  // by checkRdpFgHold (event thread) so the FAST fg-hold timer can re-assert it below the ~125ms menu
  // cancel without re-reading the file. nullptr when no hold is in force. See checkRdpFgHoldFast.
  HWND m_fgHoldWant = nullptr;
  // The faster (sub-125ms) fg-hold re-assert timer. It ONLY acts in the one case checkRdpFgHold's 0.2s
  // cadence is too slow for: the held export has an OWNED popup up (a context menu) whose foreground a
  // SAME-PROCESS non-popup window (the app's main form) has stolen — the History failure. A cheap
  // no-op in every other state, so working palettes never touch it. See checkRdpFgHoldFast.
  EventQueueTimer *m_fgHoldFastTimer = nullptr;
  // MouseTransfer (History insta-close INSTRUMENTATION, 2026-08-21): the last DIAG line each fg-hold
  // path logged, so the change-gated LOG_INFO in checkRdpFgHold/checkRdpFgHoldFast emits only on a
  // STATE TRANSITION rather than at the 5/s (slow) and 25/s (fast) timer rates — one line per real
  // change while a hold is in force, nothing at all when none is. Diagnostic only; no behaviour rides
  // on these. Cleared to "" when the hold ends so the next episode logs fresh.
  std::string m_fgHoldDbgSlow;
  std::string m_fgHoldDbgFast;

  // MouseTransfer (RDP adaptive cursor park) — see checkRdpPark/getRdpPark. m_rdpParkFile is the
  // signal file's path (resolved once in enable(), beside m_fgHoldFile); m_rdpPark is the published
  // park, packed as (uint32(x) << 32 | uint32(y)), with UINT64_MAX meaning "no park — use the screen
  // centre". A computed off-window park is never exactly (-1,-1), so it can never collide with the
  // sentinel. Written only by checkRdpPark (event thread), read only by getRdpPark (hook thread).
  std::string m_rdpParkFile;
  std::atomic<uint64_t> m_rdpPark{UINT64_MAX};

  IEventQueue *m_events;
};
