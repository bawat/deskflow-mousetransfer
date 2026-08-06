/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/Event.h"
#include "base/NetworkProtocol.h"
#include "base/Stopwatch.h"
#include "deskflow/Clipboard.h"
#include "deskflow/ClipboardLane.h"
#include "deskflow/ClipboardTypes.h"
#include "deskflow/KeyTypes.h"
#include "deskflow/MouseTypes.h"
#include "server/Config.h"

#include <climits>
#include <map>
#include <memory>
#include <set>
#include <vector>

class BaseClientProxy;
class EventQueueTimer;
class PrimaryClient;
class InputFilter;
namespace deskflow {
class Screen;
}
class IEventQueue;
class Thread;
class ClientListener;

//! Deskflow server
/*!
This class implements the top-level server algorithms for deskflow.
*/
class Server
{
  using ServerConfig = deskflow::server::Config;

public:
  //! Lock cursor to screen data
  class LockCursorToScreenInfo : public EventData
  {
  public:
    enum State
    {
      kOff,
      kOn,
      kToggle
    };

    explicit LockCursorToScreenInfo(State state = kToggle) : m_state(state)
    {
      // do nothing
    }
    ~LockCursorToScreenInfo() override = default; // do nothing

  public:
    State m_state;
  };

  //! Switch to screen data
  class SwitchToScreenInfo : public EventData
  {
  public:
    explicit SwitchToScreenInfo(const std::string &screen) : m_screen(screen)
    {
      // do nothing
    }
    ~SwitchToScreenInfo() override = default; // do nothing

  public:
    std::string m_screen;
  };

  //! Switch in direction data
  class SwitchInDirectionInfo : public EventData
  {
  public:
    explicit SwitchInDirectionInfo(Direction direction) : m_direction(direction)
    {
      // do nothing
    }
    ~SwitchInDirectionInfo() override = default; // do nothing

  public:
    Direction m_direction;
  };

  //! Screen connected data
  class ScreenConnectedInfo
  {
  public:
    explicit ScreenConnectedInfo(std::string screen) : m_screen(screen)
    {
      // do nothing
    }

  public:
    std::string m_screen; // was char[1]
  };

  //! Keyboard broadcast data
  class KeyboardBroadcastInfo : public EventData
  {
  public:
    enum State
    {
      kOff,
      kOn,
      kToggle
    };

    explicit KeyboardBroadcastInfo(State state = kToggle) : m_state(state)
    {
      // do nothing
    }
    KeyboardBroadcastInfo(State state, const std::string &screens) : m_state(state), m_screens(screens)
    {
      // do nothing
    }
    ~KeyboardBroadcastInfo() override = default; // do nothing

  public:
    State m_state;
    std::string m_screens;
  };

  /*!
  Start the server with the configuration \p config and the primary
  client (local screen) \p primaryClient.  The client retains
  ownership of \p primaryClient.
  */
  Server(ServerConfig &config, PrimaryClient *primaryClient, deskflow::Screen *screen, IEventQueue *events);
  Server(Server const &) = delete;
  Server(Server &&) = delete;
  ~Server();

  Server &operator=(Server const &) = delete;
  Server &operator=(Server &&) = delete;

  //! @name manipulators
  //@{

  //! Set configuration
  /*!
  Change the server's configuration.  Returns true iff the new
  configuration was accepted (it must include the server's name).
  This will disconnect any clients no longer in the configuration.
  */
  bool setConfig(const ServerConfig &);

  //! Add a client
  /*!
  Adds \p client to the server.  The client is adopted and will be
  destroyed when the client disconnects or is disconnected.
  */
  void adoptClient(BaseClientProxy *client);

  //! Disconnect clients
  /*!
  Disconnect clients.  This tells them to disconnect but does not wait
  for them to actually do so.  The server sends the disconnected event
  when they're all disconnected (or immediately if none are connected).
  The caller can also just destroy this object to force the disconnection.
  */
  void disconnect();

