/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardChunk.h"
#include "deskflow/ClipboardTypes.h"
#include "deskflow/KeyTypes.h"
#include "deskflow/languages/LanguageManager.h"

class Client;
class ClientInfo;
class EventQueueTimer;
class IClipboard;
namespace deskflow {
class IStream;
}
class IEventQueue;

//! Proxy for server
/*!
This class acts a proxy for the server, converting calls into messages
to the server and messages from the server to calls on the client.
*/
class ServerProxy
{
public:
  /*!
  Process messages from the server on \p stream and forward to
  \p client.
  */
  ServerProxy(Client *client, deskflow::IStream *stream, IEventQueue *events);
  ServerProxy(ServerProxy const &) = delete;
  ServerProxy(ServerProxy &&) = delete;
  ~ServerProxy();

  ServerProxy &operator=(ServerProxy const &) = delete;
  ServerProxy &operator=(ServerProxy &&) = delete;

  //! @name manipulators
  //@{

  void onInfoChanged();
  bool onGrabClipboard(ClipboardID);
  void onClipboardChanged(ClipboardID, const IClipboard *);

  //! Apply a clipboard that arrived over the lane (MouseTransfer fork)
  /*!
  Does exactly what setClipboard()'s completion branch does -- unmarshalls, hands the clipboard to
  the client and logs "clipboard was updated" -- for data that came in over the dedicated lane
  instead of in-stream. Deliberately the same path and the same log line, so nothing downstream can
  behave differently depending on which transport carried the bytes.

  Main thread only; the lane manager gets it here by posting an event.
  */
  void applyLaneClipboard(ClipboardID id, uint32_t seqNum, const std::string &data);

  //@}

protected:
  enum class ConnectionResult
  {
    Okay,
    Unknown,
    Disconnect
  };
  ConnectionResult parseHandshakeMessage(const uint8_t *code);
  ConnectionResult parseMessage(const uint8_t *code);

private:
  // if compressing mouse motion then send the last motion now
  void flushCompressedMouse();

  void sendInfo(const ClientInfo &);

  void resetKeepAliveAlarm();
  void setKeepAliveRate(double);

  // modifier key translation
  KeyID translateKey(KeyID) const;
  KeyModifierMask translateModifierMask(KeyModifierMask) const;

  // event handlers
  void handleData();
  void handleKeepAliveAlarm();

  // message handlers
  void enter();
  void leave();
  void setClipboard();
  void clipboardLaneAdvert();
  void grabClipboard();
  void keyDown(uint16_t id, uint16_t mask, uint16_t button, const std::string &lang);
  void keyRepeat();
  void keyUp();
  void mouseDown();
  void mouseUp();
  void mouseMove();
  void mouseRelativeMove();
  void mouseWheel();
  void screensaver();
  void resetOptions();
  void setOptions();
  void queryInfo();
  void infoAcknowledgment();
  void secureInputNotification();
  void setServerLanguages();
  void setActiveServerLanguage(const std::string_view &language);
  void checkMissedLanguages() const;

private:
  using MessageParser = ConnectionResult (ServerProxy::*)(const uint8_t *);

  Client *m_client = nullptr;
  deskflow::IStream *m_stream = nullptr;

  uint32_t m_seqNum = 0;

  bool m_compressMouse = false;
  bool m_compressMouseRelative = false;
  int32_t m_xMouse = 0;
  int32_t m_yMouse = 0;
  int32_t m_dxMouse = 0;
  int32_t m_dyMouse = 0;

  bool m_ignoreMouse = false;

  KeyModifierID m_modifierTranslationTable[kKeyModifierIDLast];

  double m_keepAliveAlarm = 0.0;
  EventQueueTimer *m_keepAliveAlarmTimer = nullptr;

  MessageParser m_parser = &ServerProxy::parseHandshakeMessage;
  IEventQueue *m_events = nullptr;
  std::string m_serverLanguage = "";
  bool m_isUserNotifiedAboutLanguageSyncError = false;
  deskflow::languages::LanguageManager m_languageManager;

  // Per-connection, per-clipboard reassembly state. Was a `static std::string dataCached` inside
  // setClipboard() -- ONE buffer shared by both clipboard ids and every ServerProxy in the
  // process. See ClipboardChunk::Assembly for the corruption that caused.
  ClipboardChunk::Assembly m_clipboardAssembly[kClipboardEnd];

  // Rate limit for the "no lane, clipboard dropped" warning. A clipboard is offered on every leave,
  // so an un-limited warning would produce a line per crossing for as long as the lane stays down --
  // which buries the first occurrence, the only one that carries news.
  double m_lastNoLaneWarning = 0.0;
};
