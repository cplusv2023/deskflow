/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "FileTypes.h"
#include "LocalFileSource.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace deskflow::clipboard {

// Canonical v1 frame, shared by local adapters and the authenticated core channel.
// All lengths are big endian u32; uint64 values are decimal fields, never JSON doubles.
struct RpcMessage
{
  std::vector<std::string> fields;
  std::vector<std::uint8_t> bytes;
};
std::vector<std::uint8_t> encodeRpcFrame(const RpcMessage &message);
RpcMessage decodeRpcBody(std::span<const std::uint8_t> body);
RpcMessage offerMessage(const OfferInfo &offer);
OfferInfo parseOfferMessage(const RpcMessage &message);
RpcMessage dispatchFileRequest(LocalFileSource &source, const RpcMessage &request);

// Experimental same-user AF_UNIX transport, not the Deskflow network protocol.
// A new connection creates a new epoch; only paths selected by the server can be read.
class RpcError : public std::runtime_error
{
public:
  RpcError(Status status, const std::string &message);
  Status status;
};

struct FileRpcServerOptions
{
  std::filesystem::path socketPath; // Parent must be private and owned by this user.
  std::vector<std::filesystem::path> paths;
  std::filesystem::path logPath;
  std::uint32_t responseBytes{262144};
  std::uint32_t delayMs{0}; // Test injection; interruptible by stop/disconnect.
};

void serveLocalFiles(const FileRpcServerOptions &options, const std::atomic_bool &stop);

class FileRpcClient
{
public:
  explicit FileRpcClient(const std::filesystem::path &socketPath, std::uint32_t timeoutMs = 5000);
  ~FileRpcClient();
  FileRpcClient(const FileRpcClient &) = delete;
  FileRpcClient &operator=(const FileRpcClient &) = delete;

  OfferResult hello();
  OfferResult refresh(); // New offer from the server's original selection, never arbitrary paths.
  LeaseResult acquire(const OfferInfo &offer, std::uint32_t ttlMs = 0);
  LeaseResult renew(const std::string &leaseId, std::uint32_t ttlMs = 0);
  LeaseResult release(const std::string &leaseId);
  OpenResult open(const std::string &leaseId, const FileInfo &file, ConsistencyMode mode = ConsistencyMode::Immutable);
  ReadResult read(const std::string &leaseId, const std::string &handleId, std::uint64_t offset, std::uint32_t length);
  Status close(const std::string &handleId);
  // Safe from another thread: wake any blocked call and prevent late delivery.
  void cancel();

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace deskflow::clipboard
