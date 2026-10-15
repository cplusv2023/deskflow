/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace deskflow::clipboard {

enum class Status
{
  Ok,
  InvalidArgument,
  UnsupportedSize,
  NotFound,
  VersionMismatch,
  SourceChanged,
  SourceMissing,
  SourceNotStable,
  PermissionDenied,
  OfferExpired,
  LeaseExpired,
  LeaseReleased,
  LeaseRevoked,
  HandleClosed,
  Disconnected,
  StaleSession,
  ResourceLimit,
  InvalidSourceResponse,
  Timeout,
  Cancelled,
  IoError,
};

inline std::string_view statusName(Status status)
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
  case Status::OfferExpired:
    return "OFFER_EXPIRED";
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
  case Status::ResourceLimit:
    return "RESOURCE_LIMIT";
  case Status::InvalidSourceResponse:
    return "INVALID_SOURCE_RESPONSE";
  case Status::Timeout:
    return "TIMEOUT";
  case Status::Cancelled:
    return "CANCELLED";
  case Status::IoError:
    return "IO_ERROR";
  }
  return "IO_ERROR";
}

enum class ConsistencyMode
{
  Immutable,
  Detectable
};
enum class FileKind
{
  File,
  Directory
};
enum class LeaseState
{
  Active,
  Draining,
  Closed,
  Expired,
  Revoked
};

inline std::string_view leaseStateName(LeaseState state)
{
  switch (state) {
  case LeaseState::Active:
    return "ACTIVE";
  case LeaseState::Draining:
    return "DRAINING";
  case LeaseState::Closed:
    return "CLOSED";
  case LeaseState::Expired:
    return "EXPIRED";
  case LeaseState::Revoked:
    return "REVOKED";
  }
  return "REVOKED";
}

struct FileInfo
{
  std::string fileId;
  std::string parentId; // Empty for a selected root.
  std::string name;
  std::string relativePath; // Display path under the offer; never accepted as authority.
  std::string version;
  FileKind kind{FileKind::File};
  std::uint64_t size{0};
};

struct OfferInfo
{
  std::string sessionEpoch;
  std::string offerId;
  std::string offerVersion;
  std::vector<std::string> roots;
  std::vector<FileInfo> files; // Fixed, bounded manifest including every descendant.
  std::uint32_t acquisitionWindowMs{30000};
  bool immutableVersion{false}; // Ordinary local files are detectable only.
};

struct OfferResult
{
  Status status{Status::IoError};
  OfferInfo offer;
};
struct StatResult
{
  Status status{Status::IoError};
  FileInfo file;
};
struct ListResult
{
  Status status{Status::IoError};
  std::vector<FileInfo> entries;
};

struct LeaseResult
{
  Status status{Status::IoError};
  std::string leaseId;
  std::string offerVersion;
  std::uint32_t grantedTtlMs{0};
  LeaseState state{LeaseState::Closed};
};

struct OpenResult
{
  Status status{Status::IoError};
  std::string handleId;
  std::string version;
  std::uint64_t size{0};
};

struct ReadRequest
{
  std::string sessionEpoch;
  std::string leaseId;
  std::string handleId;
  std::string requestId;
  std::uint64_t offset{0};
  std::uint32_t length{0};
};

struct ReadResult
{
  Status status{Status::IoError};
  std::string sessionEpoch;
  std::string offerId;
  std::string fileId;
  std::string version;
  std::string leaseId;
  std::string handleId;
  std::string requestId;
  std::uint64_t offset{0};
  std::uint32_t length{0};
  std::vector<std::uint8_t> bytes;
  bool eof{false}; // Meaningful only when status == Ok.
};

} // namespace deskflow::clipboard
