// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "io/IStream.h"
#include <memory>
namespace deskflow::clipboard {
// A proxy owns this session; destroying it revokes the epoch and all mounts.
class PeerLink
{
public:
  static std::unique_ptr<PeerLink> listen(IStream *mainStream);
  static std::unique_ptr<PeerLink> connect(IStream *mainStream, unsigned short port);
  ~PeerLink();
  unsigned short port() const;

private:
  struct Impl;
  explicit PeerLink(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};
#ifdef DESKFLOW_CLIPBOARD_FILES
bool coreClipboardActive();
#else
inline bool coreClipboardActive()
{
  return false;
}
#endif
} // namespace deskflow::clipboard
