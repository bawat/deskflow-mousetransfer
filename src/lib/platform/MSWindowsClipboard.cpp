/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsClipboard.h"

#include "base/Log.h"
#include "platform/MSWindowsClipboardBitmapConverter.h"
#include "platform/MSWindowsClipboardFacade.h"
#include "platform/MSWindowsClipboardHTMLConverter.h"
#include "platform/MSWindowsClipboardUTF16Converter.h"

//
// MSWindowsClipboard
//

UINT MSWindowsClipboard::s_ownershipFormat = 0;

MSWindowsClipboard::MSWindowsClipboard(HWND window)
    : m_window(window),
      m_time(0),
      m_facade(new MSWindowsClipboardFacade()),
      m_deleteFacade(true)
{
  // add converters, most desired first
  m_converters.push_back(new MSWindowsClipboardUTF16Converter);
  m_converters.push_back(new MSWindowsClipboardBitmapConverter);
  m_converters.push_back(new MSWindowsClipboardHTMLConverter);
}

MSWindowsClipboard::~MSWindowsClipboard()
{
  clearConverters();

  // dependency injection causes confusion over ownership, so we need
  // logic to decide whether or not we delete the facade. there must
  // be a more elegant way of doing this.
  if (m_deleteFacade)
    delete m_facade;
}

void MSWindowsClipboard::setFacade(IMSWindowsClipboardFacade &facade)
{
  delete m_facade;
  m_facade = &facade;
  m_deleteFacade = false;
}

bool MSWindowsClipboard::emptyUnowned()
{
  LOG_DEBUG("empty clipboard");

  // empty the clipboard (and take ownership)
  if (!EmptyClipboard()) {
    // unable to cause this in integ tests, but this error has never
    // actually been reported by users.
    LOG_WARN("failed to grab clipboard");
    return false;
  }

  return true;
}

bool MSWindowsClipboard::empty()
{
  if (!emptyUnowned()) {
    return false;
  }

  // mark clipboard as being owned by deskflow
  HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, 1);
  if (nullptr == SetClipboardData(getOwnershipFormat(), data)) {
    LOG_WARN("failed to set clipboard data");
    GlobalFree(data);
    return false;
  }

  return true;
}

void MSWindowsClipboard::add(Format format, const std::string &data)
{
  // exit early if there is no data to prevent spurious "failed to convert clipboard data" errors
  if (data.empty()) {
    LOG_DEBUG("not adding 0 bytes to clipboard format: %d", format);
    return;
  }
  bool isSucceeded = false;
  // convert data to win32 form
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    IMSWindowsClipboardConverter *converter = *index;

    // skip converters for other formats
    if (converter->getFormat() == format) {
      HANDLE win32Data = converter->fromIClipboard(data);
      if (win32Data != nullptr) {
        LOG_DEBUG("add %d bytes to clipboard format: %d", data.size(), format);
        m_facade->write(win32Data, converter->getWin32Format());
        isSucceeded = true;
        break;
      } else {
        LOG_DEBUG("failed to convert clipboard data to platform format");
      }
    }
  }

  if (!isSucceeded) {
    LOG_DEBUG("missed clipboard data convert for format: %d", format);
  }
}

bool MSWindowsClipboard::open(Time time) const
{
  LOG_DEBUG("open clipboard");

  if (!OpenClipboard(m_window)) {
    LOG_WARN("failed to open clipboard: %d", GetLastError());
    return false;
  }

  m_time = time;

  return true;
}

void MSWindowsClipboard::close() const
{
  LOG_DEBUG("close clipboard");
  CloseClipboard();
}

IClipboard::Time MSWindowsClipboard::getTime() const
{
  return m_time;
}

bool MSWindowsClipboard::has(Format format) const
{
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    IMSWindowsClipboardConverter *converter = *index;
    if (converter->getFormat() == format) {
      // MouseTransfer: via isFormatOnClipboard, not IsClipboardFormatAvailable — "HTML Format" is a
      // REGISTERED format and is invisible to that API on an elevated core (see the note there).
      if (isFormatOnClipboard(converter->getWin32Format())) {
        return true;
      }
    }
  }
  return false;
}

std::string MSWindowsClipboard::get(Format format) const
{
  // find the converter for the first clipboard format we can handle
  IMSWindowsClipboardConverter *converter = nullptr;
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {

    converter = *index;
    if (converter->getFormat() == format) {
      break;
    }
    converter = nullptr;
  }

  // if no converter then we don't recognize any formats
  if (converter == nullptr) {
    LOG_WARN("no converter for format %d", format);
    return std::string();
  }

  // get a handle to the clipboard data
  HANDLE win32Data = GetClipboardData(converter->getWin32Format());
  if (win32Data == nullptr) {
    // nb: can't cause this using integ tests; this is only caused when
    // the selected converter returns an invalid format -- which you
    // cannot cause using public functions.
    return std::string();
  }

  // convert
  return converter->toIClipboard(win32Data);
}

