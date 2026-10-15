/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2006 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "common/Settings.h"
#include "deskflow/ipc/CoreIpc.h"

#include "server/BaseClientProxy.h"

//
// BaseClientProxy
//

BaseClientProxy::BaseClientProxy(const std::string &name) : m_name(name)
{
  // do nothing
}

void BaseClientProxy::setJumpCursorPos(int32_t x, int32_t y)
{
  m_x = x;
  m_y = y;
}

void BaseClientProxy::getJumpCursorPos(int32_t &x, int32_t &y) const
{
  x = m_x;
  y = m_y;
}

std::string BaseClientProxy::getName() const
{
  return m_name;
}

void BaseClientProxy::startClipboard()
{
  if (Settings::value(Settings::Core::FileClipboard).toBool())
    ipcSendToClient("clipboardStatus", "Legacy peer: file clipboard unavailable");
}
