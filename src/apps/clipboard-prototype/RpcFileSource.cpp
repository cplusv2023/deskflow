/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "RpcFileSource.h"
#include "clipboard/LocalFileRpc.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace clipboard_proto {
namespace {
using namespace deskflow::clipboard;
using Clock = std::chrono::steady_clock;

int errorNumber(Status status)
{
  switch (status) {
  case Status::Ok:
    return 0;
  case Status::SourceChanged:
  case Status::OfferExpired:
  case Status::LeaseExpired:
  case Status::LeaseReleased:
  case Status::LeaseRevoked:
  case Status::VersionMismatch:
  case Status::StaleSession:
    return ESTALE;
  case Status::Disconnected:
    return ENOTCONN;
  case Status::Timeout:
    return ETIMEDOUT;
  case Status::Cancelled:
    return EINTR;
  case Status::SourceMissing:
  case Status::NotFound:
    return ENOENT;
  case Status::PermissionDenied:
    return EACCES;
  case Status::HandleClosed:
    return EBADF;
  case Status::InvalidArgument:
    return EINVAL;
  case Status::UnsupportedSize:
    return EFBIG;
  case Status::SourceNotStable:
    return EOPNOTSUPP;
  case Status::ResourceLimit:
    return EBUSY;
  case Status::InvalidSourceResponse:
  case Status::IoError:
    return EIO;
  }
  return EIO;
}

void require(Status status)
{
  if (status != Status::Ok)
    throw FileSourceError(errorNumber(status), std::string(statusName(status)));
}

struct Pending
{
  std::atomic<unsigned int> &count;
  explicit Pending(std::atomic<unsigned int> &value) : count(value)
  {
    count.fetch_add(1);
  }
  ~Pending()
  {
    count.fetch_sub(1);
  }
};

void validateManifest(const OfferInfo &offer)
{
  if (offer.sessionEpoch.empty() || offer.offerId.empty() || offer.offerVersion.empty() || offer.files.empty() ||
      offer.files.size() > 4096 || offer.roots.empty() || offer.roots.size() > 64)
    throw RpcError(Status::InvalidSourceResponse, "Invalid manifest identity or budget");
  std::unordered_map<std::string, const FileInfo *> byId;
  std::set<std::string> paths;
  for (const auto &entry : offer.files) {
    const std::filesystem::path path(entry.relativePath);
    if (entry.fileId.empty() || entry.fileId == "@fuse-root@" || entry.version.empty() || entry.name.empty() ||
        entry.relativePath.empty() || entry.relativePath.find('\0') != std::string::npos || path.is_absolute() ||
        path.lexically_normal().generic_string() != entry.relativePath || path.filename().string() != entry.name ||
        !paths.insert(entry.relativePath).second || !byId.emplace(entry.fileId, &entry).second)
      throw RpcError(Status::InvalidSourceResponse, "Invalid or duplicate manifest path");
    std::size_t depth = 0;
    for (const auto &component : path) {
      if (component == "." || component == ".." || component.empty() || ++depth > 65)
        throw RpcError(Status::InvalidSourceResponse, "Unsafe manifest path component");
    }
    if (entry.size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      throw RpcError(Status::UnsupportedSize, "FUSE cannot represent this file size");
    if (entry.kind == FileKind::Directory && entry.size != 0)
      throw RpcError(Status::InvalidSourceResponse, "Nonzero directory byte size");
  }
  const std::set<std::string> roots(offer.roots.begin(), offer.roots.end());
  if (roots.size() != offer.roots.size())
    throw RpcError(Status::InvalidSourceResponse, "Duplicate root ID");
  for (const auto &root : roots)
    if (!byId.contains(root))
      throw RpcError(Status::InvalidSourceResponse, "Unknown manifest root");
  for (const auto &entry : offer.files) {
    const auto parentPath = std::filesystem::path(entry.relativePath).parent_path().generic_string();
    if (entry.parentId.empty()) {
      if (!parentPath.empty() || !roots.contains(entry.fileId))
        throw RpcError(Status::InvalidSourceResponse, "Invalid selected root");
    } else {
      const auto parent = byId.find(entry.parentId);
      if (parent == byId.end() || parent->second->kind != FileKind::Directory ||
          parent->second->relativePath != parentPath || roots.contains(entry.fileId))
        throw RpcError(Status::InvalidSourceResponse, "Invalid manifest parent");
    }
  }
}

} // namespace

struct RpcFileSource::Impl
{
  struct Handle
  {
    std::string remoteId;
    std::string fileId;
  };
  FileRpcClient client;
  TraceLog &log;
  Options options;
  OfferInfo offer;
  LeaseResult lease;
  std::vector<FileEntry> tree;
  std::unordered_map<std::string, FileInfo> files;
  std::unordered_map<std::uint64_t, Handle> handles;
  std::mutex rpcMutex; // FileRpcClient permits one in-flight call on this connection.
  std::mutex waitMutex;
  std::condition_variable waitCondition;
  std::thread renewal;
  std::atomic_bool stopping{false};
  std::atomic_bool finished{false};
  std::atomic<Status> failure{Status::Ok};
  std::atomic<unsigned int> pending{0};
  Clock::time_point renewAt;

