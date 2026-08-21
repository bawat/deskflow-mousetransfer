/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsDesks.h"

#include "arch/Arch.h"
#include "base/IEventQueue.h"
#include "base/IJob.h"
#include "base/Log.h"
#include "base/MTWatchdog.h" // TEMPORARY MouseTransfer diag: MT_HOOKBEAT for the desk-thread pump
#include "base/TMethodJob.h"
#include "deskflow/IScreenSaver.h"
#include "deskflow/ScreenException.h"
#include "deskflow/win32/AppUtilWindows.h"
#include "mt/Lock.h"
#include "mt/Thread.h"
#include "platform/MSWindowsHook.h"
#include "platform/MSWindowsScreen.h"

#include <cstdlib>
#include <cwchar> // MouseTransfer: wcscmp for the fg-hold popup-class check (History fix)
#include <fstream>
#include <malloc.h>

// these are only defined when WINVER >= 0x0500
#if !defined(SPI_GETMOUSESPEED)
#define SPI_GETMOUSESPEED 112
#endif
#if !defined(SPI_SETMOUSESPEED)
#define SPI_SETMOUSESPEED 113
#endif
#if !defined(SPI_GETSCREENSAVERRUNNING)
#define SPI_GETSCREENSAVERRUNNING 114
#endif

#if !defined(MOUSEEVENTF_HWHEEL)
#define MOUSEEVENTF_HWHEEL 0x1000
#endif

// X button stuff
#if !defined(WM_XBUTTONDOWN)
#define WM_XBUTTONDOWN 0x020B
#define WM_XBUTTONUP 0x020C
#define WM_XBUTTONDBLCLK 0x020D
#define WM_NCXBUTTONDOWN 0x00AB
#define WM_NCXBUTTONUP 0x00AC
#define WM_NCXBUTTONDBLCLK 0x00AD
#define MOUSEEVENTF_XDOWN 0x0080
#define MOUSEEVENTF_XUP 0x0100
#define XBUTTON1 0x0001
#define XBUTTON2 0x0002
#endif
#if !defined(VK_XBUTTON1)
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#endif

// <unused>; <unused>
#define DESKFLOW_MSG_SWITCH DESKFLOW_HOOK_LAST_MSG + 1
// <unused>; <unused>
#define DESKFLOW_MSG_ENTER DESKFLOW_HOOK_LAST_MSG + 2
// <unused>; <unused>
#define DESKFLOW_MSG_LEAVE DESKFLOW_HOOK_LAST_MSG + 3
// wParam = flags, HIBYTE(lParam) = virtual key, LOBYTE(lParam) = scan code
#define DESKFLOW_MSG_FAKE_KEY DESKFLOW_HOOK_LAST_MSG + 4
// flags, XBUTTON id
#define DESKFLOW_MSG_FAKE_BUTTON DESKFLOW_HOOK_LAST_MSG + 5
// x; y
#define DESKFLOW_MSG_FAKE_MOVE DESKFLOW_HOOK_LAST_MSG + 6
// xDelta; yDelta
#define DESKFLOW_MSG_FAKE_WHEEL DESKFLOW_HOOK_LAST_MSG + 7
// POINT*; <unused>
#define DESKFLOW_MSG_CURSOR_POS DESKFLOW_HOOK_LAST_MSG + 8
// IKeyState*; <unused>
#define DESKFLOW_MSG_SYNC_KEYS DESKFLOW_HOOK_LAST_MSG + 9
// install; <unused>
#define DESKFLOW_MSG_SCREENSAVER DESKFLOW_HOOK_LAST_MSG + 10
// dx; dy
#define DESKFLOW_MSG_FAKE_REL_MOVE DESKFLOW_HOOK_LAST_MSG + 11
// enable; <unused>
#define DESKFLOW_MSG_FAKE_INPUT DESKFLOW_HOOK_LAST_MSG + 12
// MouseTransfer (RDP window handoff foreground hold): HWND to hold foreground (0 = release); <unused>
#define DESKFLOW_MSG_RDP_FG_HOLD DESKFLOW_HOOK_LAST_MSG + 13

static void send_keyboard_input(WORD wVk, WORD wScan, DWORD dwFlags)
{
  INPUT inp;
  inp.type = INPUT_KEYBOARD;
  inp.ki.wVk = (dwFlags & KEYEVENTF_UNICODE) ? 0 : wVk; // 1..254 inclusive otherwise
  inp.ki.wScan = wScan;
  inp.ki.dwFlags = dwFlags & 0xF;
  inp.ki.time = 0;
  inp.ki.dwExtraInfo = 0;
  SendInput(1, &inp, sizeof(inp));
}

static void send_mouse_input(DWORD dwFlags, DWORD dx, DWORD dy, DWORD dwData)
{
  INPUT inp;
  inp.type = INPUT_MOUSE;
  inp.mi.dwFlags = dwFlags;
  inp.mi.dx = dx;
  inp.mi.dy = dy;
  inp.mi.mouseData = dwData;
  inp.mi.time = 0;
  inp.mi.dwExtraInfo = 0;
  SendInput(1, &inp, sizeof(inp));
}

//
// MSWindowsDesks
//

// Single instance pointer for the static EVENT_SYSTEM_DESKTOPSWITCH WinEvent callback (which can't
// carry user data). Set/cleared alongside the hook in enable()/disable() — one MSWindowsDesks per
// screen.
MSWindowsDesks *MSWindowsDesks::s_instance = nullptr;

MSWindowsDesks::MSWindowsDesks(
    bool isPrimary, bool useHooks, const IScreenSaver *screensaver, IEventQueue *events, IJob *updateKeys
)
    : m_isPrimary(isPrimary),
      m_useHooks(useHooks),
      m_isOnScreen(m_isPrimary),
      m_screensaver(screensaver),
      m_deskReady(&m_mutex, false),
      m_updateKeys(updateKeys),
      m_events(events)
{

  m_cursor = createBlankCursor();
  m_deskClass = createDeskWindowClass(m_isPrimary);
  m_keyLayout = AppUtilWindows::instance().getCurrentKeyboardLayout();
  resetOptions();
}

MSWindowsDesks::~MSWindowsDesks()
{
  disable();
  destroyClass(m_deskClass);
  destroyCursor(m_cursor);
  delete m_updateKeys;
}

