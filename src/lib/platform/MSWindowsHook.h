/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2011 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <stdint.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#define DESKFLOW_MSG_MARK WM_APP + 0x0011         // mark id; <unused>
#define DESKFLOW_MSG_KEY WM_APP + 0x0012          // vk code; key data
#define DESKFLOW_MSG_MOUSE_BUTTON WM_APP + 0x0013 // button msg; <unused>
#define DESKFLOW_MSG_MOUSE_WHEEL WM_APP + 0x0014  // delta; <unused>
#define DESKFLOW_MSG_MOUSE_MOVE WM_APP + 0x0015   // x; y
#define DESKFLOW_MSG_POST_WARP WM_APP + 0x0016    // <unused>; <unused>
#define DESKFLOW_MSG_PRE_WARP WM_APP + 0x0017     // x; y
// Merged-fork: a TAGGED local injection (kDeskflowLocalInjectSignature) moved the physical
// cursor to x;y. Bookkeeping only -- the screen must keep its "last known position" honest
// (MSWindowsScreen::onMouseMove computes every relayed hardware delta against it), or the
// displacement replays onto the client's cursor as a jump on the next real motion. Inside the
// INPUT_FIRST..INPUT_LAST range ON PURPOSE: warpCursor()'s discard loop must flush a stale one
// exactly like any other queued input message, because the warp re-saves the true position
// itself. SCREEN_SAVER and DEBUG shift up one slot; every id is symbolic and in-process only
// (MSWindowsDesks derives its own ids from DESKFLOW_HOOK_LAST_MSG, so they follow).
#define DESKFLOW_MSG_INJECT_AT WM_APP + 0x0018    // x; y
#define DESKFLOW_MSG_SCREEN_SAVER WM_APP + 0x0019 // activated; <unused>
#define DESKFLOW_MSG_DEBUG WM_APP + 0x001a        // data, data
#define DESKFLOW_MSG_INPUT_FIRST DESKFLOW_MSG_KEY
#define DESKFLOW_MSG_INPUT_LAST DESKFLOW_MSG_INJECT_AT
#define DESKFLOW_HOOK_LAST_MSG DESKFLOW_MSG_DEBUG
// Merged-fork: mouseLLHook ATE a pen/touch-promoted mouse event while relaying (x; y = the
// event's stamped position). Diagnostic only -- the jump-diag ring records it as 'T' so a
// teleport investigation can SEE the suppressed echoes; no bookkeeping changes (the event was
// eaten, the parked cursor never moved). Deliberately OUTSIDE INPUT_FIRST..INPUT_LAST (it is
// not input and must not be flushed as such) and NUMBERED PAST the MSWindowsDesks block --
// Desks derives DESKFLOW_HOOK_LAST_MSG+1..+13 (0x1b..0x27), so the next free id is 0x28.
// DESKFLOW_HOOK_LAST_MSG must NOT move (the Desks ids would shift with it).
#define DESKFLOW_MSG_TOUCH_ECHO WM_APP + 0x0028 // x; y

#define DESKFLOW_HOOK_FAKE_INPUT_VIRTUAL_KEY VK_CANCEL
#define DESKFLOW_HOOK_FAKE_INPUT_SCANCODE 0

enum EHookResult
{
  kHOOK_FAILED,
  kHOOK_OKAY,
  kHOOK_OKAY_LL
};

enum EHookMode
{
  kHOOK_DISABLE,
  kHOOK_WATCH_JUMP_ZONE,
  kHOOK_RELAY_EVENTS
};

//! Loads and provides functions for the Windows hook
class MSWindowsHook
{
public:
  MSWindowsHook() = default;
  ~MSWindowsHook();

  void loadLibrary();

  int init(DWORD threadID);

  int cleanup();

  void setSides(uint32_t sides);
  uint32_t getSides();

  void setZone(int32_t x, int32_t y, int32_t w, int32_t h, int32_t jumpZoneSize);

  void setMode(EHookMode mode);

  static EHookResult install();

  static int uninstall();

  static int installScreenSaver();

  static int uninstallScreenSaver();

  //! Re-sync the hook's internal key-state shadow from the real global key state.
  /*!
  Call on a desktop switch (Ctrl+Alt+Del / UAC / lock) to clear a phantom modifier whose
  key-UP was delivered to the Winlogon secure desktop the hook never saw. Must NOT be called
  per-keystroke: while relaying, held modifiers are swallowed and GetAsyncKeyState under-reports
  them, so a per-key resync would drop a genuinely-held Shift/Ctrl.
  */
  static void resyncKeyState();
};
