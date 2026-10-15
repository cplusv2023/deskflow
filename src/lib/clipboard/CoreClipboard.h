// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "io/IStream.h"
#include <memory>
#include <mutex>
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
// Serialize native legacy reads/claims with the handoff to and from the new backend.
// Keep the guard alive for the complete legacy operation, including cache updates.
class LegacyClipboardGuard
{
public:
  LegacyClipboardGuard();
  explicit operator bool() const;

private:
  std::unique_lock<std::mutex> m_lock;
};
#else
inline bool coreClipboardActive()
{
  return false;
}
class LegacyClipboardGuard
{
public:
  explicit operator bool() const
  {
    return true;
  }
};
#endif
} // namespace deskflow::clipboard