void MSWindowsDesks::enable()
{
  m_threadID = GetCurrentThreadId();

  // set the active desk and (re)install the hooks
  checkDesk();

  // install the desk timer.  this timer periodically checks
  // which desk is active and reinstalls the hooks as necessary.
  // we wouldn't need this if windows notified us of a desktop
  // change but as far as i can tell it doesn't.
  m_timer = m_events->newTimer(0.2, nullptr);
  m_events->addHandler(EventTypes::Timer, m_timer, [this](const auto &) { handleCheckDesk(); });

  // MouseTransfer (History context-menu fix, 2026-08-21): a SECOND, faster timer that only re-asserts
  // the fg-hold in the one case the 0.2s cadence above is too slow for — a held export's context menu
  // being stolen foreground by its own main form, which the app cancels in ~125ms. 40ms gives three
  // re-asserts inside that window. checkRdpFgHoldFast is a cheap no-op in every other state (an
  // in-memory hwnd check), so this costs ~nothing while no such steal is happening. See it for why it
  // cannot regress the working palettes.
  m_fgHoldFastTimer = m_events->newTimer(0.04, nullptr);
  m_events->addHandler(EventTypes::Timer, m_fgHoldFastTimer, [this](const auto &) { checkRdpFgHoldFast(); });

  // MouseTransfer (RDP window handoff foreground hold): resolve the signal file the wrapper
  // writes. Same convention as the server's switchreq poll — an env override, else a well-known
  // name relative to the CWD (the launcher sets the core's CWD to its bundle dir, where the
  // wrapper writes its other core-facing files: coremode, switchreq, userquit).
  if (const char *env = std::getenv("MOUSETRANSFER_RDPFGFILE"); env != nullptr && *env != '\0') {
    m_fgHoldFile = env;
  } else {
    m_fgHoldFile = "rdp-fg-hold";
  }

  // MouseTransfer (RDP adaptive cursor park): the sibling signal file, resolved the same way.
  if (const char *env = std::getenv("MOUSETRANSFER_RDPPARKFILE"); env != nullptr && *env != '\0') {
    m_rdpParkFile = env;
  } else {
    m_rdpParkFile = "rdp-park";
  }

  // MouseTransfer fix (stuck-AltGr via Ctrl+Alt+Del / UAC secure desktop): get an EVENT-DRIVEN
  // notification of desktop switches and re-sync the key state on each one. The Secure Attention
  // Sequence routes the Ctrl+Alt key-UP to the Winlogon secure desktop, which our low-level hook
  // cannot see, so the core's internal modifier state latches Ctrl+Alt (AltGr) and every relayed key
  // is then mapped through the AltGr 3rd level (accented vowels on the client). The existing
  // checkDesk poll CANNOT catch this on the elevated core: OpenInputDesktop() returns NULL on/after
  // the secure desktop, so the desktop NAME never changes and its `name != m_activeDeskName` gate
  // never fires. EVENT_SYSTEM_DESKTOPSWITCH is generated by the SYSTEM and fires independently of
  // OpenInputDesktop, so it catches exactly that round-trip. idProcess/idThread MUST be 0 (the switch
  // is system-generated, not from us); WINEVENT_OUTOFCONTEXT delivers the callback to THIS thread's
  // message pump (MSWindowsEventQueueBuffer's GetMessage loop), so no reentrancy. Primary only — the
  // stale-modifier-on-capture bug is server-side.
  if (m_isPrimary) {
    s_instance = this;
    m_desktopEventHook = SetWinEventHook(
        EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH,
        nullptr, // hmod: callback is in-process (OUTOFCONTEXT)
        &MSWindowsDesks::onDesktopSwitchEvent,
        0, 0, // idProcess=0, idThread=0: monitor system-wide desktop switches
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );
    if (m_desktopEventHook == nullptr) {
      LOG_WARN("failed to install desktop-switch WinEvent hook, err=%d (key resync falls back to "
               "seam-crossing only)",
               (int)GetLastError());
    } else {
      LOG_INFO("desktop-switch WinEvent hook installed (Ctrl+Alt+Del key resync)");
    }
  }

  updateKeys();
}

void MSWindowsDesks::disable()
{
  // remove the desktop-switch WinEvent hook (see enable())
  if (m_desktopEventHook != nullptr) {
    UnhookWinEvent(m_desktopEventHook);
    m_desktopEventHook = nullptr;
  }
  if (s_instance == this) {
    s_instance = nullptr;
  }

  // remove timer
  if (m_timer != nullptr) {
    m_events->removeHandler(EventTypes::Timer, m_timer);
    m_events->deleteTimer(m_timer);
    m_timer = nullptr;
  }

  // MouseTransfer: the fast fg-hold timer (History fix).
  if (m_fgHoldFastTimer != nullptr) {
    m_events->removeHandler(EventTypes::Timer, m_fgHoldFastTimer);
    m_events->deleteTimer(m_fgHoldFastTimer);
    m_fgHoldFastTimer = nullptr;
  }

  // destroy desks
  removeDesks();

  m_isOnScreen = m_isPrimary;
}

void MSWindowsDesks::enter()
{
  sendMessage(DESKFLOW_MSG_ENTER, 0, 0);
}

void MSWindowsDesks::leave(HKL keyLayout)
{
  sendMessage(DESKFLOW_MSG_LEAVE, (WPARAM)keyLayout, 0);
}

void MSWindowsDesks::resetOptions()
{
  m_leaveForegroundOption = false;
}

void MSWindowsDesks::setOptions(const OptionsList &options)
{
  for (uint32_t i = 0, n = (uint32_t)options.size(); i < n; i += 2) {
    if (options[i] == kOptionWin32KeepForeground) {
      m_leaveForegroundOption = (options[i + 1] != 0);
      LOG_DEBUG1("%s the foreground window", m_leaveForegroundOption ? "don\'t grab" : "grab");
    }
  }
}

void MSWindowsDesks::updateKeys()
{
  sendMessage(DESKFLOW_MSG_SYNC_KEYS, 0, 0);
}

void MSWindowsDesks::setShape(
    int32_t x, int32_t y, int32_t width, int32_t height, int32_t xCenter, int32_t yCenter, bool isMultimon
)
{
  m_x = x;
  m_y = y;
  m_w = width;
  m_h = height;
  m_xCenter = xCenter;
  m_yCenter = yCenter;
  m_multimon = isMultimon;
}

void MSWindowsDesks::installScreensaverHooks(bool install)
{
  if (m_isPrimary && m_screensaverNotify != install) {
    m_screensaverNotify = install;
    sendMessage(DESKFLOW_MSG_SCREENSAVER, install, 0);
  }
}

void MSWindowsDesks::fakeInputBegin()
{
  sendMessage(DESKFLOW_MSG_FAKE_INPUT, 1, 0);
}

void MSWindowsDesks::fakeInputEnd()
{
  sendMessage(DESKFLOW_MSG_FAKE_INPUT, 0, 0);
}

void MSWindowsDesks::getCursorPos(int32_t &x, int32_t &y) const
{
  POINT pos{0, 0};
  sendMessage(DESKFLOW_MSG_CURSOR_POS, reinterpret_cast<WPARAM>(&pos), 0);
  x = pos.x;
  y = pos.y;
}

void MSWindowsDesks::fakeKeyEvent(WORD virtualKey, WORD scanCode, DWORD flags, bool /*isAutoRepeat*/) const
{
  sendMessage(DESKFLOW_MSG_FAKE_KEY, flags, MAKELPARAM(scanCode, virtualKey));
}

void MSWindowsDesks::fakeMouseButton(ButtonID button, bool press)
{
  // the system will swap the meaning of left/right for us if
  // the user has configured a left-handed mouse but we don't
  // want it to swap since we want the handedness of the
  // server's mouse.  so pre-swap for a left-handed mouse.
  if (GetSystemMetrics(SM_SWAPBUTTON)) {
    switch (button) {
    case kButtonLeft:
      button = kButtonRight;
      break;

    case kButtonRight:
      button = kButtonLeft;
      break;
    }
  }

  // map button id to button flag and button data
  DWORD data = 0;
  DWORD flags;
  switch (button) {
  case kButtonLeft:
    flags = press ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    break;

  case kButtonMiddle:
    flags = press ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    break;

  case kButtonRight:
    flags = press ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    break;

  case kButtonExtra0:
    data = XBUTTON1;
    flags = press ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
    break;

  case kButtonExtra1:
    data = XBUTTON2;
    flags = press ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
    break;

  default:
    return;
  }

  // do it
  sendMessage(DESKFLOW_MSG_FAKE_BUTTON, flags, data);
}

void MSWindowsDesks::fakeMouseMove(int32_t x, int32_t y) const
{
  sendMessage(DESKFLOW_MSG_FAKE_MOVE, static_cast<WPARAM>(x), static_cast<LPARAM>(y));
}

