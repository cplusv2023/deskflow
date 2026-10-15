/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "ClipboardBroker.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace deskflow::clipboard {

ClipboardBroker::ClipboardBroker(std::string localOrigin, std::string publicationToken, BrokerLimits limits)
    : m_localOrigin(std::move(localOrigin)),
      m_publicationToken(std::move(publicationToken)),
      m_limits(limits)
{
  if (m_localOrigin.empty() || m_publicationToken.empty() || !m_limits.maxOrigins || !m_limits.maxFormats ||
      !m_limits.maxMetadataBytes || m_localOrigin.size() > m_limits.maxMetadataBytes ||
      m_publicationToken.size() > m_limits.maxMetadataBytes) {
    throw std::invalid_argument("invalid clipboard broker identity or limits");
  }
}

bool ClipboardBroker::valid(const ClipboardAnnouncement &announcement) const
{
  if (announcement.offerId.empty() || announcement.offerVersion.empty() || announcement.origin.empty() ||
      !announcement.sequence || announcement.formats.size() > m_limits.maxFormats) {
    return false;
  }
  std::size_t remaining = m_limits.maxMetadataBytes;
  const auto consume = [&remaining](const std::string &value) {
    if (value.size() > remaining) {
      return false;
    }
    remaining -= value.size();
    return true;
  };
  if (!consume(announcement.offerId) || !consume(announcement.offerVersion) || !consume(announcement.origin)) {
    return false;
  }
  for (const auto &format : announcement.formats) {
    if (format.empty() || !consume(format)) {
      return false;
    }
  }
  return true;
}

bool ClipboardBroker::canAdvance() const
{
  return m_revision != std::numeric_limits<std::uint64_t>::max();
}

void ClipboardBroker::advance()
{
  ++m_revision;
  m_pending.reset();
}

RemoteAnnouncementResult ClipboardBroker::receiveRemote(ClipboardAnnouncement announcement)
{
  if (!m_connected) {
    return {BrokerStatus::Disconnected, std::nullopt};
  }
  if (!valid(announcement)) {
    return {BrokerStatus::InvalidAnnouncement, std::nullopt};
  }
  if (announcement.origin == m_localOrigin) {
    return {BrokerStatus::LocalOrigin, std::nullopt};
  }
  const auto found = m_remoteSequences.find(announcement.origin);
  if (found != m_remoteSequences.end() && announcement.sequence <= found->second) {
    return {announcement.sequence == found->second ? BrokerStatus::Duplicate : BrokerStatus::OutOfOrder, std::nullopt};
  }
  if (!canAdvance() || (found == m_remoteSequences.end() && m_remoteSequences.size() >= m_limits.maxOrigins)) {
    return {BrokerStatus::ResourceLimit, std::nullopt};
  }

  // Different origins are ordered by local arrival, never by remote wall clocks
  // or by comparing unrelated origin sequence numbers.
  m_remoteSequences[announcement.origin] = announcement.sequence;
  advance();
  m_current = std::move(announcement);
  m_pending = PublicationTicket{*m_current, {m_publicationToken, m_revision}};
  return {BrokerStatus::Accepted, m_pending};
}

BrokerStatus ClipboardBroker::publishIfCurrent(const PublicationTicket &ticket, ClipboardBackend &backend)
{
  if (!m_connected) {
    return BrokerStatus::Disconnected;
  }
  if (!m_pending || ticket != *m_pending) {
    return BrokerStatus::StalePublication;
  }
  const auto generation = backend.publish(ticket.announcement, ticket.marker);
  if (!generation) {
    return BrokerStatus::BackendFailure;
  }
  m_published = PublishedClipboard{ticket.marker, *generation};
  m_pending.reset();
  return BrokerStatus::Accepted;
}

LocalObservationResult ClipboardBroker::observeLocal(
    std::uint64_t generation, std::optional<PublicationMarker> marker, std::string offerId, std::string offerVersion,
    std::vector<std::string> formats
)
{
  if (!m_connected) {
    return {BrokerStatus::Disconnected, std::nullopt};
  }
  if (m_published && marker && generation == m_published->generation && *marker == m_published->marker) {
    m_lastObservedGeneration = generation;
    return {BrokerStatus::OwnPublication, std::nullopt};
  }
  if (m_lastObservedGeneration && generation == *m_lastObservedGeneration) {
    return {BrokerStatus::Duplicate, std::nullopt};
  }
  if (!canAdvance() || m_localSequence == std::numeric_limits<std::uint64_t>::max()) {
    return {BrokerStatus::ResourceLimit, std::nullopt};
  }

  ClipboardAnnouncement announcement{
      std::move(offerId), std::move(offerVersion), m_localOrigin, m_localSequence + 1, std::move(formats)
  };
  if (!valid(announcement)) {
    return {BrokerStatus::InvalidAnnouncement, std::nullopt};
  }
  ++m_localSequence;
  advance();
  m_lastObservedGeneration = generation;
  m_published.reset();
  m_current = std::move(announcement);
  return {BrokerStatus::Accepted, m_current};
}

void ClipboardBroker::clear()
{
  m_pending.reset();
  m_current.reset();
  // No ticket can remain valid at exhaustion. Future arrivals fail closed.
  if (canAdvance()) {
    ++m_revision;
  }
}

void ClipboardBroker::disconnect()
{
  if (!m_connected) {
    return;
  }
  clear();
  m_connected = false;
  m_published.reset();
  m_lastObservedGeneration.reset();
  m_remoteSequences.clear();
}

const std::optional<ClipboardAnnouncement> &ClipboardBroker::current() const
{
  return m_current;
}

std::uint64_t ClipboardBroker::revision() const
{
  return m_revision;
}

bool ClipboardBroker::connected() const
{
  return m_connected;
}

} // namespace deskflow::clipboard
