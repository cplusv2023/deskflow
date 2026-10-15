/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "FakeRemoteSource.h"

#include <filesystem>
#include <memory>

namespace clipboard_proto {

// Receiver adapter for the same-user AF_UNIX experiment, not Deskflow networking.
class RpcFileSource final : public FileTreeSource
{
public:
  struct Options
  {
    bool detectable{false}; // Explicit opt-in; ordinary files cannot promise immutable versions.
    std::uint32_t timeoutMs{5000};
    std::uint32_t leaseTtlMs{30000};
  };
  RpcFileSource(const std::filesystem::path &socketPath, TraceLog &log, Options options);
  ~RpcFileSource() override;
  const std::vector<FileEntry> &entries() const override;
  void metadata(std::string_view action, std::string_view path, const FileEntry *entry) override;
  void open(const FileEntry &entry, std::uint64_t handleId) override;
  void close(const FileEntry &entry, std::uint64_t handleId) override;
  std::size_t
  readRange(const FileEntry &entry, std::uint64_t offset, std::span<char> buffer, std::uint64_t handleId = 0) override;
  void cancel() noexcept override;
  void beginShutdown() noexcept override;
  bool hasPendingRequests() const noexcept override;
  void shutdown() noexcept override;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace clipboard_proto