void MSWindowsDesks::fakeMouseRelativeMove(int32_t dx, int32_t dy) const
{
  sendMessage(DESKFLOW_MSG_FAKE_REL_MOVE, static_cast<WPARAM>(dx), static_cast<LPARAM>(dy));
}

void MSWindowsDesks::fakeMouseWheel(int32_t xDelta, int32_t yDelta) const
{
  sendMessage(DESKFLOW_MSG_FAKE_WHEEL, xDelta, yDelta);
}

void MSWindowsDesks::sendMessage(UINT msg, WPARAM wParam, LPARAM lParam) const
{
  if (m_activeDesk != nullptr && m_activeDesk->m_window != nullptr) {
    PostThreadMessage(m_activeDesk->m_threadID, msg, wParam, lParam);
    waitForDesk();
  }
}

HCURSOR
MSWindowsDesks::createBlankCursor() const
{
  // create a transparent cursor
  int cw = GetSystemMetrics(SM_CXCURSOR);
  int ch = GetSystemMetrics(SM_CYCURSOR);
  uint8_t *cursorAND = new uint8_t[ch * ((cw + 31) >> 2)];
  uint8_t *cursorXOR = new uint8_t[ch * ((cw + 31) >> 2)];
  memset(cursorAND, 0xff, ch * ((cw + 31) >> 2));
  memset(cursorXOR, 0x00, ch * ((cw + 31) >> 2));
  HCURSOR c = CreateCursor(MSWindowsScreen::getWindowInstance(), 0, 0, cw, ch, cursorAND, cursorXOR);
  delete[] cursorXOR;
  delete[] cursorAND;
  return c;
}

void MSWindowsDesks::destroyCursor(HCURSOR cursor) const
{
  if (cursor != nullptr) {
    DestroyCursor(cursor);
  }
}

ATOM MSWindowsDesks::createDeskWindowClass(bool isPrimary) const
{
  WNDCLASSEX classInfo;
  classInfo.cbSize = sizeof(classInfo);
  classInfo.style = CS_DBLCLKS | CS_NOCLOSE;
  classInfo.lpfnWndProc = isPrimary ? &MSWindowsDesks::primaryDeskProc : &MSWindowsDesks::secondaryDeskProc;
  classInfo.cbClsExtra = 0;
  classInfo.cbWndExtra = 0;
  classInfo.hInstance = MSWindowsScreen::getWindowInstance();
  classInfo.hIcon = nullptr;
  classInfo.hCursor = m_cursor;
  classInfo.hbrBackground = nullptr;
  classInfo.lpszMenuName = nullptr;
  classInfo.lpszClassName = L"DeskflowDesk";
  classInfo.hIconSm = nullptr;
  return RegisterClassEx(&classInfo);
}

void MSWindowsDesks::destroyClass(ATOM windowClass) const
{
  if (windowClass != 0) {
    UnregisterClass(MAKEINTATOM(windowClass), MSWindowsScreen::getWindowInstance());
  }
}

HWND MSWindowsDesks::createWindow(ATOM windowClass, const wchar_t *name) const
{
  HWND window = CreateWindowEx(
      WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW, MAKEINTATOM(windowClass), name, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
      MSWindowsScreen::getWindowInstance(), nullptr
  );
  if (window == nullptr) {
    LOG_ERR("failed to create window: %d", GetLastError());
    throw ScreenOpenFailureException();
  }
  return window;
}

void MSWindowsDesks::destroyWindow(HWND hwnd) const
{
  if (hwnd != nullptr) {
    DestroyWindow(hwnd);
  }
}

LRESULT CALLBACK MSWindowsDesks::primaryDeskProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK MSWindowsDesks::secondaryDeskProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  // would like to detect any local user input and hide the hider
  // window but for now we just detect mouse motion.
  bool hide = false;
  switch (msg) {
  case WM_MOUSEMOVE:
    if (LOWORD(lParam) != 0 || HIWORD(lParam) != 0) {
      hide = true;
    }
    break;
  }

  if (hide && IsWindowVisible(hwnd)) {
    ReleaseCapture();
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
  }

  return DefWindowProc(hwnd, msg, wParam, lParam);
}

void MSWindowsDesks::deskMouseMove(int32_t x, int32_t y) const
{
  // when using absolute positioning with mouse_event(),
  // the normalized device coordinates range over only
  // the primary screen.
  int32_t w = GetSystemMetrics(SM_CXSCREEN);
  int32_t h = GetSystemMetrics(SM_CYSCREEN);
  send_mouse_input(
      MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, (DWORD)((65535.0f * x) / (w - 1) + 0.5f),
      (DWORD)((65535.0f * y) / (h - 1) + 0.5f), 0
  );
}

void MSWindowsDesks::deskMouseRelativeMove(int32_t dx, int32_t dy) const
{
  // relative moves are subject to cursor acceleration which we don't
  // want.so we disable acceleration, do the relative move, then
  // restore acceleration.  there's a slight chance we'll end up in
  // the wrong place if the user moves the cursor using this system's
  // mouse while simultaneously moving the mouse on the server
  // system.  that defeats the purpose of deskflow so we'll assume
  // that won't happen.  even if it does, the next mouse move will
  // correct the position.

  // save mouse speed & acceleration
  int oldSpeed[4];
  bool accelChanged =
      SystemParametersInfo(SPI_GETMOUSE, 0, oldSpeed, 0) && SystemParametersInfo(SPI_GETMOUSESPEED, 0, oldSpeed + 3, 0);

  // use 1:1 motion
  if (accelChanged) {
    int newSpeed[4] = {0, 0, 0, 1};
    accelChanged = SystemParametersInfo(SPI_SETMOUSE, 0, newSpeed, 0) ||
                   SystemParametersInfo(SPI_SETMOUSESPEED, 0, newSpeed + 3, 0);
  }

  // move relative to mouse position
  send_mouse_input(MOUSEEVENTF_MOVE, dx, dy, 0);

  // restore mouse speed & acceleration
  if (accelChanged) {
    SystemParametersInfo(SPI_SETMOUSE, 0, oldSpeed, 0);
    SystemParametersInfo(SPI_SETMOUSESPEED, 0, oldSpeed + 3, 0);
  }
}

/*!
 * Wraps the `ShowCursor` function and calls it repeatedly until the cursor visibility is at
 * the desired state. Windows maintains an internal counter for cursor visibility, and only
 * shows or hides the cursor when it reaches a certain threshold.
 */
void setCursorVisibility(bool visible)
{
  LOG_DEBUG("%s cursor", visible ? "showing" : "hiding");

  const int max = 10;
  int attempts = 0;
  while (attempts++ < max) {
    const auto displayCounter = ShowCursor(visible ? TRUE : FALSE);
    LOG_DEBUG1("cursor display counter: %d", displayCounter);

    if (visible) {
      if (displayCounter < 0) {
        LOG_DEBUG1("cursor still hidden, retrying, attempt: %d", attempts);
      } else {
        LOG_DEBUG1("cursor is now visible, attempts: %d", attempts);
        return;
      }
    } else {
      if (displayCounter >= 0) {
        LOG_DEBUG1("cursor still visible, retrying, attempt: %d", attempts);
      } else {
        LOG_DEBUG1("cursor is now hidden, attempts: %d", attempts);
        return;
      }
    }
  }

  LOG_ERR("unable to set cursor visibility after %d attempts", attempts);
}

