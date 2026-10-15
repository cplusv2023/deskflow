// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "clipboard/CoreClipboard.h"
#include "server/ClientProxy1_8.h"
class ClientProxy1_9 : public ClientProxy1_8
{
public:
  using ClientProxy1_8::ClientProxy1_8;
  void startClipboard() override;
  void stopClipboard() override;

private:
  std::unique_ptr<deskflow::clipboard::PeerLink> m_clipboard;
};
