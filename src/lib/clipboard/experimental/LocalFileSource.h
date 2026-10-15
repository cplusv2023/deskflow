/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "FileService.h"

#include <filesystem>

namespace deskflow::clipboard::experimental {

struct LocalCapture
{
  Status status{Status::Ok};
  Offer offer;
};

// POSIX source for macOS and Ubuntu. Captures stable IDs and directory membership,
// pins file descriptors, and checks identity/size/mtime/ctime before AND after
// pread. This detects ordinary mutations but is not an immutable snapshot.
// Symlinks and special files are rejected, never followed implicitly.
class LocalFileSource final : public FileSource
{
public:
  static LocalCapture capture(
      const std::vector<std::filesystem::path> &paths, std::string offerId, std::string offerVersion,
      std::uint32_t maxEntries = 512
  );
  ~LocalFileSource() override;
  bool immutable() const override;
  Status validate(const Entry &entry) override;
  SourceRead
  read(const Entry &entry, std::uint64_t offset, std::uint32_t length, const RequestContext &context) override;

private:
  struct File;
  LocalFileSource() = default;
  Status captureObject(
      int parentFd, const std::string &name, const std::string &parentId, Offer &offer, std::uint32_t maxEntries,
      unsigned int depth
  );
  std::unordered_map<std::string, std::unique_ptr<File>> m_files;
};

} // namespace deskflow::clipboard::experimental