void MSWindowsDesks::deskEnter(Desk *desk)
{
  if (!m_isPrimary) {
    ReleaseCapture();
  }

  setCursorVisibility(true);

  SetWindowPos(desk->m_window, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);

  // restore the foreground window
  // XXX -- this raises the window to the top of the Z-order.  we
  // want it to stay wherever it was to properly support X-mouse
  // (mouse over activation) but i've no idea how to do that.
  // the obvious workaround of using SetWindowPos() to move it back
  // after being raised doesn't work.
  DWORD thisThread = GetWindowThreadProcessId(desk->m_window, nullptr);
  DWORD thatThread = GetWindowThreadProcessId(desk->m_foregroundWindow, nullptr);
  AttachThreadInput(thatThread, thisThread, TRUE);
  SetForegroundWindow(desk->m_foregroundWindow);
  AttachThreadInput(thatThread, thisThread, FALSE);
  EnableWindow(desk->m_window, desk->m_lowLevel ? FALSE : TRUE);
  desk->m_foregroundWindow = nullptr;
}

void MSWindowsDesks::deskLeave(Desk *desk, HKL keyLayout)
{
  setCursorVisibility(false);

  if (m_isPrimary) {
    // map a window to hide the cursor and to use whatever keyboard
    // layout we choose rather than the keyboard layout of the last
    // active window.
    int x, y, w, h;
    if (desk->m_lowLevel) {
      // with a low level hook the cursor will never budge so
      // just a 1x1 window is sufficient.
      x = m_xCenter;
      y = m_yCenter;
      w = 1;
      h = 1;
    } else {
      // with regular hooks the cursor will jitter as it's moved
      // by the user then back to the center by us.  to be sure
      // we never lose it, cover all the monitors with the window.
      x = m_x;
      y = m_y;
      w = m_w;
      h = m_h;
    }
    SetWindowPos(desk->m_window, HWND_TOP, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // switch to requested keyboard layout
    ActivateKeyboardLayout(keyLayout, 0);

    // if not using low-level hooks we have to also activate the
    // window to ensure we don't lose keyboard focus.
    // FIXME -- see if this can be avoided.  if so then always
    // disable the window (see handling of DESKFLOW_MSG_SWITCH).
    if (!desk->m_lowLevel) {
      SetActiveWindow(desk->m_window);
    }

    // if using low-level hooks then disable the foreground window
    // so it can't mess up any of our keyboard events.  the console
    // program, for example, will cause characters to be reported as
    // unshifted, regardless of the shift key state.  interestingly
    // we do see the shift key go down and up.
    //
    // note that we must enable the window to activate it and we
    // need to disable the window on deskEnter.
    else {
      desk->m_foregroundWindow = getForegroundWindow();
      // MouseTransfer (RDP window handoff foreground hold): never save the held export as the
      // foreground-to-restore — it is an INVISIBLE window, and deskEnter restoring it would leave
      // the user typing into nothing when the cursor comes home. Can only happen if a leave races
      // the wrapper's own cursor-home clear of the hold file (the hold is normally released before
      // any re-leave).
      if (m_fgHoldHwnd != nullptr && desk->m_foregroundWindow == m_fgHoldHwnd) {
        desk->m_foregroundWindow = nullptr;
      }
      if (desk->m_foregroundWindow != nullptr) {
        EnableWindow(desk->m_window, TRUE);
        SetActiveWindow(desk->m_window);
        DWORD thisThread = GetWindowThreadProcessId(desk->m_window, nullptr);
        DWORD thatThread = GetWindowThreadProcessId(desk->m_foregroundWindow, nullptr);

        AttachThreadInput(thatThread, thisThread, TRUE);
        SetForegroundWindow(desk->m_window);
        AttachThreadInput(thatThread, thisThread, FALSE);
      }
    }
  } else {
    // move hider window under the cursor center, raise, and show it
    SetWindowPos(desk->m_window, HWND_TOP, m_xCenter, m_yCenter, 1, 1, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // watch for mouse motion.  if we see any then we hide the
    // hider window so the user can use the physically attached
    // mouse if desired.  we'd rather not capture the mouse but
    // we aren't notified when the mouse leaves our window.
    SetCapture(desk->m_window);

    // windows can take a while to hide the cursor, so wait a few milliseconds to ensure the cursor
    // is hidden before centering. this doesn't seem to affect the fluidity of the transition.
    // without this, the cursor appears to flicker in the center of the screen which is annoying.
    // a slightly more elegant but complex solution could be to use a timed event.
    // 30 ms seems to work well enough without making the transition feel janky; a lower number
    // would be better but 10 ms doesn't seem to be quite long enough, as we get noticeable flicker.
    // this is largely a balance and out of our control, since windows can be unpredictable...
    // maybe another approach would be to repeatedly check the cursor visibility until it is hidden.
    LOG_DEBUG1("centering cursor on leave: %+d,%+d", m_xCenter, m_yCenter);
    ARCH->sleep(0.03);
    deskMouseMove(m_xCenter, m_yCenter);
  }
}

void MSWindowsDesks::deskThread(const void *vdesk)
{
  MSG msg;

  // use given desktop for this thread
  Desk *desk = const_cast<Desk *>(static_cast<const Desk *>(vdesk));
  desk->m_threadID = GetCurrentThreadId();
  desk->m_window = nullptr;
  desk->m_foregroundWindow = nullptr;
  if (desk->m_desk != nullptr && SetThreadDesktop(desk->m_desk) != 0) {
    // create a message queue
    PeekMessage(&msg, nullptr, 0, 0, PM_NOREMOVE);

    // create a window.  we use this window to hide the cursor.
    try {
      desk->m_window = createWindow(m_deskClass, L"DeskflowDesk");
      LOG_DEBUG("desk %ls window is 0x%08x", desk->m_name.c_str(), desk->m_window);
    } catch (...) {
      // ignore
      LOG_DEBUG("can't create desk window for %ls", desk->m_name.c_str());
    }
  }

  // tell main thread that we're ready
  {
    Lock lock(&m_mutex);
    m_deskReady = true;
    m_deskReady.broadcast();
  }

  while (GetMessage(&msg, nullptr, 0, 0)) {
    // TEMPORARY MouseTransfer diag: DESK-thread heartbeat -- this loop pumps the desk window and
    // services the LL hooks. Kept in the separate hook heartbeat (GetMessage legitimately blocks here
    // while idle, so this must NOT feed the MAIN-thread stall watchdog).
    MT_HOOKBEAT("desk:pump");
    switch (msg.message) {
    default:
      TranslateMessage(&msg);
      DispatchMessage(&msg);
      continue;

    case DESKFLOW_MSG_SWITCH:
      if (m_useHooks) {
        MSWindowsHook::uninstall();
        if (m_screensaverNotify) {
          MSWindowsHook::uninstallScreenSaver();
          MSWindowsHook::installScreenSaver();
        }
        switch (MSWindowsHook::install()) {
        case kHOOK_FAILED:
          // we won't work on this desk
          desk->m_lowLevel = false;
          break;

        case kHOOK_OKAY:
          desk->m_lowLevel = false;
          break;

        case kHOOK_OKAY_LL:
          desk->m_lowLevel = true;
          break;
        }

        // a window on the primary screen with low-level hooks
        // should never activate.
        if (desk->m_window)
          EnableWindow(desk->m_window, desk->m_lowLevel ? FALSE : TRUE);
      }
      break;

    case DESKFLOW_MSG_ENTER:
      m_isOnScreen = true;
      deskEnter(desk);
      break;

    case DESKFLOW_MSG_LEAVE:
      m_isOnScreen = false;
      m_keyLayout = (HKL)msg.wParam;
      deskLeave(desk, m_keyLayout);
      break;

    case DESKFLOW_MSG_FAKE_KEY:
      // Note, this is intended to be HI/LOWORD and not HI/LOBYTE
      send_keyboard_input(HIWORD(msg.lParam), LOWORD(msg.lParam), (DWORD)msg.wParam);
      break;

    case DESKFLOW_MSG_FAKE_BUTTON:
      if (msg.wParam != 0) {
        send_mouse_input((DWORD)msg.wParam, 0, 0, (DWORD)msg.lParam);
      }
      break;

    case DESKFLOW_MSG_FAKE_MOVE:
      deskMouseMove(static_cast<int32_t>(msg.wParam), static_cast<int32_t>(msg.lParam));
      break;

    case DESKFLOW_MSG_FAKE_REL_MOVE:
      deskMouseRelativeMove(static_cast<int32_t>(msg.wParam), static_cast<int32_t>(msg.lParam));
      break;

    case DESKFLOW_MSG_FAKE_WHEEL:
      // XXX -- add support for x-axis scrolling
      if (msg.lParam != 0) {
        send_mouse_input(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)msg.lParam);
      }
      if (msg.wParam != 0) {
        send_mouse_input(MOUSEEVENTF_HWHEEL, 0, 0, (DWORD)msg.wParam);
      }
      break;

    case DESKFLOW_MSG_CURSOR_POS: {
      POINT *pos = reinterpret_cast<POINT *>(msg.wParam);
      if (!GetCursorPos(pos)) {
        pos->x = m_xCenter;
        pos->y = m_yCenter;
      }
      break;
    }

    case DESKFLOW_MSG_SYNC_KEYS:
      m_updateKeys->run();
      break;

    case DESKFLOW_MSG_SCREENSAVER:
      if (m_useHooks) {
        if (msg.wParam != 0) {
          MSWindowsHook::installScreenSaver();
        } else {
          MSWindowsHook::uninstallScreenSaver();
        }
      }
      break;

    case DESKFLOW_MSG_FAKE_INPUT:
      send_keyboard_input(
          DESKFLOW_HOOK_FAKE_INPUT_VIRTUAL_KEY, DESKFLOW_HOOK_FAKE_INPUT_SCANCODE, msg.wParam ? 0 : KEYEVENTF_KEYUP
      );
      break;

    case DESKFLOW_MSG_RDP_FG_HOLD:
      deskRdpFgHold(desk, reinterpret_cast<HWND>(msg.wParam));
      break;
    }

    // notify that message was processed
    Lock lock(&m_mutex);
    m_deskReady = true;
    m_deskReady.broadcast();
  }

  // clean up
  deskEnter(desk);
  if (desk->m_window != nullptr) {
    DestroyWindow(desk->m_window);
  }
  if (desk->m_desk != nullptr) {
    closeDesktop(desk->m_desk);
  }
}

MSWindowsDesks::Desk *MSWindowsDesks::addDesk(const std::wstring &name, HDESK hdesk)
{
  Desk *desk = new Desk;
  desk->m_name = name;
  desk->m_desk = hdesk;
  desk->m_targetID = GetCurrentThreadId();
  desk->m_thread = new Thread(new TMethodJob<MSWindowsDesks>(this, &MSWindowsDesks::deskThread, desk));
  waitForDesk();
  m_desks.insert(std::make_pair(name, desk));
  return desk;
}

void MSWindowsDesks::removeDesks()
{
  for (Desks::iterator index = m_desks.begin(); index != m_desks.end(); ++index) {
    Desk *desk = index->second;
    PostThreadMessage(desk->m_threadID, WM_QUIT, 0, 0);
    desk->m_thread->wait();
    delete desk->m_thread;
    delete desk;
  }
  m_desks.clear();
  m_activeDesk = nullptr;
  m_activeDeskName = L"";
}

void MSWindowsDesks::checkDesk()
{
  // get current desktop.  if we already know about it then return.
  Desk *desk;
  HDESK hdesk = openInputDesktop();
  std::wstring name = getDesktopName(hdesk);
  Desks::const_iterator index = m_desks.find(name);
  if (index == m_desks.end()) {
    desk = addDesk(name, hdesk);
    // hold on to hdesk until thread exits so the desk can't
    // be removed by the system
  } else {
    closeDesktop(hdesk);
    desk = index->second;
  }

  // if active desktop changed then tell the old and new desk threads
  // about the change.  don't switch desktops when the screensaver is
  // active becaue we'd most likely switch to the screensaver desktop
  // which would have the side effect of forcing the screensaver to
  // stop.
  if (name != m_activeDeskName && !m_screensaver->isActive()) {
    // show cursor on previous desk
    bool wasOnScreen = m_isOnScreen;
    if (!wasOnScreen) {
      sendMessage(DESKFLOW_MSG_ENTER, 0, 0);
    }

    // always sync keys when switching desks to ensure keyboard modifier
    // states are correct.
    LOG_DEBUG("switched to desk \"%ls\"", name.c_str());
    bool syncKeys = false;
    if (isDeskAccessible(desk)) {
      LOG_DEBUG("desktop is accessible - syncing keyboard state after desk switch");
      syncKeys = true;
    } else {
      LOG_DEBUG("desktop is inaccessible");
    }

    // switch desk
    m_activeDesk = desk;
    m_activeDeskName = name;
    sendMessage(DESKFLOW_MSG_SWITCH, 0, 0);

    // hide cursor on new desk
    if (!wasOnScreen) {
      sendMessage(DESKFLOW_MSG_LEAVE, (WPARAM)m_keyLayout, 0);
    }

    // update keys if necessary
    if (syncKeys) {
      updateKeys();
    }
  } else if (name != m_activeDeskName) {
    // screen saver might have started
    PostThreadMessage(m_threadID, DESKFLOW_MSG_SCREEN_SAVER, TRUE, 0);
  }
}

bool MSWindowsDesks::isDeskAccessible(const Desk *desk) const
{
  return (desk != nullptr && desk->m_desk != nullptr);
}

void MSWindowsDesks::waitForDesk() const
{
  MSWindowsDesks *self = const_cast<MSWindowsDesks *>(this);

  Lock lock(&m_mutex);
  while (!(bool)m_deskReady) {
    m_deskReady.wait();
  }
  self->m_deskReady = false;
}

void MSWindowsDesks::handleCheckDesk()
{
  checkDesk();

  // also check if screen saver is running if on a modern OS and
  // this is the primary screen.
  if (m_isPrimary) {
    BOOL running;
    SystemParametersInfo(SPI_GETSCREENSAVERRUNNING, 0, &running, FALSE);
    PostThreadMessage(m_threadID, DESKFLOW_MSG_SCREEN_SAVER, running, 0);
  }

  // MouseTransfer (RDP window handoff foreground hold) — same 0.2s cadence, negligible cost
  // beside checkDesk()'s OpenInputDesktop.
  checkRdpFgHold();

  // MouseTransfer (RDP adaptive cursor park) — same cadence, same one-file-read cost.
  checkRdpPark();
}

// MouseTransfer: how long a window gets to answer a liveness ping before the fg-hold gives up on it
// for this tick.
//
// SOURCED FROM THE HOLD'S OWN CADENCE, not picked: checkRdpFgHold re-asserts every 0.2s, so a
// SKIPPED tick costs nothing — the next one tries again 200ms later. 50ms therefore bounds the worst
// case at a quarter of one re-assert interval while being an age for a healthy window to answer a
// WM_NULL (which does no work at all; it only has to reach the front of a pumping message queue).
static const UINT kRdpFgHoldProbeMs = 50;

// MouseTransfer: is this window's thread PUMPING MESSAGES right now?
//
// 🔴 WHY THIS EXISTS — the 2026-08-21 `.12` incident. The fg-hold's enforcement is a
// sendMessage(DESKFLOW_MSG_RDP_FG_HOLD) from the MAIN EVENT-QUEUE THREAD, and sendMessage is
// PostThreadMessage + waitForDesk(), whose CondVar::wait(-1) is UNBOUNDED. The desk thread then runs
// AttachThreadInput + SetForegroundWindow against the target — both of which serialise against the
// TARGET's input queue. So a busy or modal target does not merely delay the hold: it blocks the desk
// thread, which blocks the main thread inside waitForDesk, which stops the event loop.
//
// That is not a cosmetic stall. The main thread is where ClientProxy's keepalives are serviced, and
// the protocol declares a peer dead after kKeepAliveRate (3.0s) x kKeepAlivesUntilDeath (3.0) = 9s
// of silence. Measured live: ~1s after the wrapper pointed the hold at a busy VLC window during a
// stage swap, this core went silent for ~7 SECONDS and the peer machine declared "server is dead".
//
// WM_NULL is the right probe: it carries NO POINTER, so UIPI permits it across integrity levels
// (a bare WM_NULL crosses where a buffer-carrying message does not), and the target does no work to
// answer it — the reply proves only that its message loop is running, which is exactly the
// precondition the AttachThreadInput/SetForegroundWindow pair needs. SMTO_ABORTIFHUNG makes Windows
// answer immediately for a window already known to be hung instead of waiting out the timeout.
//
// A false answer SKIPS one re-assert. It never abandons the hold: m_fgHoldActive stays set, so the
// release path still runs, and the next tick re-tries. The failure mode this converts is
// "the whole mesh loses its server for 7 seconds" into "the foreground steal is 200ms late".
static bool rdpFgHoldWindowResponsive(HWND hwnd)
{
  if (hwnd == nullptr || !IsWindow(hwnd)) {
    return false;
  }
  DWORD_PTR result = 0;
  return SendMessageTimeout(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, kRdpFgHoldProbeMs, &result) != 0;
}

// MouseTransfer (History context-menu fix, 2026-08-21): is this window a TRANSIENT POPUP — a context
// menu, a drop-down, a flyout — as opposed to a persistent top-level app window (the application's
// main form)?
//
// This is the ONE bit that separates the History failure from the palettes that already work. When a
// palette's context menu opens and correctly HOLDS foreground, GetForegroundWindow() IS the menu — a
// transient popup. When HISTORY's context menu opens, the app's MAIN FORM (a captioned, overlapped
// window) steals foreground from it, and the menu is cancelled ~125ms later. So the fg-hold must keep
// asserting the menu ONLY when the same-process foreground is NOT itself a popup — i.e. a real window
// grabbed it. A #32768 menu is a popup by class; every other borderless WS_POPUP (WinForms
// ToolStripDropDown, custom flyout) is one by style; a WS_CAPTION overlapped window (the main form) is
// not.
static bool rdpFgHoldIsTransientPopup(HWND hwnd)
{
  if (hwnd == nullptr || !IsWindow(hwnd)) {
    return false;
  }
  constexpr int kClsLen = 64;
  wchar_t cls[kClsLen] = {0};
  if (GetClassNameW(hwnd, cls, kClsLen) > 0 && wcscmp(cls, L"#32768") == 0) {
    return true; // the standard context-menu class
  }
  const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  return (style & static_cast<LONG_PTR>(WS_POPUP)) != 0 && (style & static_cast<LONG_PTR>(WS_CAPTION)) != WS_CAPTION;
}

// MouseTransfer (History context-menu fix): the pure gate deciding whether the fg-hold must RE-ASSERT
// the held export even though a SAME-PROCESS window currently holds foreground.
//
// This mirrors, byte-for-byte, the Go executable specification + unit test in the wrapper
// (cmd/rdpfghold.go rdpFgHoldAssertOverSameProcess / rdpfghold_test.go) — kept in Go so the decision
// is unit-tested; kept identical here because this is where the foreground state actually lives.
//
//   - ownedPopupUp: the held export has an OWNED popup up right now (GetLastActivePopup(want) != want
//     — a context menu / dialog it raised). This is TRUE for History's #32768 menu (owned by History),
//     the case that must be rescued.
//   - fgIsTransientPopup: the same-process window holding foreground is itself a menu/popup.
//
// Assert ONLY when a menu is up AND a NON-popup same-process window stole its foreground — the History
// failure and nothing else. When the foreground IS the popup (a palette's menu holding focus) or no
// owned popup is up (a WinForms invisible-owner menu — active == want), leave it: identical to the
// existing same-process back-off. That is the whole regression guard.
static bool rdpFgHoldAssertOverSameProcess(bool ownedPopupUp, bool fgIsTransientPopup)
{
  return ownedPopupUp && !fgIsTransientPopup;
}

// MouseTransfer (RDP window handoff foreground hold), the event-thread half.
//
// WHY: the wrapper exports a window over RDP and delivers the viewer's typing as tagged
// SendInput — real input, which lands on the FOREGROUND thread's focus window. While the shared
// cursor is away this core's own deskLeave has made the DeskflowDesk hider the foreground window,
// and the wrapper (non-elevated) cannot take foreground back from a LocalSystem+UIAccess-owned
// window (UIPI refuses the steal — measured live, owner round 10N/10P: "typing dies"). Only THIS
// process can reliably reassign it, and while the hider is foreground it is trivially allowed to
// (the foreground process may always give foreground away).
//
// CONTRACT: the wrapper writes "<hwnd> <pid>" (both decimal; pid = the wrapper's own) to the
// signal file while a viewer is interacting with that export, and deletes the file when the
// episode ends. The hold is honoured only while (a) the cursor is away (m_isOnScreen false — the
// wrapper's own cursor-home gate clears the file first, this is the backstop), (b) the named
// window is alive, and (c) the WRITER is alive — a wrapper that crashed mid-episode must not
// leave its export pinned foreground (PID reuse could defeat (c) in principle, but the hold also
// requires (a) and (b), and the wrapper clears the file in every orderly path).
//
// The re-assert is the point: this runs every 0.2s and re-takes the foreground whenever something
// else has it, which is what makes the hold a HOLD rather than deskLeave's one-shot steal.
void MSWindowsDesks::checkRdpFgHold()
{
  HWND want = nullptr;
  if (!m_isOnScreen && !m_fgHoldFile.empty()) {
    std::ifstream in(m_fgHoldFile);
    unsigned long long hwndVal = 0;
    unsigned long long pidVal = 0;
    if (in && (in >> hwndVal >> pidVal) && hwndVal != 0 && pidVal != 0) {
      HWND hwnd = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(hwndVal));
      if (IsWindow(hwnd)) {
        // Writer liveness: SYNCHRONIZE is enough to wait, and a signaled process object means
        // the wrapper is gone. One OpenProcess per tick, only while a hold file exists.
        if (HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pidVal))) {
          if (WaitForSingleObject(proc, 0) == WAIT_TIMEOUT) {
            want = hwnd;
          }
          CloseHandle(proc);
        }
      }
    }
  }

  if (want != nullptr) {
    // MouseTransfer: follow a modal OWNED dialog. If the held export window (an app like Notepad)
    // has an active owned pop-up -- a Save As dialog and the like -- THAT is the window Windows
    // treats as foreground, and it is shown on the far side as its own crop. Holding the OWNER would
    // fight the dialog for foreground on every 0.2s tick, so the dialog can never keep focus
    // (owner-reported: "Save As acquires focus for a second, then Notepad steals it back").
    // GetLastActivePopup returns the dialog when one is up, else the window itself, so the hold
    // tracks whichever window the viewer's input is actually meant to reach.
    HWND active = want;
    bool ownedPopupUp = false;
    if (HWND pop = GetLastActivePopup(want); pop != nullptr && pop != want && IsWindow(pop)) {
      active = pop;
      ownedPopupUp = true; // a context menu / dialog the held export raised — see the History fix below
    }
    m_fgHoldActive = true;
    m_fgHoldWant = want; // publish for the fast timer (History fix)
    // Only bother the desk thread when the hold is not already satisfied.
    //
    // MouseTransfer (PDN menus, 2026-08-16): SAME-PROCESS foreground SATISFIES the hold. The
    // GetLastActivePopup follow above only sees popups OWNED by the held window — but WinForms
    // parks its menu drop-downs under invisible helper owners (Paint.NET's File/Edit menus and
    // palette-toggle popups measure owner = a 16x16 invisible window, never the main form), so the
    // follow could not see them, the 0.2s re-assert snatched activation back from the just-opened
    // drop-down, and the app closed it on deactivation (owner: the menu "disappears in about .5 of
    // a second"). If the application handed foreground to ANOTHER OF ITS OWN windows, that is the
    // window the viewer's input is meant to reach — the same rationale the owned-dialog follow
    // already encodes, minus the assumption that ownership links the two. A different process in
    // the foreground is still stolen from, which is the hold doing its job.
    HWND fgNow = GetForegroundWindow();
    bool sameProcess = false;
    if (fgNow != nullptr && fgNow != active) {
      DWORD fgPid = 0;
      DWORD wantPid = 0;
      GetWindowThreadProcessId(fgNow, &fgPid);
      GetWindowThreadProcessId(want, &wantPid);
      sameProcess = (fgPid != 0 && fgPid == wantPid);
    }
    // MouseTransfer (History context-menu fix, 2026-08-21): the same-process back-off above is RIGHT
    // when the app handed foreground to its own MENU (a palette's context menu holding focus), and
    // WRONG when the app's MAIN FORM stole foreground from a menu that should have it (History — its
    // #32768 menu is owned by History, so ownedPopupUp is true, yet the main form grabs foreground and
    // the app cancels the menu ~125ms later). rdpFgHoldAssertOverSameProcess isolates exactly that:
    // assert over the same-process foreground ONLY when a menu is up AND the stealer is not itself a
    // popup. A palette whose menu holds foreground has fgNow == active (satisfied, never reaches here);
    // a WinForms invisible-owner menu has ownedPopupUp == false — both keep the identical old path.
    const bool assertOverSame = rdpFgHoldAssertOverSameProcess(ownedPopupUp, rdpFgHoldIsTransientPopup(fgNow));
    if (fgNow != active && (!sameProcess || assertOverSame)) {
      // 🔴 DO NOT SEND INTO A WINDOW THAT IS NOT PUMPING. sendMessage blocks this — the MAIN
      // EVENT-QUEUE — thread on an unbounded waitForDesk while the desk thread serialises against
      // the target's input queue, so a modal or busy target stalls the whole core and the mesh
      // declares this server dead after 9s. See rdpFgHoldWindowResponsive for the incident.
      // Skipping costs one 0.2s re-assert; the hold stays armed and the next tick retries.
      if (rdpFgHoldWindowResponsive(active)) {
        sendMessage(DESKFLOW_MSG_RDP_FG_HOLD, reinterpret_cast<WPARAM>(active), 0);
      } else {
        LOG_DEBUG("rdp fg-hold: window 0x%08x is not pumping messages — skipping this re-assert "
                  "rather than blocking the event loop on it",
                  active);
      }
    }
  } else if (m_fgHoldActive) {
    m_fgHoldActive = false;
    m_fgHoldWant = nullptr; // the hold ended — the fast timer goes idle (History fix)
    sendMessage(DESKFLOW_MSG_RDP_FG_HOLD, 0, 0);
  }
}

