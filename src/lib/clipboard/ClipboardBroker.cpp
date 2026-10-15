/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "ClipboardBroker.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace deskflow::clipboard {
namespace {

ClipboardBroker::Result failure(Status status)
{
  return {status, false, {}};
}

bool addBytes(std::size_t &total, std::size_t count, std::size_t limit)
{
  if (count > limit || total > limit - count)
    return false;
  total += count;
  return true;
}

} // namespace

struct ClipboardBroker::Impl
{
  enum class Phase
  {
    Local,
    Acquiring,
    Publishing,
    Published
  };

  struct Record
  {
    std::shared_ptr<const OfferAnnouncement> announcement;
    std::size_t bytes{0};
    Phase phase{Phase::Acquiring};
    std::string requestId;
    std::string leaseId;
    ClipboardOwner owner;
    std::uint32_t grantedTtlMs{0};
  };

  explicit Impl(Options value) : options(std::move(value))
  {
    if (!validId(options.localOriginId) || !validId(options.ownerId) || options.maxRetainedOffers == 0 ||
        options.maxPendingAcquires == 0 || options.maxEntriesPerOffer == 0 || options.maxRootsPerOffer == 0 ||
        options.maxManifestBytes == 0 || options.maxAnnouncementsPerSession == 0 || options.maxSessionEpochs == 0 ||
        options.requestedTtlMs == 0)
      throw std::invalid_argument("ClipboardBroker requires identities and positive budgets");
  }

  bool validId(const std::string &value) const
  {
    return !value.empty() && value.size() <= options.maxIdentifierBytes && value.find('\0') == std::string::npos;
  }

  Status validateOffer(const OfferInfo &offer, const std::string &originId, std::size_t &bytes) const
  {
    if (!validId(offer.offerId) || !validId(offer.offerVersion) || !validId(offer.sessionEpoch) || !validId(originId) ||
        offer.files.empty() || offer.roots.empty())
      return Status::InvalidArgument;
    if (offer.files.size() > options.maxEntriesPerOffer || offer.roots.size() > options.maxRootsPerOffer)
      return Status::ResourceLimit;

    bytes = 0;
    const auto charge = [&](std::size_t count) { return addBytes(bytes, count, options.maxManifestBytes); };
    if (!charge(sizeof(OfferAnnouncement)) || !charge(originId.size()) || !charge(offer.sessionEpoch.size()) ||
        !charge(offer.offerId.size()) || !charge(offer.offerVersion.size()))
      return Status::ResourceLimit;

    std::map<std::string, std::size_t> byId;
    std::set<std::pair<std::string, std::string>> siblings;
    for (std::size_t index = 0; index < offer.files.size(); ++index) {
      const auto &file = offer.files[index];
      if (!validId(file.fileId) || !validId(file.version) || !validId(file.name) || file.name == "." ||
          file.name == ".." || file.name.find('/') != std::string::npos ||
          (!file.parentId.empty() && !validId(file.parentId)) || file.relativePath.find('\0') != std::string::npos ||
          (file.kind != FileKind::File && file.kind != FileKind::Directory))
        return Status::InvalidArgument;
      if (file.size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return Status::UnsupportedSize;
      if (!byId.emplace(file.fileId, index).second || !siblings.emplace(file.parentId, file.name).second)
        return Status::InvalidArgument;
      if (!charge(sizeof(FileInfo)) || !charge(file.fileId.size()) || !charge(file.parentId.size()) ||
          !charge(file.name.size()) || !charge(file.relativePath.size()) || !charge(file.version.size()))
        return Status::ResourceLimit;
    }

    std::set<std::string> rootIds;
    std::vector<std::size_t> pending;
    for (const auto &root : offer.roots) {
      const auto found = byId.find(root);
      if (found == byId.end() || !rootIds.insert(root).second || !offer.files[found->second].parentId.empty())
        return Status::InvalidArgument;
      if (!charge(sizeof(std::string)) || !charge(root.size()))
        return Status::ResourceLimit;
      pending.push_back(found->second);
    }
    std::vector<std::vector<std::size_t>> children(offer.files.size());
    for (std::size_t index = 0; index < offer.files.size(); ++index) {
      const auto &file = offer.files[index];
      if (file.parentId.empty()) {
        if (!rootIds.contains(file.fileId))
          return Status::InvalidArgument;
      } else {
        const auto parent = byId.find(file.parentId);
        if (parent == byId.end() || offer.files[parent->second].kind != FileKind::Directory)
          return Status::InvalidArgument;
        children[parent->second].push_back(index);
      }
    }
    // Each node has one parent. Reachability from all roots rejects disconnected
    // cycles and unreachable entries without recursive stack growth.
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
      const auto &next = children[pending[cursor]];
      pending.insert(pending.end(), next.begin(), next.end());
    }
    return pending.size() == offer.files.size() ? Status::Ok : Status::InvalidArgument;
  }