  //! Store ClientListener pointer
  void setListener(ClientListener *p)
  {
    m_clientListener = p;
  }

  //@}
  //! @name accessors
  //@{

  //! Get the network protocol
  /*!
  Returns the network protocol used by the server.
  */
  std::string protocolString() const;

  //! Get the clipboard lane manager
  /*!
  The server owns exactly one, with a session per connected 1.9+ client. ClientProxyUnknown needs it
  to validate an incoming lane greeting; ClientProxy1_6 needs it to send. Always valid for the
  server's lifetime. See MT-CLIPBOARD-LANE-DESIGN.md.
  */
  deskflow::ClipboardLaneManager &clipboardLane() const
  {
    return *m_clipboardLane;
  }

  //! The server's own marshalling of clipboard \p id, if \p forObject IS the server's clipboard
  /*!
  MouseTransfer, stage 5. A client proxy asked to push a clipboard used to copy it into its own
  Clipboard and marshall THAT -- producing, byte for byte, the string the server had already
  marshalled a moment earlier in onClipboardChanged, once per client. At 64 MiB that was 134 MB of
  allocation and copying per client per push, and one more retained copy per client for as long as
  the lane held it.

  Both call sites that push to a client proxy (onClipboardChanged and switchScreen) pass
  &m_clipboards[id].m_clipboard, so the identity test is exact rather than a guess: if the object is
  the server's own, the cached marshalling of it is by construction the same bytes, and the caller
  gets to share the one buffer instead of building its own. Anything else -- a clipboard from
  somewhere this function does not know about -- gets nullptr and the caller marshalls for itself.

  \return the shared payload, or nullptr if \p forObject is not the server's clipboard \p id.
  */
  std::shared_ptr<const std::string> marshalledClipboard(ClipboardID id, const IClipboard *forObject) const;

  //! The CONFIGURATION's spelling of a screen name
  /*!
  Config lookups are CASELESS and resolve aliases, so the name a client announces about itself is
  not necessarily the name the configuration (and therefore m_clients, and therefore a clipboard
  lane session) knows it by. Anything that arrives from the wire, or from a proxy's own getName(),
  has to come through here before it can be matched against them -- otherwise an aliased or
  differently-cased screen silently fails every lookup. Falls back to \p name when the
  configuration has never heard of it, exactly as getName() always has.
  */
  std::string canonicalName(const std::string &name) const;

  //! Get number of connected clients
  /*!
  Returns the number of connected clients, including the server itself.
  */
  uint32_t getNumClients() const;

  //! Get the list of connected clients
  /*!
  Set the \c list to the names of the currently connected clients.
  */
  void getClients(std::vector<std::string> &list) const;

  //@}

private:
  // get canonical name of client
  std::string getName(const BaseClientProxy *) const;

  // get the sides of the primary screen that have neighbors
  uint32_t getActivePrimarySides() const;

  // returns true iff mouse should be locked to the current screen
  // according to this object only, ignoring what the primary client
  // says.
  bool isLockedToScreenServer() const;

  // returns true iff mouse should be locked to the current screen
  // according to this object or the primary client.
  bool isLockedToScreen() const;

  // returns the jump zone of the client
  int32_t getJumpZoneSize(const BaseClientProxy *) const;

  // change the active screen
  void switchScreen(BaseClientProxy *, int32_t x, int32_t y, bool forScreenSaver);

  // jump to screen
  void jumpToScreen(BaseClientProxy *);

  // convert pixel position to fraction, using x or y depending on the
  // direction.
  float mapToFraction(const BaseClientProxy *, Direction, int32_t x, int32_t y) const;

  // convert fraction to pixel position, writing only x or y depending
  // on the direction.
  void mapToPixel(const BaseClientProxy *, Direction, float f, int32_t &x, int32_t &y) const;

  // returns true if the client has a neighbor anywhere along the edge
  // indicated by the direction.
  bool hasAnyNeighbor(const BaseClientProxy *, Direction) const;

