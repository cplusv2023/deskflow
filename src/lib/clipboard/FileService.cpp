/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "FileService.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>

namespace deskflow::clipboard {

namespace {
constexpr std::size_t maxIdentifierBytes = 256;
constexpr std::size_t maxNameBytes = 1024;
// Includes conservative node/allocator overhead; payload lengths are separately bounded.
constexpr std::uint64_t nodeOverhead = 128;

bool validIdentifier(const std::string &id)
{
  return !id.empty() && id.size() <= maxIdentifierBytes;
}

Clock::time_point expiryAfter(Clock::time_point now, std::chrono::milliseconds ttl)
{
  const auto duration = std::chrono::duration_cast<Clock::duration>(ttl);
  return now > Clock::time_point::max() - duration ? Clock::time_point::max() : now + duration;
}
} // namespace

const char *statusName(Status status)
{
  switch (status) {
  case Status::Ok:
    return "OK";
  case Status::InvalidArgument:
    return "INVALID_ARGUMENT";
  case Status::UnsupportedSize:
    return "UNSUPPORTED_SIZE";
  case Status::NotFound:
    return "NOT_FOUND";
  case Status::OfferExpired:
    return "OFFER_EXPIRED";
  case Status::VersionMismatch:
    return "VERSION_MISMATCH";
  case Status::SourceChanged:
    return "SOURCE_CHANGED";
  case Status::SourceMissing:
    return "SOURCE_MISSING";
  case Status::SourceNotStable:
    return "SOURCE_NOT_STABLE";
  case Status::PermissionDenied:
    return "PERMISSION_DENIED";
  case Status::LeaseExpired:
    return "LEASE_EXPIRED";
  case Status::LeaseReleased:
    return "LEASE_RELEASED";
  case Status::LeaseRevoked:
    return "LEASE_REVOKED";
  case Status::HandleClosed:
    return "HANDLE_CLOSED";
  case Status::Disconnected:
    return "DISCONNECTED";
  case Status::StaleSession:
    return "STALE_SESSION";
  case Status::Timeout:
    return "TIMEOUT";
  case Status::Cancelled:
    return "CANCELLED";
  case Status::InvalidSourceResponse:
    return "INVALID_SOURCE_RESPONSE";
  case Status::ResourceLimit:
    return "RESOURCE_LIMIT";
  case Status::IoError:
    return "IO_ERROR";
  }
  return "IO_ERROR";
}

int statusErrno(Status status)
{
  switch (status) {
  case Status::Ok:
    return 0;
  case Status::InvalidArgument:
    return EINVAL;
  case Status::UnsupportedSize:
    return EOVERFLOW;
  case Status::NotFound:
  case Status::SourceMissing:
    return ENOENT;
  case Status::PermissionDenied:
    return EACCES;
  case Status::HandleClosed:
    return EBADF;
  case Status::Disconnected:
    return ENOTCONN;
  case Status::Timeout:
    return ETIMEDOUT;
  case Status::Cancelled:
    return EINTR;
  case Status::ResourceLimit:
    return EAGAIN;
  case Status::OfferExpired:
  case Status::VersionMismatch:
  case Status::SourceChanged:
  case Status::SourceNotStable:
  case Status::LeaseExpired:
  case Status::LeaseReleased:
  case Status::LeaseRevoked:
  case Status::StaleSession:
    return ESTALE;
  default:
    return EIO;
  }
}

Status validateReadResult(const ReadResult &result, const ReadResult &expected, std::uint64_t size)
{
  if (result.sessionEpoch != expected.sessionEpoch || result.offerId != expected.offerId ||
      result.fileId != expected.fileId || result.version != expected.version || result.handleId != expected.handleId ||
      result.requestId != expected.requestId || result.offset != expected.offset || result.length != expected.length)
    return Status::InvalidSourceResponse;
  if (result.status != Status::Ok)
    return result.bytes.empty() && !result.eof ? result.status : Status::InvalidSourceResponse;
  if (expected.length == 0)
    return Status::InvalidSourceResponse;
  if (expected.offset >= size)
    return result.bytes.empty() && result.eof ? Status::Ok : Status::InvalidSourceResponse;
  const auto remaining = size - expected.offset;
  if (result.bytes.empty() || result.bytes.size() > std::min<std::uint64_t>(remaining, expected.length) ||
      result.eof != (result.bytes.size() == remaining))
    return Status::InvalidSourceResponse;
  return Status::Ok;
}

struct FileService::StoredOffer
{
  Offer offer;
  Clock::time_point acquireUntil{Clock::time_point::max()};
  std::uint64_t metadataBytes{0};
  std::unordered_map<std::string, std::size_t> byId;
};

struct FileService::Lease
{
  std::shared_ptr<StoredOffer> offer;
  std::string offerVersion;
  LeaseState state{LeaseState::Active};
  Clock::time_point expires;
  std::chrono::milliseconds grantedTtl{0};
  std::uint32_t handles{0};
};

struct FileService::Handle
{
  std::string leaseId;
  std::string offerId;
  Entry entry;
  bool closed{false};
};

FileService::FileService(std::string sessionEpoch, Limits limits, std::function<Clock::time_point()> now)
    : m_sessionEpoch(std::move(sessionEpoch)),
      m_limits(limits),
      m_now(std::move(now))
{
  if (!validIdentifier(m_sessionEpoch) || !m_now || !limits.maxRequestBytes || !limits.maxActiveRequests ||
      !limits.maxInFlightBytes || limits.maxTtl.count() <= 0 || limits.maxTtl > std::chrono::hours(24))
    throw std::invalid_argument("Invalid clipboard session or limits");
}

FileService::~FileService() = default;

Status FileService::contextStatus(const RequestContext &context) const
{
  if (context.sessionEpoch != m_sessionEpoch)
    return Status::StaleSession;
  if (!m_connected)
    return Status::Disconnected;
  if (context.stop.stop_requested())
    return Status::Cancelled;
  if (m_now() >= context.deadline)
    return Status::Timeout;
  return Status::Ok;
}

Status FileService::begin(const RequestContext &context)
{
  collect();
  if (auto status = contextStatus(context); status != Status::Ok)
    return status;
  if (!validIdentifier(context.requestId) || m_requests.contains(context.requestId))
    return Status::InvalidArgument;
  const auto retained = sizeof(std::string) + context.requestId.size() + nodeOverhead;
  if (m_requests.size() >= m_limits.maxSessionRequests || retained > m_limits.maxRetainedBytes - m_usage.retainedBytes)
    return Status::ResourceLimit;
  m_requests.insert(context.requestId);
  m_usage.retainedBytes += retained;
  return Status::Ok;
}

Status FileService::leaseStatus(const Lease &lease, bool allowDraining) const
{
  switch (lease.state) {
  case LeaseState::Active:
    return Status::Ok;
  case LeaseState::Draining:
    return allowDraining ? Status::Ok : Status::LeaseReleased;
  case LeaseState::Closed:
    return Status::LeaseReleased;
  case LeaseState::Expired:
    return Status::LeaseExpired;
  case LeaseState::Revoked:
    return Status::LeaseRevoked;
  }
  return Status::LeaseRevoked;
}

void FileService::collect()
{
  const auto now = m_now();
  for (auto &[id, lease] : m_leases) {
    if ((lease.state == LeaseState::Active || lease.state == LeaseState::Draining) && now >= lease.expires) {
      lease.state = LeaseState::Expired;
      lease.offer.reset();
      --m_usage.activeLeases;
      for (auto &[handleId, handle] : m_handles) {
        if (handle.leaseId == id && !handle.closed) {
          handle.closed = true;
          --m_usage.activeHandles;
        }
      }
      lease.handles = 0;
    }
  }
  for (auto it = m_offers.begin(); it != m_offers.end();) {
    if (it->first != m_currentOffer && now >= it->second->acquireUntil && it->second.use_count() == 1) {
      m_usage.metadataBytes -= it->second->metadataBytes;
      m_entryCount -= static_cast<std::uint32_t>(it->second->offer.entries.size());
      it = m_offers.erase(it);
    } else {
      ++it;
    }
  }
  m_usage.offers = static_cast<std::uint32_t>(m_offers.size());
}

Status FileService::publishOffer(Offer offer)
{
  std::lock_guard lock(m_mutex);
  collect();
  if (!m_connected)
    return Status::Disconnected;
  if (!validIdentifier(offer.id) || !validIdentifier(offer.version) || !offer.source || offer.entries.empty() ||
      offer.acquisitionWindow.count() < 0 || offer.acquisitionWindow > std::chrono::hours(24) ||
      m_publishedIds.contains(offer.id))
    return Status::InvalidArgument;
  if (m_offers.size() >= m_limits.maxOffers || m_publishedIds.size() >= m_limits.maxSessionObjects ||
      offer.entries.size() > m_limits.maxEntries - m_entryCount ||
      sizeof(std::string) + offer.id.size() + nodeOverhead > m_limits.maxRetainedBytes - m_usage.retainedBytes)
    return Status::ResourceLimit;
  auto stored = std::make_shared<StoredOffer>();
  stored->metadataBytes = offer.id.size() + offer.version.size();
  std::size_t roots = 0;
  std::unordered_set<std::string> siblings;
  for (std::size_t index = 0; index < offer.entries.size(); ++index) {
    const auto &entry = offer.entries[index];
    if (!validIdentifier(entry.id) || !validIdentifier(entry.version) || entry.parentId.size() > maxIdentifierBytes ||
        entry.name.size() > maxNameBytes || !stored->byId.emplace(entry.id, index).second)
      return Status::InvalidArgument;
    if (entry.size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return Status::UnsupportedSize;
    if (entry.directory && entry.size != 0)
      return Status::InvalidArgument;
    if (entry.parentId.empty()) {
      if (!entry.directory || !entry.name.empty())
        return Status::InvalidArgument;
      ++roots;
    } else if (entry.name.empty() || entry.name == "." || entry.name == ".." ||
               entry.name.find('/') != std::string::npos || entry.name.find('\0') != std::string::npos ||
               !siblings.insert(entry.parentId + '\0' + entry.name).second) {
      return Status::InvalidArgument;
    }
    stored->metadataBytes +=
        sizeof(Entry) + entry.id.size() + entry.parentId.size() + entry.name.size() + entry.version.size();
    if (stored->metadataBytes > m_limits.maxMetadataBytes - m_usage.metadataBytes)
      return Status::ResourceLimit;
  }
  if (roots != 1)
    return Status::InvalidArgument;
  // Every parent chain must terminate at the unique root; no cycles or missing parents.
  for (const auto &entry : offer.entries) {
    const Entry *node = &entry;
    std::size_t depth = 0;
    while (!node->parentId.empty()) {
      const auto parent = stored->byId.find(node->parentId);
      if (parent == stored->byId.end() || !offer.entries[parent->second].directory || ++depth > offer.entries.size())
        return Status::InvalidArgument;
      node = &offer.entries[parent->second];
    }
  }
  if (auto current = m_offers.find(m_currentOffer); current != m_offers.end())
    current->second->acquireUntil = expiryAfter(m_now(), current->second->offer.acquisitionWindow);
  stored->offer = std::move(offer);
  m_currentOffer = stored->offer.id;
  m_publishedIds.insert(m_currentOffer);
  m_usage.retainedBytes += sizeof(std::string) + m_currentOffer.size() + nodeOverhead;
  m_entryCount += static_cast<std::uint32_t>(stored->offer.entries.size());
  m_usage.metadataBytes += stored->metadataBytes;
  m_offers.emplace(m_currentOffer, std::move(stored));
  m_usage.offers = static_cast<std::uint32_t>(m_offers.size());
  return Status::Ok;
}

LeaseResult FileService::leaseResult(const std::string &id, const Lease &lease, Status status) const
{
  return {status, id, lease.offerVersion, lease.grantedTtl, lease.state};
}

LeaseResult FileService::acquireLease(
    const RequestContext &context, const std::string &offerId, const std::string &expectedVersion,
    std::chrono::milliseconds ttl
)
{
  std::lock_guard lock(m_mutex);
  if (auto status = begin(context); status != Status::Ok)
    return {.status = status};
  if (ttl.count() <= 0)
    return {.status = Status::InvalidArgument};
  const auto found = m_offers.find(offerId);
  if (found == m_offers.end() || m_now() >= found->second->acquireUntil)
    return {.status = Status::OfferExpired};
  if (found->second->offer.version != expectedVersion)
    return {.status = Status::VersionMismatch};
  if (m_usage.activeLeases >= m_limits.maxActiveLeases || m_leases.size() >= m_limits.maxSessionObjects)
    return {.status = Status::ResourceLimit};
  ttl = std::min(ttl, m_limits.maxTtl);
  const auto id = "lease-" + std::to_string(m_nextLease++);
  const auto retained = sizeof(Lease) + id.size() + expectedVersion.size() + nodeOverhead;
  if (retained > m_limits.maxRetainedBytes - m_usage.retainedBytes)
    return {.status = Status::ResourceLimit};
  auto [it, inserted] =
      m_leases.emplace(id, Lease{found->second, expectedVersion, LeaseState::Active, expiryAfter(m_now(), ttl), ttl});
  m_usage.retainedBytes += retained;
  ++m_usage.activeLeases;
  return leaseResult(id, it->second);
}

LeaseResult FileService::renewLease(const RequestContext &context, const std::string &id, std::chrono::milliseconds ttl)
{
  std::lock_guard lock(m_mutex);
  if (auto status = begin(context); status != Status::Ok)
    return {.status = status};
  if (ttl.count() <= 0)
    return {.status = Status::InvalidArgument};
  const auto found = m_leases.find(id);
  if (found == m_leases.end())
    return {.status = Status::LeaseReleased};
  auto &lease = found->second;
  if (auto status = leaseStatus(lease, true); status != Status::Ok)
    return leaseResult(id, lease, status);
  lease.grantedTtl = std::min(ttl, m_limits.maxTtl);
  lease.expires = expiryAfter(m_now(), lease.grantedTtl);
  return leaseResult(id, lease);
}

LeaseResult FileService::releaseLease(const RequestContext &context, const std::string &id)
{
  std::lock_guard lock(m_mutex);
  if (auto status = begin(context); status != Status::Ok)
    return {.status = status};
  const auto found = m_leases.find(id);
  if (found == m_leases.end())
    return {.status = Status::LeaseReleased};
  auto &lease = found->second;
  if (lease.state == LeaseState::Active) {
    lease.state = lease.handles ? LeaseState::Draining : LeaseState::Closed;
    if (lease.state == LeaseState::Closed) {
      lease.offer.reset();
      --m_usage.activeLeases;
    }
  }
  // Release is idempotent for all known terminal states.
  return leaseResult(id, lease);
}

DirectoryResult FileService::listDirectory(
    const RequestContext &context, const std::string &leaseId, const std::string &directoryId,
    const std::string &version, std::uint32_t cursor, std::uint32_t pageSize
)
{
  std::lock_guard lock(m_mutex);
  if (auto status = begin(context); status != Status::Ok)
    return {.status = status};
  if (pageSize == 0 || pageSize > 4096)
    return {.status = Status::InvalidArgument};
  const auto lease = m_leases.find(leaseId);
  if (lease == m_leases.end())
    return {.status = Status::LeaseReleased};
  if (auto status = leaseStatus(lease->second, false); status != Status::Ok)
    return {.status = status};
  const auto &stored = *lease->second.offer;
  const auto found = stored.byId.find(directoryId);
  if (found == stored.byId.end())
    return {.status = Status::NotFound};
  const auto &directory = stored.offer.entries[found->second];
  if (!directory.directory)
    return {.status = Status::InvalidArgument};
  if (directory.version != version)
    return {.status = Status::VersionMismatch};
  DirectoryResult result;
  std::uint32_t seen = 0;
  for (const auto &entry : stored.offer.entries) {
    if (entry.parentId != directoryId)
      continue;
    if (seen++ < cursor)
      continue;
    if (result.entries.size() == pageSize) {
      result.nextCursor = seen - 1;
      break;
    }
    result.entries.push_back(entry);
  }
  if (cursor > seen)
    return {.status = Status::InvalidArgument};
  return result;
}

OpenResult FileService::open(
    const RequestContext &context, const std::string &leaseId, const std::string &fileId, const std::string &version,
    Consistency mode
)
{
  std::shared_ptr<StoredOffer> stored;
  Entry entry;
  {
    std::lock_guard lock(m_mutex);
    if (auto status = begin(context); status != Status::Ok)
      return {.status = status};
    if (mode != Consistency::Immutable && mode != Consistency::Detectable)
      return {.status = Status::InvalidArgument};
    const auto lease = m_leases.find(leaseId);
    if (lease == m_leases.end())
      return {.status = Status::LeaseReleased};
    if (auto status = leaseStatus(lease->second, false); status != Status::Ok)
      return {.status = status};
    stored = lease->second.offer;
    const auto found = stored->byId.find(fileId);
    if (found == stored->byId.end())
      return {.status = Status::NotFound};
    entry = stored->offer.entries[found->second];
    if (entry.directory)
      return {.status = Status::InvalidArgument};
    if (entry.version != version)
      return {.status = Status::VersionMismatch};
    if (mode == Consistency::Immutable && !stored->offer.source->immutable())
      return {.status = Status::SourceNotStable};
    if (m_usage.activeHandles >= m_limits.maxActiveHandles || m_handles.size() >= m_limits.maxSessionObjects ||
        m_usage.activeRequests >= m_limits.maxActiveRequests)
      return {.status = Status::ResourceLimit};
    ++m_usage.activeRequests;
  }
  Status validation;
  try {
    validation = stored->offer.source->validate(entry);
  } catch (...) {
    validation = Status::IoError;
  }
  std::lock_guard lock(m_mutex);
  --m_usage.activeRequests;
  collect();
  if (auto status = contextStatus(context); status != Status::Ok)
    return {.status = status};
  auto &lease = m_leases.at(leaseId);
  if (auto status = leaseStatus(lease, false); status != Status::Ok)
    return {.status = status};
  if (validation != Status::Ok)
    return {.status = validation};
  if (m_usage.activeHandles >= m_limits.maxActiveHandles || m_handles.size() >= m_limits.maxSessionObjects)
    return {.status = Status::ResourceLimit};
  const auto id = "handle-" + std::to_string(m_nextHandle++);
  const auto retained = sizeof(Handle) + id.size() + leaseId.size() + stored->offer.id.size() + entry.id.size() +
                        entry.parentId.size() + entry.name.size() + entry.version.size() + nodeOverhead;
  if (retained > m_limits.maxRetainedBytes - m_usage.retainedBytes)
    return {.status = Status::ResourceLimit};
  m_handles.emplace(id, Handle{leaseId, stored->offer.id, entry});
  m_usage.retainedBytes += retained;
  ++lease.handles;
  ++m_usage.activeHandles;
  return {Status::Ok, id, entry.version, entry.size};
}

ReadResult FileService::read(
    const RequestContext &context, const std::string &leaseId, const std::string &handleId, std::uint64_t offset,
    std::uint32_t length
)
{
  ReadResult result{
      .sessionEpoch = context.sessionEpoch,
      .handleId = handleId,
      .requestId = context.requestId,
      .offset = offset,
      .length = length
  };
  std::shared_ptr<StoredOffer> stored;
  Entry entry;
  {
    std::lock_guard lock(m_mutex);
    auto handle = m_handles.find(handleId);
    if (handle != m_handles.end()) {
      result.offerId = handle->second.offerId;
      result.fileId = handle->second.entry.id;
      result.version = handle->second.entry.version;
    }
    if ((result.status = begin(context)) != Status::Ok)
      return result;
    if (!length || length > m_limits.maxRequestBytes) {
      result.status = Status::InvalidArgument;
      return result;
    }
    if (handle == m_handles.end()) {
      result.status = Status::HandleClosed;
      return result;
    }
    if (handle->second.leaseId != leaseId) {
      result.status = Status::InvalidArgument;
      return result;
    }
    auto &lease = m_leases.at(leaseId);
    if (handle->second.closed && lease.state != LeaseState::Expired && lease.state != LeaseState::Revoked) {
      result.status = Status::HandleClosed;
      return result;
    }
    if ((result.status = leaseStatus(lease, true)) != Status::Ok)
      return result;
    if (handle->second.closed) {
      result.status = Status::HandleClosed;
      return result;
    }
    if (m_usage.activeRequests >= m_limits.maxActiveRequests ||
        length > m_limits.maxInFlightBytes - m_usage.inFlightBytes) {
      result.status = Status::ResourceLimit;
      return result;
    }
    stored = lease.offer;
    entry = handle->second.entry;
    ++m_usage.activeRequests;
    m_usage.inFlightBytes += length;
  }
  std::stop_source operationStop;
  std::stop_callback requestCancellation(context.stop, [&] { operationStop.request_stop(); });
  std::stop_callback sessionCancellation(m_disconnectStop.get_token(), [&] { operationStop.request_stop(); });
  auto sourceContext = context;
  sourceContext.stop = operationStop.get_token();
  SourceRead source;
  try {
    source = stored->offer.source->read(entry, offset, length, sourceContext);
  } catch (...) {
    source.status = Status::IoError;
  }
  {
    std::lock_guard lock(m_mutex);
    --m_usage.activeRequests;
    m_usage.inFlightBytes -= length;
    collect();
    if ((result.status = contextStatus(context)) != Status::Ok)
      return result;
    const auto state = m_leases.at(leaseId).state;
    if (state == LeaseState::Expired || state == LeaseState::Revoked) {
      result.status = state == LeaseState::Expired ? Status::LeaseExpired : Status::LeaseRevoked;
      return result;
    }
  }
  const auto expected = result;
  result.status = source.status;
  result.bytes = std::move(source.bytes);
  result.eof = source.eof;
  result.status = validateReadResult(result, expected, entry.size);
  if (result.status != Status::Ok) {
    std::vector<std::uint8_t>().swap(result.bytes);
    result.eof = false;
  }
  return result;
}

Status FileService::close(const RequestContext &context, const std::string &handleId)
{
  std::lock_guard lock(m_mutex);
  if (auto status = begin(context); status != Status::Ok)
    return status;
  const auto found = m_handles.find(handleId);
  if (found == m_handles.end())
    return Status::HandleClosed;
  auto &handle = found->second;
  if (handle.closed)
    return Status::Ok;
  handle.closed = true;
  --m_usage.activeHandles;
  auto &lease = m_leases.at(handle.leaseId);
  --lease.handles;
  if (lease.state == LeaseState::Draining && lease.handles == 0) {
    lease.state = LeaseState::Closed;
    lease.offer.reset();
    --m_usage.activeLeases;
  }
  return Status::Ok;
}

void FileService::disconnect()
{
  {
    std::lock_guard lock(m_mutex);
    m_connected = false;
    for (auto &[id, lease] : m_leases) {
      if (lease.state == LeaseState::Active || lease.state == LeaseState::Draining) {
        lease.state = LeaseState::Revoked;
        lease.offer.reset();
        lease.handles = 0;
      }
    }
    for (auto &[id, handle] : m_handles)
      handle.closed = true;
    m_offers.clear();
    m_usage.activeLeases = 0;
    m_usage.activeHandles = 0;
    m_usage.offers = 0;
    m_usage.metadataBytes = 0;
    m_entryCount = 0;
  }
  // Stop callbacks may be caller code; never invoke them under the state mutex.
  m_disconnectStop.request_stop();
}

Usage FileService::usage()
{
  std::lock_guard lock(m_mutex);
  collect();
  return m_usage;
}

} // namespace deskflow::clipboard
