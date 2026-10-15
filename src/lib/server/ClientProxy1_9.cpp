// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "ClientProxy1_9.h"
#include "deskflow/ProtocolUtil.h"
void ClientProxy1_9::startClipboard()
{
  m_clipboard = deskflow::clipboard::PeerLink::listen(getStream());
  ProtocolUtil::writef(getStream(), "DCAP%2i%2i", 1, m_clipboard ? m_clipboard->port() : 0);
}
void ClientProxy1_9::stopClipboard()
{
  m_clipboard.reset();
}