// MouseTransfer (History context-menu fix, 2026-08-21): the sub-125ms fg-hold re-assert.
//
// WHY A SECOND TIMER. checkRdpFgHold re-asserts every 0.2s. Paint.NET cancels History's just-opened
// context menu ~125ms after History loses foreground, so a 0.2s cadence can miss it entirely (the menu
// is gone before the next tick). The number is SOURCED from the failure: the menu dies in ~125ms, so
// three re-asserts inside that window (40ms) makes History's re-activation land while the menu's modal
// loop is still running, which keeps the menu up.
//
// WHY IT CANNOT REGRESS THE WORKING PALETTES. It does work in exactly ONE state — the same one
// rdpFgHoldAssertOverSameProcess names in checkRdpFgHold: the held export has an OWNED popup up (a
// context menu) whose foreground a SAME-PROCESS NON-POPUP window (the main form) has stolen. In every
// other state it returns after a couple of in-memory reads: no hold (m_fgHoldWant null), no owned
// popup up, the popup still holds foreground (fgNow == active), or the foreground is itself a popup. A
// palette whose menu holds focus has fgNow == active and never gets past that guard.
//
// COMPOSES WITH THE ANTI-FREEZE PROBE. Like the 0.2s path it will not sendMessage into a window that
// is not pumping — rdpFgHoldWindowResponsive gates every re-assert, so a faster cadence cannot
// reintroduce the e487043ff unbounded-wait mesh freeze. A hung target simply skips (bounded at the
// 50ms probe), and the next tick retries.
void MSWindowsDesks::checkRdpFgHoldFast()
{
  HWND want = m_fgHoldWant;
  if (want == nullptr || !IsWindow(want)) {
    return; // no hold in force (the 0.2s tick publishes/clears this)
  }
  // An owned popup up on the held export? (History's #32768 menu is owned by History.) Only then can
  // this be the menu-steal we rescue; a hold with no popup is the 0.2s path's business.
  HWND active = GetLastActivePopup(want);
  if (active == nullptr || active == want || !IsWindow(active)) {
    return;
  }
  HWND fgNow = GetForegroundWindow();
  if (fgNow == active || fgNow == nullptr) {
    return; // the menu already holds foreground — nothing to rescue
  }
  DWORD fgPid = 0;
  DWORD wantPid = 0;
  GetWindowThreadProcessId(fgNow, &fgPid);
  GetWindowThreadProcessId(want, &wantPid);
  const bool sameProcess = (fgPid != 0 && fgPid == wantPid);
  if (!sameProcess) {
    return; // a different process has it — the 0.2s path already steals from that, no rush
  }
  if (!rdpFgHoldAssertOverSameProcess(true /*ownedPopupUp*/, rdpFgHoldIsTransientPopup(fgNow))) {
    return; // the same-process foreground IS a popup (a palette's menu) — leave it, identical to 0.2s
  }
  // History: a menu is up but the main form has its foreground. Re-assert — probe-gated, exactly as
  // the 0.2s path, so a non-pumping target skips instead of stalling the event loop.
  if (rdpFgHoldWindowResponsive(active)) {
    sendMessage(DESKFLOW_MSG_RDP_FG_HOLD, reinterpret_cast<WPARAM>(active), 0);
  }
}

