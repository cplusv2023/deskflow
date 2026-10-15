/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace deskflow::clipboard::experimental {

// Identity and advertised formats only. Payloads, file leases, and retained old
// offers belong to the caller/FileService, not to the current-clipboard broker.
// origin includes the source process epoch; sequence is monotonic within it.
struct ClipboardAnnouncement
{
  std::string offerId;
  std::string offerVersion;
  std::string origin;
  std::uint64_t sequence{0};
  std::vector<std::string> formats;
  bool operator==(const ClipboardAnnouncement &) const = default;
};

struct PublicationMarker
{
  std::string token;
  std::uint64_t revision{0};
  bool operator==(const PublicationMarker &) const = default;
};

struct PublicationTicket
{
  ClipboardAnnouncement announcement;
  PublicationMarker marker;
  bool operator==(const PublicationTicket &) const = default;
};

enum class BrokerStatus
{
  Accepted,
  Duplicate,
  OwnPublication,
  LocalOrigin,
  OutOfOrder,
  StalePublication,
  InvalidAnnouncement,
  Disconnected,
  ResourceLimit,
  BackendFailure
};

struct RemoteAnnouncementResult
{
  BrokerStatus status;
  std::optional<PublicationTicket> publication;
};

struct LocalObservationResult
{
  BrokerStatus status;
  std::optional<ClipboardAnnouncement> announcement;
};

struct BrokerLimits
{
  std::uint32_t maxOrigins{256};
  std::uint32_t maxFormats{64};
  std::uint32_t maxMetadataBytes{65536};
};

// Called synchronously on the platform clipboard's owner thread. The adapter
// writes marker with the representations and returns their resulting generation
// (e.g. NSPasteboard changeCount), or nullopt on failure. It must not call back
// into the broker. Snapshot/observation callbacks are queued after publication.
// A file adapter must prepare its projection and acquire the whole-offer lease
// before this call. An empty format list represents an empty clipboard.
class ClipboardBackend
{
public:
  virtual ~ClipboardBackend() = default;
  virtual std::optional<std::uint64_t>
  publish(const ClipboardAnnouncement &announcement, const PublicationMarker &marker) noexcept = 0;
};

// All methods, including backend publication, run on one caller-owned serial
// executor (on macOS, the AppKit owner thread). Network/file workers post their
// results there. This class does not create threads, run I/O, or own a backend.
// In particular, do not check a ticket on one thread and write the clipboard on
// another: publishIfCurrent makes the check and synchronous write one operation.
//
// One instance is used for one connected session. disconnect() is terminal;
// reconnect creates a new instance, with fresh session/publication identity.
class ClipboardBroker
{
public:
  // localOrigin must include a process epoch; publicationToken is a fresh opaque
  // nonce supplied by the platform, never a content hash or a stable device ID.
  ClipboardBroker(std::string localOrigin, std::string publicationToken, BrokerLimits limits = {});

  RemoteAnnouncementResult receiveRemote(ClipboardAnnouncement announcement);
  BrokerStatus publishIfCurrent(const PublicationTicket &ticket, ClipboardBackend &backend);

  // A complete platform snapshot. Same content at a new generation is a new
  // copy operation. An echoed marker alone does not prove our own publication.
  LocalObservationResult observeLocal(
      std::uint64_t generation, std::optional<PublicationMarker> marker, std::string offerId, std::string offerVersion,
      std::vector<std::string> formats
  );

  // Invalidates pending work and current selection without writing the platform
  // clipboard or releasing old leases. Origin high-water marks remain intact.
  // Retaining old leases is necessary for directory copies still in progress.
  void clear();
  void disconnect();

  const std::optional<ClipboardAnnouncement> &current() const;
  std::uint64_t revision() const;
  bool connected() const;

private:
  struct PublishedClipboard
  {
    PublicationMarker marker;
    std::uint64_t generation;
  };

  bool valid(const ClipboardAnnouncement &announcement) const;
  bool canAdvance() const;
  void advance();

  std::string m_localOrigin;
  std::string m_publicationToken;
  BrokerLimits m_limits;
  bool m_connected{true};
  std::uint64_t m_revision{0};
  std::uint64_t m_localSequence{0};
  std::unordered_map<std::string, std::uint64_t> m_remoteSequences;
  std::optional<ClipboardAnnouncement> m_current;
  std::optional<PublicationTicket> m_pending;
  std::optional<PublishedClipboard> m_published;
  std::optional<std::uint64_t> m_lastObservedGeneration;
};

} // namespace deskflow::clipboard::experimental