  Impl(const std::filesystem::path &socketPath, TraceLog &trace, Options settings)
      : client(socketPath, settings.timeoutMs),
        log(trace),
        options(settings)
  {
    const auto hello = client.hello();
    require(hello.status);
    offer = hello.offer;
    validateManifest(offer);
    log.setOfferId(offer.offerId);
    lease = client.acquire(offer, options.leaseTtlMs);
    require(lease.status);
    tree.push_back({"@fuse-root@", "/", true, 0, 0, {}});
    for (const auto &entry : offer.files) {
      tree.push_back({entry.fileId, '/' + entry.relativePath, entry.kind == FileKind::Directory, entry.size, 0, {}});
      files.emplace(entry.fileId, entry);
    }
    scheduleRenew();
    log.event("lease_acquired", context() + leaseFields() + ",\"status\":\"OK\"");
    renewal = std::thread([this] { renewLoop(); });
  }

  std::string context() const
  {
    return "\"schemaVersion\":1,\"side\":\"receiver\",\"sessionEpoch\":" + jsonQuote(offer.sessionEpoch) +
           ",\"leaseId\":" + jsonQuote(lease.leaseId);
  }
  std::string leaseFields() const
  {
    return ",\"grantedTtlMs\":" + std::to_string(lease.grantedTtlMs) +
           ",\"state\":" + jsonQuote(leaseStateName(lease.state));
  }
  void scheduleRenew()
  {
    const auto interval = std::max<std::uint32_t>(1, std::min<std::uint32_t>(10000, lease.grantedTtlMs / 3));
    renewAt = Clock::now() + std::chrono::milliseconds(interval);
  }
  void recordFailure(Status status)
  {
    Status expected = Status::Ok;
    failure.compare_exchange_strong(expected, status);
    client.cancel();
    waitCondition.notify_all();
  }
  void checkFailure() const
  {
    require(failure.load());
    if (stopping.load())
      throw FileSourceError(EINTR, "CANCELLED");
  }
  void renewIfDue()
  {
    checkFailure();
    if (Clock::now() < renewAt)
      return;
    Pending call(pending);
    const auto renewed = client.renew(lease.leaseId, options.leaseTtlMs);
    if (renewed.status != Status::Ok)
      recordFailure(renewed.status);
    require(renewed.status);
    if (renewed.leaseId != lease.leaseId || renewed.offerVersion != offer.offerVersion ||
        renewed.state != LeaseState::Active || renewed.grantedTtlMs == 0)
      throw RpcError(Status::InvalidSourceResponse, "Renew changed whole-offer lease");
    lease = renewed;
    scheduleRenew();
    log.event("lease_renewed", context() + leaseFields() + ",\"status\":\"OK\"");
  }
  void renewLoop()
  {
    for (;;) {
      std::unique_lock waiting(waitMutex);
      if (waitCondition.wait_for(waiting, std::chrono::milliseconds(25), [this] {
            return stopping.load() || failure.load() != Status::Ok;
          }))
        return;
      waiting.unlock();
      try {
        std::lock_guard rpc(rpcMutex);
        if (stopping.load())
          return;
        renewIfDue();
      } catch (const RpcError &error) {
        recordFailure(error.status);
        log.event("lease_error", context() + ",\"status\":" + jsonQuote(statusName(error.status)));
        return;
      } catch (const FileSourceError &error) {
        if (stopping.load())
          return;
        recordFailure(Status::LeaseExpired);
        log.event("lease_error", context() + ",\"message\":" + jsonQuote(error.what()));
        return;
      } catch (const std::exception &) {
        recordFailure(Status::IoError);
        return;
      }
    }
  }
};

RpcFileSource::RpcFileSource(const std::filesystem::path &socketPath, TraceLog &log, Options options)
    : m_impl(std::make_unique<Impl>(socketPath, log, options))
{
}

RpcFileSource::~RpcFileSource()
{
  shutdown();
}

const std::vector<FileEntry> &RpcFileSource::entries() const
{
  return m_impl->tree;
}

void RpcFileSource::metadata(std::string_view action, std::string_view path, const FileEntry *entry)
{
  m_impl->log.event(
      "metadata", "\"action\":" + jsonQuote(action) + ",\"path\":" + jsonQuote(path) + ",\"fileId\":" +
                      (entry ? jsonQuote(entry->id) : "null") + ",\"status\":" + jsonQuote(entry ? "OK" : "NOT_FOUND")
  );
}

void RpcFileSource::open(const FileEntry &entry, std::uint64_t handleId)
{
  auto &self = *m_impl;
  Pending pending(self.pending);
  std::lock_guard lock(self.rpcMutex);
  try {
    self.renewIfDue();
    const auto known = self.files.find(entry.id);
    if (known == self.files.end())
      throw FileSourceError(ENOENT, "NOT_FOUND");
    const auto opened = self.client.open(
        self.lease.leaseId, known->second,
        self.options.detectable ? ConsistencyMode::Detectable : ConsistencyMode::Immutable
    );
    self.log.event(
        "rpc_open",
        self.context() + ",\"fileId\":" + jsonQuote(entry.id) + ",\"localHandleId\":" + std::to_string(handleId) +
            ",\"handleId\":" + jsonQuote(opened.handleId) + ",\"status\":" + jsonQuote(statusName(opened.status))
    );
    require(opened.status);
    self.handles.emplace(handleId, Impl::Handle{opened.handleId, entry.id});
  } catch (const RpcError &error) {
    self.recordFailure(error.status);
    throw FileSourceError(errorNumber(error.status), std::string(statusName(error.status)));
  }
}

void RpcFileSource::close(const FileEntry &entry, std::uint64_t handleId)
{
  auto &self = *m_impl;
  Pending pending(self.pending);
  std::lock_guard lock(self.rpcMutex);
  const auto found = self.handles.find(handleId);
  if (found == self.handles.end())
    return;
  const auto remote = found->second.remoteId;
  try {
    require(self.failure.load());
    if (!self.stopping.load())
      self.renewIfDue();
    const auto status = self.client.close(remote);
    self.handles.erase(handleId);
    self.log.event(
        "rpc_close", self.context() + ",\"fileId\":" + jsonQuote(entry.id) + ",\"handleId\":" + jsonQuote(remote) +
                         ",\"status\":" + jsonQuote(statusName(status))
    );
    require(status);
  } catch (const RpcError &error) {
    self.recordFailure(error.status);
    throw FileSourceError(errorNumber(error.status), std::string(statusName(error.status)));
  }
}

std::size_t
RpcFileSource::readRange(const FileEntry &entry, std::uint64_t offset, std::span<char> buffer, std::uint64_t handleId)
{
  auto &self = *m_impl;
  Pending pending(self.pending);
  std::lock_guard lock(self.rpcMutex);
  try {
    self.renewIfDue();
    const auto found = self.handles.find(handleId);
    if (found == self.handles.end() || found->second.fileId != entry.id)
      throw FileSourceError(EBADF, "HANDLE_CLOSED");
    if (buffer.empty() || buffer.size() > maxRequestBytes)
      throw FileSourceError(EINVAL, "INVALID_ARGUMENT");
    const auto start = Clock::now();
    const auto result =
        self.client.read(self.lease.leaseId, found->second.remoteId, offset, static_cast<std::uint32_t>(buffer.size()));
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
    self.log.event(
        "read_range",
        self.context() + ",\"fileId\":" + jsonQuote(result.fileId) + ",\"version\":" + jsonQuote(result.version) +
            ",\"handleId\":" + jsonQuote(result.handleId) + ",\"requestId\":" + jsonQuote(result.requestId) +
            ",\"offset\":" + jsonQuote(std::to_string(offset)) + ",\"length\":" + std::to_string(buffer.size()) +
            ",\"returnedBytes\":" + std::to_string(result.bytes.size()) +
            ",\"eof\":" + (result.eof ? "true" : "false") + ",\"status\":" + jsonQuote(statusName(result.status)) +
            ",\"durationUs\":" + std::to_string(duration) +
            ",\"activeRequests\":1,\"inFlightBytes\":" + std::to_string(buffer.size())
    );
    require(result.status);
    const auto &version = self.files.at(entry.id).version;
    const auto remaining = offset >= entry.size ? 0 : entry.size - offset;
    if (result.sessionEpoch != self.offer.sessionEpoch || result.offerId != self.offer.offerId ||
        result.fileId != entry.id || result.version != version || result.bytes.size() > buffer.size() ||
        result.bytes.size() > remaining || result.eof != (result.bytes.size() == remaining) ||
        (remaining != 0 && result.bytes.empty()))
      throw RpcError(Status::InvalidSourceResponse, "Read does not match published immutable manifest");
    if (!result.bytes.empty())
      std::memcpy(buffer.data(), result.bytes.data(), result.bytes.size());
    return result.bytes.size();
  } catch (const RpcError &error) {
    self.log.event("rpc_error", self.context() + ",\"status\":" + jsonQuote(statusName(error.status)));
    self.recordFailure(error.status);
    throw FileSourceError(errorNumber(error.status), std::string(statusName(error.status)));
  }
}

void RpcFileSource::cancel() noexcept
{
  m_impl->recordFailure(Status::Cancelled);
}

void RpcFileSource::beginShutdown() noexcept
{
  m_impl->stopping.store(true);
  m_impl->waitCondition.notify_all();
}

bool RpcFileSource::hasPendingRequests() const noexcept
{
  return m_impl->pending.load() != 0;
}

void RpcFileSource::shutdown() noexcept
{
  auto &self = *m_impl;
  if (self.finished.exchange(true))
    return;
  beginShutdown();
  // A shutdown deadline is independent of the per-call timeout (which can be
  // ten minutes). It covers renewal join, queued RPCs and every cleanup call.
  std::mutex completionMutex;
  std::condition_variable completionCondition;
  bool complete = false;
  std::thread deadline([&] {
    std::unique_lock completion(completionMutex);
    if (!completionCondition.wait_for(completion, std::chrono::milliseconds(500), [&] { return complete; }))
      self.recordFailure(Status::Cancelled);
  });
  if (self.renewal.joinable())
    self.renewal.join();
  {
    std::lock_guard lock(self.rpcMutex);
    try {
      if (self.failure.load() == Status::Ok) {
        for (const auto &[local, handle] : self.handles) {
          (void)local;
          Pending call(self.pending);
          require(self.client.close(handle.remoteId));
        }
        self.handles.clear();
        Pending call(self.pending);
        const auto released = self.client.release(self.lease.leaseId);
        require(released.status);
        self.lease = released;
        self.log.event("lease_released", self.context() + self.leaseFields() + ",\"status\":\"OK\"");
      }
    } catch (const RpcError &error) {
      self.failure.store(error.status);
    } catch (const std::exception &) {
      self.failure.store(Status::IoError);
    }
  }
  {
    std::lock_guard completion(completionMutex);
    complete = true;
  }
  completionCondition.notify_all();
  deadline.join();
  self.client.cancel();
  self.log.event("rpc_shutdown", self.context() + ",\"status\":" + jsonQuote(statusName(self.failure.load())));
}

} // namespace clipboard_proto
