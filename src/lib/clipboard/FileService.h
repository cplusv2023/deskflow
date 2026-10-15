/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace deskflow::clipboard {

enum class Status
{
  Ok,
  InvalidArgument,
  UnsupportedSize,
  NotFound,
  OfferExpired,
  VersionMismatch,
  SourceChanged,
  SourceMissing,
  SourceNotStable,
  PermissionDenied,
  LeaseExpired,
  LeaseReleased,
  LeaseRevoked,
  HandleClosed,
  Disconnected,
  StaleSession,
  Timeout,
  Cancelled,
  InvalidSourceResponse,
  ResourceLimit,
  IoError
};

const char *statusName(Status status);
int statusErrno(Status status);

enum class LeaseState
{
  Active,
  Draining,
  Closed,
  Expired,
  Revoked
};
enum class Consistency
{
  Immutable,
  Detectable
};
using Clock = std::chrono::steady_clock;

struct RequestContext
{
  std::string sessionEpoch;
  std::string requestId;
  Clock::time_point deadline{Clock::time_point::max()};
  std::stop_token stop;
};

struct Entry
{
  std::string id;
  std::string parentId; // Empty only for the virtual root.
  std::string name;     // One path component, never a local absolute path.
  std::string version;
  bool directory{false};
  std::uint64_t size{0};
};

struct SourceRead
{
  Status status{Status::Ok};
  std::vector<std::uint8_t> bytes;
  bool eof{false};
};

// Sources must be safe for concurrent reads, preserve captured object identities,
// and cooperate with cancellation/deadlines. The service never holds its state
// mutex during read(). A blocking source cannot be forcibly interrupted here.
class FileSource
{
public:
  virtual ~FileSource() = default;
  virtual bool immutable() const = 0;
  virtual Status validate(const Entry &entry) = 0;
  virtual SourceRead
  read(const Entry &entry, std::uint64_t offset, std::uint32_t length, const RequestContext &context) = 0;
};

struct Offer
{
  std::string id;
  std::string version;
  std::vector<Entry> entries;
  std::shared_ptr<FileSource> source;
  std::chrono::milliseconds acquisitionWindow{30000};
};

struct Limits
{
  std::uint32_t maxRequestBytes{262144};
  std::uint32_t maxActiveRequests{8};
  std::uint64_t maxInFlightBytes{2 * 1024 * 1024};
  std::uint32_t maxActiveLeases{128};
  std::uint32_t maxActiveHandles{512};
  std::uint32_t maxOffers{256};
  std::uint32_t maxEntries{65536};
  std::uint64_t maxMetadataBytes{16 * 1024 * 1024};
  std::uint64_t maxRetainedBytes{32 * 1024 * 1024}; // Requests, IDs, leases and handle tombstones.
  // Bounded tombstones and replay protection; a fresh session is required when
  // exhausted. These are lifetime limits, not silently evictable LRU caches.
  std::uint32_t maxSessionObjects{131072};
  std::uint32_t maxSessionRequests{1048576};
  std::chrono::milliseconds maxTtl{30000};
};

struct LeaseResult
{
  Status status{Status::Ok};
  std::string leaseId;
  std::string offerVersion;
  std::chrono::milliseconds grantedTtl{0};
  LeaseState state{LeaseState::Closed};
};

struct OpenResult
{
  Status status{Status::Ok};
  std::string handleId;
  std::string version;
  std::uint64_t size{0};
};

struct ReadResult
{
  Status status{Status::Ok};
  std::string sessionEpoch;
  std::string offerId;
  std::string fileId;
  std::string version;
  std::string handleId;
  std::string requestId;
  std::uint64_t offset{0};
  std::uint32_t length{0};
  std::vector<std::uint8_t> bytes;
  bool eof{false};
};

struct DirectoryResult
{
  Status status{Status::Ok};
  std::vector<Entry> entries;
  std::uint32_t nextCursor{0}; // Zero means no further page.
};

struct Usage
{
  std::uint32_t activeRequests{0};
  std::uint64_t inFlightBytes{0};
  std::uint32_t activeLeases{0};
  std::uint32_t activeHandles{0};
  std::uint32_t offers{0};
  std::uint64_t metadataBytes{0};
  std::uint64_t retainedBytes{0};
};

// One instance belongs to one already-authenticated peer session. The transport
// must not share an instance between principals or resurrect it after disconnect.
// Callers must disconnect and join all outstanding calls before destruction.
class FileService
{
public:
  explicit FileService(
      std::string sessionEpoch, Limits limits = {}, std::function<Clock::time_point()> now = Clock::now
  );
  ~FileService();

  Status publishOffer(Offer offer);
  LeaseResult acquireLease(
      const RequestContext &, const std::string &offerId, const std::string &expectedVersion,
      std::chrono::milliseconds requestedTtl = std::chrono::milliseconds(30000)
  );
  LeaseResult renewLease(
      const RequestContext &, const std::string &leaseId,
      std::chrono::milliseconds requestedTtl = std::chrono::milliseconds(30000)
  );
  LeaseResult releaseLease(const RequestContext &, const std::string &leaseId);
  DirectoryResult listDirectory(
      const RequestContext &, const std::string &leaseId, const std::string &directoryId,
      const std::string &expectedVersion, std::uint32_t cursor = 0, std::uint32_t pageSize = 256
  );
  OpenResult open(
      const RequestContext &, const std::string &leaseId, const std::string &fileId, const std::string &expectedVersion,
      Consistency mode = Consistency::Immutable
  );
  ReadResult read(
      const RequestContext &, const std::string &leaseId, const std::string &handleId, std::uint64_t offset,
      std::uint32_t length
  );
  Status close(const RequestContext &, const std::string &handleId);
  void disconnect();
  Usage usage();

private:
  struct StoredOffer;
  struct Lease;
  struct Handle;
  Status begin(const RequestContext &);
  Status contextStatus(const RequestContext &) const;
  Status leaseStatus(const Lease &, bool allowDraining) const;
  void collect();
  LeaseResult leaseResult(const std::string &id, const Lease &, Status status = Status::Ok) const;
  std::string m_sessionEpoch;
  Limits m_limits;
  std::function<Clock::time_point()> m_now;
  std::mutex m_mutex;
  bool m_connected{true};
  std::stop_source m_disconnectStop;
  std::string m_currentOffer;
  std::uint64_t m_nextLease{1};
  std::uint64_t m_nextHandle{1};
  std::unordered_map<std::string, std::shared_ptr<StoredOffer>> m_offers;
  std::unordered_map<std::string, Lease> m_leases;
  std::unordered_map<std::string, Handle> m_handles;
  std::unordered_set<std::string> m_requests;
  std::unordered_set<std::string> m_publishedIds;
  Usage m_usage;
  std::uint32_t m_entryCount{0};
};

// Validates a full received response, including all correlation fields. Transport
// adapters must call this before returning data to a filesystem consumer.
Status validateReadResult(const ReadResult &result, const ReadResult &expected, std::uint64_t fixedSize);

} // namespace deskflow::clipboard