  std::size_t pendingCount() const
  {
    return static_cast<std::size_t>(std::count_if(records.begin(), records.end(), [](const auto &item) {
      return item.second.phase == Phase::Acquiring;
    }));
  }

  std::optional<std::uint64_t> replaceableLocal() const
  {
    if (current) {
      const auto found = records.find(*current);
      if (found != records.end() && found->second.phase == Phase::Local)
        return current;
    }
    return std::nullopt;
  }

  bool canAdmit(std::size_t bytes, bool remote) const
  {
    const auto previous = replaceableLocal();
    const auto reclaimed = previous ? records.at(*previous).bytes : 0;
    return records.size() - (previous ? 1 : 0) < options.maxRetainedOffers &&
           bytes <= options.maxManifestBytes - (manifestBytes - reclaimed) &&
           (!remote || pendingCount() < options.maxPendingAcquires) &&
           seenOffers.size() < options.maxAnnouncementsPerSession &&
           nextSelection != std::numeric_limits<std::uint64_t>::max();
  }

  void erase(std::uint64_t revision)
  {
    const auto found = records.find(revision);
    if (found == records.end())
      return;
    manifestBytes -= found->second.bytes;
    records.erase(found);
    if (current == revision)
      current.reset();
  }

  Action action(ActionKind kind, std::uint64_t revision, const Record &record) const
  {
    const auto requestId =
        kind == ActionKind::AcquireLease || kind == ActionKind::CancelAcquire ? record.requestId : "";
    return {kind,         record.announcement,    revision,           requestId, record.leaseId,
            record.owner, options.requestedTtlMs, record.grantedTtlMs};
  }

  auto publication(const std::string &token)
  {
    return std::find_if(records.begin(), records.end(), [&](const auto &item) {
      return !token.empty() && item.second.owner.publicationToken == token;
    });
  }

  auto publication(const std::string &token) const
  {
    return std::find_if(records.begin(), records.end(), [&](const auto &item) {
      return !token.empty() && item.second.owner.publicationToken == token;
    });
  }

  Result retire(std::uint64_t revision, Status status)
  {
    const auto &record = records.at(revision);
    Result result{status, false, {}};
    if (!record.owner.publicationToken.empty())
      result.actions.push_back(action(ActionKind::RetirePublication, revision, record));
    if (!record.leaseId.empty())
      result.actions.push_back(action(ActionKind::ReleaseLease, revision, record));
    erase(revision);
    return result;
  }

  Options options;
  std::string sessionEpoch;
  std::string peerOriginId;
  std::set<std::string> seenEpochs;
  std::set<std::pair<OfferDirection, std::string>> seenOffers;
  std::map<std::uint64_t, Record> records;
  std::optional<std::uint64_t> current;
  std::uint64_t nextSelection{1};
  std::uint64_t nextLocalRevision{1};
  std::uint64_t lastRemoteRevision{0};
  std::size_t manifestBytes{0};
};

ClipboardBroker::ClipboardBroker(Options options) : m_impl(std::make_unique<Impl>(std::move(options)))
{
}

ClipboardBroker::~ClipboardBroker() = default;

ClipboardBroker::Result ClipboardBroker::beginSession(const std::string &sessionEpoch, const std::string &peerOriginId)
{
  auto &state = *m_impl;
  if (!state.validId(sessionEpoch) || !state.validId(peerOriginId) || peerOriginId == state.options.localOriginId)
    return failure(Status::InvalidArgument);
  if (state.seenEpochs.contains(sessionEpoch))
    return failure(Status::StaleSession);
  if (!state.sessionEpoch.empty())
    return failure(Status::InvalidArgument);
  if (state.seenEpochs.size() >= state.options.maxSessionEpochs)
    return failure(Status::ResourceLimit);
  state.sessionEpoch = sessionEpoch;
  state.peerOriginId = peerOriginId;
  state.seenEpochs.insert(sessionEpoch);
  state.nextLocalRevision = 1;
  state.lastRemoteRevision = 0;
  return {};
}