// MouseTransfer (RDP adaptive cursor park). Read the point the wrapper wants the away cursor to
// rest at (off the window it is streaming over RDP) and publish it for MSWindowsScreen's warp path.
// No foreground/desk work — this only stores a value, so unlike checkRdpFgHold there is no
// desk-thread half. Honoured only while the cursor is AWAY (m_isOnScreen false) and the writer is
// alive; otherwise "no park" is published and the warp falls back to the screen centre. The file is
// "<x> <y> <pid>" (decimal, x/y signed virtual-screen coords), the same shape and liveness rule as
// rdp-fg-hold. Running purely on the event thread; getRdpPark on the hook thread reads the atomic.
void MSWindowsDesks::checkRdpPark()
{
  uint64_t packed = UINT64_MAX; // no park
  if (!m_isOnScreen && !m_rdpParkFile.empty()) {
    std::ifstream in(m_rdpParkFile);
    long long x = 0, y = 0;
    unsigned long long pidVal = 0;
    if (in && (in >> x >> y >> pidVal) && pidVal != 0) {
      // Writer liveness, exactly as checkRdpFgHold: a signaled process object means the wrapper is
      // gone and its park is stale — fall back to the centre. One OpenProcess per tick, only while a
      // park file exists.
      if (HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pidVal))) {
        if (WaitForSingleObject(proc, 0) == WAIT_TIMEOUT) {
          packed = (static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(x))) << 32) |
                   static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(y)));
        }
        CloseHandle(proc);
      }
    }
  }
  m_rdpPark.store(packed, std::memory_order_relaxed);
}

