/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/Clipboard.h"
#include "base/Log.h"

#include <utility>

//
// Clipboard
//

Clipboard::Clipboard()
{
  open(0);
  empty();
  close();
}

bool Clipboard::empty()
{
  if (!m_open) {
    LOG_WARN("cannot empty clipboard, not open");
    return false;
  }

  // clear all data
  for (int32_t index = 0; index < static_cast<int>(Format::TotalFormats); ++index) {
    m_data[index] = "";
    m_added[index] = false;
  }

  // save time
  m_timeOwned = m_time;

  // we're the owner now
  m_owner = true;

  return true;
}

void Clipboard::add(Format format, const std::string &data)
{
  if (!m_open) {
    LOG_WARN("cannot add to clipboard, not open");
    return;
  }

  if (!m_owner) {
    LOG_WARN("cannot add to clipboard, no owner");
    return;
  }

  const auto formatID = static_cast<int>(format);
  m_data[formatID] = data;
  m_added[formatID] = true;
}

// MouseTransfer: identical, except that the caller's buffer is TAKEN rather than copied. This is
// the overload IClipboard::copy() and IClipboard::unmarshall() reach, because both hand over a
// temporary they have no further use for -- see IClipboard::add(Format, std::string &&). The
// guard clauses are duplicated rather than delegated so that a rejected add() destroys the
// caller's buffer at exactly the same point the copying version would have ignored it.
void Clipboard::add(Format format, std::string &&data)
{
  if (!m_open) {
    LOG_WARN("cannot add to clipboard, not open");
    return;
  }

  if (!m_owner) {
    LOG_WARN("cannot add to clipboard, no owner");
    return;
  }

  const auto formatID = static_cast<int>(format);
  m_data[formatID] = std::move(data);
  m_added[formatID] = true;
}

bool Clipboard::open(Time time) const
{
  if (m_open) {
    LOG_DEBUG("skipping clipboard open, already open");
    return true;
  }

  m_open = true;
  m_time = time;

  return true;
}

void Clipboard::close() const
{
  if (!m_open) {
    LOG_WARN("clipboard is not open");
  }
  m_open = false;
}

Clipboard::Time Clipboard::getTime() const
{
  return m_timeOwned;
}

bool Clipboard::has(Format format) const
{
  if (!m_open) {
    LOG_WARN("cannot check for clipboard format, not open");
    return false;
  }
  return m_added[static_cast<int>(format)];
}

std::string Clipboard::get(Format format) const
{
  if (!m_open) {
    LOG_WARN("cannot get clipboard format, not open");
    return "";
  }
  return m_data[static_cast<int>(format)];
}

void Clipboard::unmarshall(const std::string &data, Time time)
{
  IClipboard::unmarshall(this, data, time);
}

std::string Clipboard::marshall() const
{
  return IClipboard::marshall(this);
}