  // lookup neighboring screen, mapping the coordinate independent of
  // the direction to the neighbor's coordinate space.
  BaseClientProxy *getNeighbor(const BaseClientProxy *, Direction, int32_t &x, int32_t &y) const;

  // lookup neighboring screen.  given a position relative to the
  // source screen, find the screen we should move onto and where.
  // if the position is sufficiently far from the source then we
  // cross multiple screens.  if there is no suitable screen then
  // return nullptr and x,y are not modified.
  BaseClientProxy *mapToNeighbor(BaseClientProxy *, Direction, int32_t &x, int32_t &y) const;

  // adjusts x and y or neither to avoid ending up in a jump zone
  // after entering the client in the given direction.
  void avoidJumpZone(const BaseClientProxy *, Direction, int32_t &x, int32_t &y) const;

  // test if a switch is permitted.  this includes testing user
  // options like switch delay and tracking any state required to
  // implement them.  returns true iff a switch is permitted.
  bool isSwitchOkay(BaseClientProxy *dst, Direction, int32_t x, int32_t y, int32_t xActive, int32_t yActive);

  // update switch state due to a mouse move at \p x, \p y that
  // doesn't switch screens.
  void noSwitch(int32_t x, int32_t y);

  // stop switch timers
  void stopSwitch();

  // start two tap switch timer
  void startSwitchTwoTap();

  // arm the two tap switch timer if \p x, \p y is outside the tap zone
  void armSwitchTwoTap(int32_t x, int32_t y);

  // stop the two tap switch timer
  void stopSwitchTwoTap();

  // returns true iff the two tap switch timer is started
  bool isSwitchTwoTapStarted() const;

  // returns true iff should switch because of two tap
  bool shouldSwitchTwoTap() const;

  // start delay switch timer
  void startSwitchWait(int32_t x, int32_t y);

  // stop delay switch timer
  void stopSwitchWait();

  // returns true iff the delay switch timer is started
  bool isSwitchWaitStarted() const;

  // returns the corner (EScreenSwitchCornerMasks) where x,y is on the
  // given client.  corners have the given size.
  uint32_t getCorner(const BaseClientProxy *, int32_t x, int32_t y, int32_t size) const;

  // stop relative mouse moves
  void stopRelativeMoves();

  // send screen options to \c client
  void sendOptions(BaseClientProxy *client) const;

  // process options from configuration
  void processOptions();

  // event handlers
  void handleShapeChanged(BaseClientProxy *client);
  void handleClipboardGrabbed(const Event &event, BaseClientProxy *client);
  void handleClipboardChanged(const Event &event, BaseClientProxy *client);
  void handleKeyDownEvent(const Event &event);
  void handleKeyUpEvent(const Event &event);
  void handleKeyRepeatEvent(const Event &event);
  void handleButtonDownEvent(const Event &event);
  void handleButtonUpEvent(const Event &event);
  void handleMotionPrimaryEvent(const Event &event);
  void handleMotionSecondaryEvent(const Event &event);
  void handleWheelEvent(const Event &event);
  void handleSwitchWaitTimeout();
  void handleClientDisconnected(BaseClientProxy *client);
  void handleClientCloseTimeout(BaseClientProxy *client);
  void handleSwitchToScreenEvent(const Event &event);
  // MouseTransfer: centre-switch the cursor to a screen by name (shared by the switchToScreen input
  // filter action and the switch-request file poll); checkSwitchRequest reads the wrapper file.
  void switchToScreenByName(const std::string &name);
  void checkSwitchRequest();
  // MouseTransfer: poll a wrapper-written "layout reload request" file so the wrapper can apply an
  // edited external layout (changed edge-transition ranges) to the RUNNING server with no restart.
  void checkReloadRequest();
  void handleSwitchInDirectionEvent(const Event &event);
  void handleToggleScreenEvent(const Event &);
  void handleKeyboardBroadcastEvent(const Event &event);
  void handleLockCursorToScreenEvent(const Event &event);

