// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "client/ServerProxy1_8.h"
#include "clipboard/CoreClipboard.h"
class ServerProxy1_9 : public ServerProxy1_8
{
public:
  using ServerProxy1_8::ServerProxy1_8;

protected:
  ConnectionResult parseMessage(const uint8_t *code) override;

private:
  bool m_capabilitySeen = false;
  std::unique_ptr<deskflow::clipboard::PeerLink> m_clipboard;
};
