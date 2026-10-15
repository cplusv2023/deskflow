/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "FileTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace deskflow::clipboard {

enum class OfferDirection
{
  Local,
  Remote
};

// revision is monotonic for this origin in the authenticated session. A future
// multi-peer server adapter additionally assigns the global wire revision.
struct OfferAnnouncement
{
  OfferInfo offer;
  std::string originId;
  std::uint64_t revision{0};
  OfferDirection direction{OfferDirection::Remote};
};

// ownerId comes from the clipboard backend's actual owner identity, not from a
// MIME payload alone. publicationToken is a correlation marker, not a secret.
struct ClipboardOwner
{
  std::string ownerId;
  std::string originId;
  std::string publicationToken;
};

class ClipboardBroker
{
public:
  struct Options
  {
    std::string localOriginId;
    std::string ownerId;
    std::size_t maxRetainedOffers{32};
    std::size_t maxPendingAcquires{16};
    std::size_t maxEntriesPerOffer{4096};
    std::size_t maxRootsPerOffer{64};
    // Aggregate logical manifest storage, including strings and FileInfo slots.
    // Action lists hold shared immutable references, not further manifest copies.
    std::size_t maxManifestBytes{4 * 1024 * 1024};
    // Bound replay bookkeeping too. Exhaustion requires a new broker instance.
    std::size_t maxAnnouncementsPerSession{4096};
    std::size_t maxSessionEpochs{256};
    std::size_t maxIdentifierBytes{256};
    std::uint32_t requestedTtlMs{30000};
  };

  enum class ActionKind
  {
    Announce,
    AcquireLease,
    Publish,
    RenewLease,
    ReleaseLease,
    RetirePublication,
    CancelAcquire
  };

  struct Action
  {
    ActionKind kind;
    std::shared_ptr<const OfferAnnouncement> announcement;
    std::uint64_t selectionRevision{0};
    // Broker correlation ID for AcquireLease/CancelAcquire only. The transport
    // allocates fresh wire request IDs for every renewal/release and correlates
    // their responses before calling completeRenew.
    std::string requestId;
    std::string leaseId;
    ClipboardOwner owner;
    std::uint32_t requestedTtlMs{0};
    std::uint32_t grantedTtlMs{0};
  };

  struct Result
  {
    Status status{Status::Ok};
    bool ignored{false};
    std::vector<Action> actions;
  };

  struct Publication
  {
    std::shared_ptr<const OfferAnnouncement> announcement;
    std::uint64_t selectionRevision{0};
    std::string leaseId;
    ClipboardOwner owner;
    std::uint32_t grantedTtlMs{0};
  };

  explicit ClipboardBroker(Options options);
  ~ClipboardBroker();
  ClipboardBroker(const ClipboardBroker &) = delete;
  ClipboardBroker &operator=(const ClipboardBroker &) = delete;

  // Single-dispatcher actor: serialize all methods and apply returned actions on
  // that dispatcher. No method calls platform code, network code, or callbacks.
  // This M4 component owns one authenticated peer session, not a server topology.
  Result beginSession(const std::string &sessionEpoch, const std::string &peerOriginId);
  Result closeSession(const std::string &sessionEpoch);

  // External owner events always make a fresh announcement, even for identical
  // content. Source offers must have new identities and the active session epoch.
  Result observeLocal(OfferInfo offer, const ClipboardOwner &owner);
  Result receiveRemote(OfferAnnouncement announcement);
  Result completeAcquire(const std::string &sessionEpoch, const std::string &requestId, const LeaseResult &result);

  // Before an asynchronous backend finally claims ownership, recheck this guard
  // on the dispatcher. A superseded Publish action must be completed CANCELLED.
  bool canPublish(const std::string &publicationToken) const;
  Result completePublication(const std::string &publicationToken, Status status);
  Result releasePublication(const std::string &publicationToken);

  // A timer in the runtime requests renewal for ALL retained publications,
  // including old directories without currently open handles. It owns deadlines.
  Result renewPublications() const;
  Result completeRenew(const std::string &publicationToken, const LeaseResult &result);

  std::shared_ptr<const OfferAnnouncement> currentOffer() const;
  std::vector<Publication> retainedPublications() const;
  std::size_t retainedOfferCount() const;
  std::size_t pendingAcquireCount() const;
  std::size_t retainedManifestBytes() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace deskflow::clipboard