// MouseTransfer (RDP adaptive cursor park): hand the published park to the warp path. Returns false
// (leaving x/y untouched) when no park is active, so the caller keeps the screen centre.
bool MSWindowsDesks::getRdpPark(int32_t &x, int32_t &y) const
{
  const uint64_t v = m_rdpPark.load(std::memory_order_relaxed);
  if (v == UINT64_MAX) {
    return false;
  }
  x = static_cast<int32_t>(static_cast<uint32_t>(v >> 32));
  y = static_cast<int32_t>(static_cast<uint32_t>(v & 0xFFFFFFFFULL));
  return true;
}

// MouseTransfer (RDP window handoff foreground hold), the desk-thread half. Mirrors deskLeave's
// proven AttachThreadInput + SetForegroundWindow recipe — the desk thread is where that recipe
// already runs, so the hold has exactly the same thread semantics as the steal it overrides.
void MSWindowsDesks::deskRdpFgHold(Desk *desk, HWND hwnd)
{
  if (hwnd != nullptr) {
    if (!IsWindow(hwnd)) {
      return;
    }
    if (m_fgHoldHwnd != hwnd) {
      LOG_DEBUG("rdp fg-hold: holding window 0x%08x foreground for viewer input", hwnd);
    }
    m_fgHoldHwnd = hwnd;
    HWND fg = GetForegroundWindow();
    if (fg == hwnd) {
      return;
    }
    DWORD thisThread =
        (desk->m_window != nullptr) ? GetWindowThreadProcessId(desk->m_window, nullptr) : GetCurrentThreadId();
    DWORD thatThread = (fg != nullptr) ? GetWindowThreadProcessId(fg, nullptr) : 0;
    if (thatThread != 0 && thatThread != thisThread) {
      AttachThreadInput(thatThread, thisThread, TRUE);
    }
    SetForegroundWindow(hwnd);
    if (thatThread != 0 && thatThread != thisThread) {
      AttachThreadInput(thatThread, thisThread, FALSE);
    }
  } else {
    if (m_fgHoldHwnd == nullptr) {
      return;
    }
    LOG_DEBUG("rdp fg-hold: released");
    m_fgHoldHwnd = nullptr;
    // Put the hider back in charge if we are still away and deskLeave would have held it —
    // primary + low-level hooks is the one case deskLeave grabs foreground (so relayed keyboard
    // state isn't mangled by whatever app is foreground; see deskLeave's own comment). The
    // pre-leave foreground saved in desk->m_foregroundWindow is deliberately untouched: deskEnter
    // still restores the window the USER had foreground before the leave.
    if (!m_isOnScreen && m_isPrimary && desk->m_lowLevel && desk->m_window != nullptr) {
      EnableWindow(desk->m_window, TRUE);
      SetActiveWindow(desk->m_window);
      HWND fg = GetForegroundWindow();
      DWORD thisThread = GetWindowThreadProcessId(desk->m_window, nullptr);
      DWORD thatThread = (fg != nullptr) ? GetWindowThreadProcessId(fg, nullptr) : 0;
      // 🔴 THE SAME HAZARD ON THE WAY OUT. This runs on the desk thread, but the MAIN thread is
      // sitting in waitForDesk waiting for it, so an AttachThreadInput against a HUNG foreground
      // window stalls the event loop exactly as the hold path does — and the release is the tick
      // that runs when a handoff ends, i.e. precisely when the app may be busy tearing something
      // down. If the current foreground is not pumping, restore the hider WITHOUT attaching:
      // SetActiveWindow above has already done the part that does not need the target's queue, and
      // deskEnter still restores the user's own pre-leave foreground.
      const bool attach = thatThread != 0 && thatThread != thisThread && rdpFgHoldWindowResponsive(fg);
      if (attach) {
        AttachThreadInput(thatThread, thisThread, TRUE);
      }
      SetForegroundWindow(desk->m_window);
      if (attach) {
        AttachThreadInput(thatThread, thisThread, FALSE);
      }
    }
  }
}