ClipboardBroker::Result ClipboardBroker::closeSession(const std::string &sessionEpoch)
{
  auto &state = *m_impl;
  if (state.sessionEpoch.empty() || state.sessionEpoch != sessionEpoch)
    return failure(Status::StaleSession);
  Result result;
  for (const auto &[revision, record] : state.records) {
    if (record.phase == Impl::Phase::Acquiring)
      result.actions.push_back(state.action(ActionKind::CancelAcquire, revision, record));
    if (!record.owner.publicationToken.empty())
      result.actions.push_back(state.action(ActionKind::RetirePublication, revision, record));
    if (!record.leaseId.empty())
      result.actions.push_back(state.action(ActionKind::ReleaseLease, revision, record));
  }
  state.records.clear();
  state.current.reset();
  state.manifestBytes = 0;
  state.seenOffers.clear();
  state.sessionEpoch.clear();
  state.peerOriginId.clear();
  return result;
}

ClipboardBroker::Result ClipboardBroker::observeLocal(OfferInfo offer, const ClipboardOwner &owner)
{
  auto &state = *m_impl;
  if (state.sessionEpoch.empty())
    return failure(Status::Disconnected);
  if (owner.ownerId == state.options.ownerId) {
    const auto found = state.publication(owner.publicationToken);
    if (found != state.records.end() && found->second.owner.originId == owner.originId)
      return {Status::Ok, true, {}};
    // Never turn an unrecognized self-owner event into a new local offer.
    return failure(Status::InvalidArgument);
  }
  if (!state.validId(owner.ownerId))
    return failure(Status::InvalidArgument);
  if (offer.sessionEpoch != state.sessionEpoch)
    return failure(Status::StaleSession);
  std::size_t bytes = 0;
  if (const auto status = state.validateOffer(offer, state.options.localOriginId, bytes); status != Status::Ok)
    return failure(status);
  if (state.seenOffers.contains({OfferDirection::Local, offer.offerId}))
    return failure(Status::VersionMismatch);
  if (!state.canAdmit(bytes, false) || state.nextLocalRevision == std::numeric_limits<std::uint64_t>::max())
    return failure(Status::ResourceLimit);

  if (const auto previous = state.replaceableLocal())
    state.erase(*previous);
  const auto revision = state.nextSelection++;
  auto announcement = std::make_shared<const OfferAnnouncement>(
      OfferAnnouncement{std::move(offer), state.options.localOriginId, state.nextLocalRevision++, OfferDirection::Local}
  );
  Impl::Record record{announcement, bytes, Impl::Phase::Local, {}, {}, {}};
  state.seenOffers.emplace(OfferDirection::Local, announcement->offer.offerId);
  state.records.emplace(revision, record);
  state.manifestBytes += bytes;
  state.current = revision;
  return {Status::Ok, false, {state.action(ActionKind::Announce, revision, record)}};
}

ClipboardBroker::Result ClipboardBroker::receiveRemote(OfferAnnouncement announcement)
{
  auto &state = *m_impl;
  if (state.sessionEpoch.empty())
    return failure(Status::Disconnected);
  if (announcement.offer.sessionEpoch != state.sessionEpoch)
    return failure(Status::StaleSession);
  if (announcement.direction != OfferDirection::Remote || announcement.originId != state.peerOriginId ||
      announcement.revision == 0)
    return failure(Status::InvalidArgument);
  if (announcement.revision <= state.lastRemoteRevision ||
      state.seenOffers.contains({OfferDirection::Remote, announcement.offer.offerId}))
    return failure(Status::VersionMismatch);
  std::size_t bytes = 0;
  if (const auto status = state.validateOffer(announcement.offer, announcement.originId, bytes); status != Status::Ok)
    return failure(status);
  if (!state.canAdmit(bytes, true))
    return failure(Status::ResourceLimit);

  if (const auto previous = state.replaceableLocal())
    state.erase(*previous);
  const auto revision = state.nextSelection++;
  auto value = std::make_shared<const OfferAnnouncement>(std::move(announcement));
  Impl::Record record{value, bytes, Impl::Phase::Acquiring, "acquire-" + std::to_string(revision), {}, {}};
  state.lastRemoteRevision = value->revision;
  state.seenOffers.emplace(OfferDirection::Remote, value->offer.offerId);
  state.records.emplace(revision, record);
  state.manifestBytes += bytes;
  state.current = revision;
  return {Status::Ok, false, {state.action(ActionKind::AcquireLease, revision, record)}};
}