void MSWindowsClipboard::clearConverters()
{
  for (ConverterList::iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    delete *index;
  }
  m_converters.clear();
}

bool MSWindowsClipboard::isOwnedByDeskflow()
{
  // create ownership format if we haven't yet
  if (s_ownershipFormat == 0) {
    s_ownershipFormat = RegisterClipboardFormat(TEXT("Deskflow Ownership"));
  }
  return isFormatOnClipboard(getOwnershipFormat());
}

// MouseTransfer: IsClipboardFormatAvailable() MISREPORTS a REGISTERED clipboard format placed by a
// LOWER-PRIVILEGE process when the reader runs elevated (LocalSystem + UIAccess, which is how the
// MouseTransfer wrapper starts the core so it can drive UAC prompts).
//
// Scope, measured rather than assumed: a format this process placed ITSELF is visible normally — so
// Deskflow's own echo suppression has always worked (no client ever announced a grab after writing a
// received clipboard) — and two same-integrity processes see each other's formats fine. Only the
// cross-privilege direction fails, which is the direction a wrapper needs and one stock Deskflow
// never exercises.
//
// Measured on Windows 10 19045 with the core's own instrumentation:
//
//   MTDIAG isOwnedByDeskflow: cached=C242 fresh=C242 availCached=0 availFresh=0 formats=[000D C242]
//
// i.e. the atom is right (a freshly registered atom equals the cached one, and equals the atom the
// writing process used), EnumClipboardFormats lists C242 sitting on the clipboard — and
// IsClipboardFormatAvailable(C242) still answers "no" for the very format just enumerated. A core
// running as the normal USER answers "yes" to the identical clipboard. Standard formats (CF_TEXT,
// CF_UNICODETEXT...) are reported correctly either way; only registered ones are affected.
//
// Consequences this fixes, both of which presented as "the core ignores what we tagged":
//   - "Deskflow Ownership" set by another process was invisible, so the elevated core treated every
//     such write as a foreign clipboard change and fired ClipboardGrabbed — defeating the ownership
//     protocol and propagating content that was explicitly marked as already-synced.
//   - "HTML Format" (MSWindowsClipboardHTMLConverter) is likewise a REGISTERED format, and the apps
//     that publish HTML run unelevated, so has() would not see it and HTML sync degrades to plain
//     text. Inferred from the same measured mechanism; not separately reproduced.
//
// EnumClipboardFormats is truthful in both contexts, so it is the authority; the cheap
// IsClipboardFormatAvailable call is kept as a fast path so unelevated behaviour is byte-identical.
bool MSWindowsClipboard::isFormatOnClipboard(UINT format)
{
  if (format == 0) {
    return false;
  }

  // Fast path — correct for a normally-privileged process, and no clipboard open required.
  if (IsClipboardFormatAvailable(format) != 0) {
    return true;
  }

  // Enumerating requires the clipboard to be OPEN by us. Try it as-is first: has() and the unit
  // tests call this while they already hold the clipboard, and opening a clipboard we already own
  // only to close it again would yank it out from under the caller mid-operation.
  SetLastError(ERROR_SUCCESS);
  const UINT first = EnumClipboardFormats(0);
  if (first != 0 || GetLastError() != ERROR_CLIPBOARD_NOT_OPEN) {
    for (UINT f = first; f != 0; f = EnumClipboardFormats(f)) {
      if (f == format) {
        return true;
      }
    }
    return false;
  }

  // Not open (the onClipboardChange / checkClipboards callers) — open it just for the scan.
  //
  // RETRY, briefly. The clipboard is a single-holder lock and this runs from the clipboard-change
  // notification, precisely when everyone else is looking too (the MouseTransfer wrapper's own
  // watcher opens it on every sequence change). A single attempt loses that race often enough to
  // make the fix look intermittent — measured 1-in-5 before this loop, 5-in-5 after. Bounded hard
  // because we are on the core's message thread: 10 x 5ms is invisible on an event that fires only
  // when a human copies something, and losing anyway just answers as the old code did.
  bool opened = false;
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (OpenClipboard(nullptr)) {
      opened = true;
      break;
    }
    Sleep(5);
  }
  if (!opened) {
    return false;
  }
  bool found = false;
  for (UINT f = EnumClipboardFormats(0); f != 0; f = EnumClipboardFormats(f)) {
    if (f == format) {
      found = true;
      break;
    }
  }
  CloseClipboard();
  return found;
}

UINT MSWindowsClipboard::getOwnershipFormat()
{
  // create ownership format if we haven't yet
  if (s_ownershipFormat == 0) {
    s_ownershipFormat = RegisterClipboardFormat(TEXT("Deskflow Ownership"));
  }

  // return the format
  return s_ownershipFormat;
}