void CALLBACK MSWindowsDesks::onDesktopSwitchEvent(
    HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD
)
{
  // The active desktop switched (e.g. to/from the Winlogon secure desktop after Ctrl+Alt+Del, a UAC
  // prompt, lock, or screensaver). Re-sync the key state so any modifier whose key-UP was routed to
  // a desktop we can't hook (latching Ctrl+Alt / AltGr) is cleared from OS ground truth. Runs on the
  // registering (main) thread via OUTOFCONTEXT delivery; updateKeys() just posts DESKFLOW_MSG_SYNC_KEYS
  // to the active desk thread (the same path checkDesk uses), so this is non-blocking and re-entrant-safe.
  if (s_instance != nullptr) {
    LOG_INFO("desktop switch — resyncing key state");
    s_instance->updateKeys();
    // Also re-sync the low-level keyboard hook's OWN g_keyState shadow (separate from the m_keys/
    // m_mask path updateKeys() rebuilds). That shadow drives the AltGr ToUnicode translation of
    // RELAYED keys; a Ctrl+Alt+Del leaves its Ctrl/Alt latched (the key-UP went to the secure
    // desktop). Doing it here, on the switch, is the safe moment — no key is being swallowed, so
    // GetAsyncKeyState reads true idle state (a per-keystroke resync would drop a held modifier).
    MSWindowsHook::resyncKeyState();
  }
}

HDESK
MSWindowsDesks::openInputDesktop()
{
  return OpenInputDesktop(DF_ALLOWOTHERACCOUNTHOOK, TRUE, DESKTOP_CREATEWINDOW | DESKTOP_HOOKCONTROL | GENERIC_WRITE);
}

void MSWindowsDesks::closeDesktop(HDESK desk)
{
  if (desk != nullptr) {
    CloseDesktop(desk);
  }
}

std::wstring MSWindowsDesks::getDesktopName(HDESK desk)
{
  if (desk == nullptr) {
    return std::wstring();
  } else {
    DWORD size;
    GetUserObjectInformation(desk, UOI_NAME, nullptr, 0, &size);
    TCHAR *name = (TCHAR *)alloca(size + sizeof(TCHAR));
    GetUserObjectInformation(desk, UOI_NAME, name, size, &size);
    std::wstring result(name);
    return result;
  }
}

HWND MSWindowsDesks::getForegroundWindow() const
{
  // Ideally we'd return nullptr as much as possible, only returning
  // the actual foreground window when we know it's going to mess
  // up our keyboard input.  For now we'll just let the user
  // decide.
  if (m_leaveForegroundOption) {
    return nullptr;
  }
  return GetForegroundWindow();
}
