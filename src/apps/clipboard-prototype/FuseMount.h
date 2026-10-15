/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "FakeRemoteSource.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

struct fuse_session;

namespace clipboard_proto {

class FuseMount
{
public:
  FuseMount(FileTreeSource &source, TraceLog &log);
  // Foreground only. libfuse handles SIGINT/SIGTERM and unmounts on exit.
  int run(const std::string &mountpoint);

  FileTreeSource &source;
  TraceLog &log;
  std::uint64_t open(const FileEntry &entry);
  const FileEntry *handle(std::uint64_t handleId);
  bool release(std::uint64_t handleId);
  void startCancellationMonitor(struct fuse_session *session);
  void stopCancellationMonitor();

private:
  std::mutex m_handlesMutex;
  std::uint64_t m_nextHandle{1};
  std::unordered_map<std::uint64_t, const FileEntry *> m_handles;
  std::jthread m_cancelMonitor;
};

} // namespace clipboard_proto