  // MouseTransfer clipboard lane: arm a 1.9+ client with a session token and tell it the lane is
  // available. Called once per adopted client, AFTER the session exists.
  void offerClipboardLane(BaseClientProxy *client);

  // MouseTransfer clipboard lane: the configured clipboard limit expressed in BYTES.
  // m_maximumClipboardSize is in KB and defaults to INT_MAX, so the multiplication needs a ceiling
  // rather than a cast.
  size_t maxClipboardBytes() const;

  // MouseTransfer clipboard lane: a complete payload arrived from a peer. Runs on the MAIN thread
  // (the lane manager posts an event to get here), so it may touch proxy state.
  void handleLaneClipboard(const deskflow::LaneClipboardInfo &info);

  // event processing
  void onClipboardChanged(const BaseClientProxy *sender, ClipboardID id, uint32_t seqNum);
  void onScreensaver(bool activated);
  void onKeyDown(KeyID, KeyModifierMask, KeyButton, const std::string &, const char *screens);
  void onKeyUp(KeyID, KeyModifierMask, KeyButton, const char *screens);
  void onKeyRepeat(KeyID, KeyModifierMask, int32_t, KeyButton, const std::string &);
  void onMouseDown(ButtonID);
  void onMouseUp(ButtonID);
  bool onMouseMovePrimary(int32_t x, int32_t y);
  void onMouseMoveSecondary(int32_t dx, int32_t dy);
  void onMouseWheel(int32_t xDelta, int32_t yDelta);

  // add client to list and attach event handlers for client
  bool addClient(BaseClientProxy *);

  // remove client from list and detach event handlers for client
  bool removeClient(BaseClientProxy *);

  // close a client
  void closeClient(BaseClientProxy *, const char *msg);

  // close clients not in \p config
  void closeClients(const ServerConfig &config);

  // close all clients whether they've completed the handshake or not,
  // except the primary client
  void closeAllClients();

  // remove clients from internal state
  void removeActiveClient(BaseClientProxy *);
  void removeOldClient(BaseClientProxy *);

  // force the cursor off of \p client
  void forceLeaveClient(const BaseClientProxy *client);

private:
  class ClipboardInfo
  {
  public:
    ClipboardInfo() = default;

  public:
    Clipboard m_clipboard;
    //! m_clipboard.marshall(), SHARED and kept in step with it -- see Server::marshalledClipboard()
    /*!
    MouseTransfer: was a plain std::string, re-derived by every consumer. It is now the ONE buffer
    every consumer of a given clipboard change uses: the dedup comparison here, the size checks in
    onScreenSwitch, and the payload each client proxy hands to the lane. A server with two clients
    used to build FOUR identical marshallings of the same bytes and keep three of them alive.

    Invariant: assigned at every point m_clipboard is assigned, including the over-limit early
    return in onClipboardChanged, so "current marshalling of m_clipboard" is a property rather than
    a hope. Never null after the constructor.
    */
    std::shared_ptr<const std::string> m_clipboardData;
    std::string m_clipboardOwner;
    uint32_t m_clipboardSeqNum = 0;
  };
  // Order suggested by clang

  // the Primary Screen Client
  PrimaryClient *m_primaryClient = nullptr;

  // the client with focus
  BaseClientProxy *m_active = nullptr;

  // current configuration
  ServerConfig *m_config = nullptr;

  // input filter (from m_config);
  InputFilter *m_inputFilter = nullptr;

  // state saved when screen saver activates
  BaseClientProxy *m_activeSaver = nullptr;

  BaseClientProxy *m_switchScreen = nullptr;
  double m_switchWaitDelay = 0.0;
  EventQueueTimer *m_switchWaitTimer = nullptr;

