// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "ServerProxy1_9.h"
#include "common/Settings.h"
#include "deskflow/ProtocolUtil.h"
#include "deskflow/ipc/CoreIpc.h"
#include <cstring>
ServerProxy::ConnectionResult ServerProxy1_9::parseMessage(const uint8_t *code)
{
  if (memcmp(code, "DCAP", 4) != 0)
    return ServerProxy1_8::parseMessage(code);
  uint16_t schema = 0, port = 0;
  ProtocolUtil::readf(getStream(), "%2i%2i", &schema, &port);
  if (m_capabilitySeen)
    return ConnectionResult::Disconnect;
  m_capabilitySeen = true;
  if (schema == 1 && port)
    m_clipboard = deskflow::clipboard::PeerLink::connect(getStream(), port);
  else if (Settings::value(Settings::Core::FileClipboard).toBool())
    ipcSendToClient("clipboardStatus", "Unavailable: peer did not enable file clipboard");
  return ConnectionResult::Okay;
}