ClipboardBroker::Result ClipboardBroker::completeAcquire(
    const std::string &sessionEpoch, const std::string &requestId, const LeaseResult &result
)
{
  auto &state = *m_impl;
  if (state.sessionEpoch.empty() || sessionEpoch != state.sessionEpoch)
    return failure(Status::StaleSession);
  const auto found = std::find_if(state.records.begin(), state.records.end(), [&](const auto &item) {
    return item.second.phase == Impl::Phase::Acquiring && item.second.requestId == requestId;
  });
  if (found == state.records.end())
    return failure(Status::InvalidSourceResponse);
  const auto revision = found->first;
  auto &record = found->second;
  if (result.status != Status::Ok) {
    state.erase(revision);
    return failure(result.status);
  }
  // A colliding lease ID must not cause a Release of another live publication.
  const bool duplicateLease = std::any_of(state.records.begin(), state.records.end(), [&](const auto &item) {
    return !result.leaseId.empty() && item.second.leaseId == result.leaseId;
  });
  if (!state.validId(result.leaseId) || duplicateLease) {
    state.erase(revision);
    return failure(Status::InvalidSourceResponse);
  }
  record.leaseId = result.leaseId;
  if (result.offerVersion != record.announcement->offer.offerVersion || result.grantedTtlMs == 0 ||
      result.state != LeaseState::Active)
    return state.retire(revision, Status::InvalidSourceResponse);
  if (state.current != revision) {
    auto ignored = state.retire(revision, Status::Ok);
    ignored.ignored = true;
    return ignored;
  }
  record.grantedTtlMs = result.grantedTtlMs;
  record.phase = Impl::Phase::Publishing;
  record.owner = {state.options.ownerId, record.announcement->originId, "publication-" + std::to_string(revision)};
  return {Status::Ok, false, {state.action(ActionKind::Publish, revision, record)}};
}

bool ClipboardBroker::canPublish(const std::string &publicationToken) const
{
  const auto &state = *m_impl;
  const auto found = state.publication(publicationToken);
  return found != state.records.end() && state.current == found->first &&
         found->second.phase == Impl::Phase::Publishing;
}

ClipboardBroker::Result ClipboardBroker::completePublication(const std::string &publicationToken, Status status)
{
  auto &state = *m_impl;
  const auto found = state.publication(publicationToken);
  if (found == state.records.end())
    return failure(Status::NotFound);
  if (found->second.phase == Impl::Phase::Published && status == Status::Ok)
    return {Status::Ok, true, {}};
  if (status != Status::Ok)
    return state.retire(found->first, status);
  if (state.current != found->first)
    return state.retire(found->first, Status::VersionMismatch);
  found->second.phase = Impl::Phase::Published;
  return {};
}

ClipboardBroker::Result ClipboardBroker::releasePublication(const std::string &publicationToken)
{
  auto &state = *m_impl;
  const auto found = state.publication(publicationToken);
  if (found == state.records.end())
    return failure(Status::NotFound);
  return state.retire(found->first, Status::Ok);
}

ClipboardBroker::Result ClipboardBroker::renewPublications() const
{
  const auto &state = *m_impl;
  if (state.sessionEpoch.empty())
    return failure(Status::Disconnected);
  Result result;
  for (const auto &[revision, record] : state.records) {
    if (!record.leaseId.empty())
      result.actions.push_back(state.action(ActionKind::RenewLease, revision, record));
  }
  return result;
}

ClipboardBroker::Result ClipboardBroker::completeRenew(const std::string &publicationToken, const LeaseResult &result)
{
  auto &state = *m_impl;
  const auto found = state.publication(publicationToken);
  if (found == state.records.end())
    return failure(Status::NotFound);
  if (result.status != Status::Ok)
    return state.retire(found->first, result.status);
  const auto &record = found->second;
  if (result.leaseId != record.leaseId || result.offerVersion != record.announcement->offer.offerVersion ||
      result.grantedTtlMs == 0 || result.state != LeaseState::Active)
    return state.retire(found->first, Status::InvalidSourceResponse);
  found->second.grantedTtlMs = result.grantedTtlMs;
  return {};
}

std::shared_ptr<const OfferAnnouncement> ClipboardBroker::currentOffer() const
{
  const auto &state = *m_impl;
  return state.current ? state.records.at(*state.current).announcement : nullptr;
}

std::vector<ClipboardBroker::Publication> ClipboardBroker::retainedPublications() const
{
  std::vector<Publication> result;
  for (const auto &[revision, record] : m_impl->records) {
    if (!record.leaseId.empty())
      result.push_back({record.announcement, revision, record.leaseId, record.owner, record.grantedTtlMs});
  }
  return result;
}

std::size_t ClipboardBroker::retainedOfferCount() const
{
  return m_impl->records.size();
}

std::size_t ClipboardBroker::pendingAcquireCount() const
{
  return m_impl->pendingCount();
}

std::size_t ClipboardBroker::retainedManifestBytes() const
{
  return m_impl->manifestBytes;
}

} // namespace deskflow::clipboard