  // MouseTransfer: poll a wrapper-written "switch request" file so an external process can move the
  // shared cursor to a named screen (the "jump cursor to this computer" feature). The wrapper writes
  // "<screen> <nonce>"; when the content changes we centre-switch to that screen. This is the
  // RELIABLE trigger: an injected switchToScreen hotkey is NOT processed by the input filter while
  // the cursor is on a client, so it can't pull the cursor back to the server.
  EventQueueTimer *m_switchReqTimer = nullptr;
  std::string m_switchReqFile;
  std::string m_switchReqLast;

  // MouseTransfer: poll a wrapper-written "layout reload request" file (a "<nonce>" the wrapper bumps
  // after rewriting the external layout). On a change we raise ServerAppReloadConfig, which re-reads
  // the external layout file and hot-applies it via setConfig() — so an edited edge-transition range
  // takes effect on the running server with NO restart (clients stay connected). The path comes from
  // MOUSETRANSFER_RELOADFILE, else "layoutreload" relative to the core's CWD (its bundle dir), the
  // same convention as the switch-request file.
  EventQueueTimer *m_reloadReqTimer = nullptr;
  std::string m_reloadReqFile;
  std::string m_reloadReqLast;

  // delay for double-tap screen switching
  double m_switchTwoTapDelay = 0.0;

  // server screen
  deskflow::Screen *m_screen;

  IEventQueue *m_events = nullptr;
  size_t m_maximumClipboardSize = INT_MAX;

  // MouseTransfer: the dedicated clipboard connections. Constructed with the server and destroyed
  // with it (which stops and joins every lane worker), so clipboardLane() is always valid.
  std::unique_ptr<deskflow::ClipboardLaneManager> m_clipboardLane;

  ClientListener *m_clientListener = nullptr;
  Stopwatch m_switchTwoTapTimer;

  // Name of screen broadcasting the keyboard events
  std::string m_keyboardBroadcastingScreens;

  // all clients (including the primary client) indexed by name
  using ClientList = std::map<std::string, BaseClientProxy *>;
  using ClientSet = std::set<BaseClientProxy *>;
  ClientList m_clients;
  ClientSet m_clientSet;

  // all old connections that we're waiting to hangup
  using OldClients = std::map<BaseClientProxy *, EventQueueTimer *>;
  OldClients m_oldClients;

  // clipboard cache
  ClipboardInfo m_clipboards[kClipboardEnd];

  // used in hello message sent to the client
  NetworkProtocol m_protocol = NetworkProtocol::Barrier;

  // the sequence number of enter messages
  uint32_t m_seqNum = 0;

  // current mouse position (in absolute screen coordinates) on
  // whichever screen is active
  int32_t m_x;
  int32_t m_y;

  // last mouse deltas.  this is needed to smooth out double tap
  // on win32 which reports bogus mouse motion at the edge of
  // the screen when using low level hooks, synthesizing motion
  // in the opposite direction the mouse actually moved.
  int32_t m_xDelta = 0;
  int32_t m_yDelta = 0;
  int32_t m_xDelta2 = 0;
  int32_t m_yDelta2 = 0;

  int32_t m_xSaver;
  int32_t m_ySaver;

  // state for delayed screen switching
  int32_t m_switchWaitX;
  int32_t m_switchWaitY;

  int32_t m_switchTwoTapZone = 3;

  // common state for screen switch tests.  all tests are always
  // trying to reach the same screen in the same direction.
  Direction m_switchDir = Direction::NoDirection;

  bool m_switchTwoTapEngaged = false;
  bool m_switchTwoTapArmed = false;

  // modifiers needed before switching
  bool m_switchNeedsShift = false;
  bool m_switchNeedsControl = false;
  bool m_switchNeedsAlt = false;

  // relative mouse move option
  bool m_relativeMoves = false;

  // flag whether or not we have broadcasting enabled and the screens to
  // which we should send broadcasted keys.
  bool m_keyboardBroadcasting = false;

  // screen locking (former scroll lock)
  bool m_lockedToScreen = false;

  bool m_defaultLockToScreenState = false;
  bool m_disableLockToScreen = false;
  bool m_enableClipboard = true;
};
