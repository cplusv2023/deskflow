/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "ClipboardBroker.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace deskflow::clipboard;

namespace {

using Kind = ClipboardBroker::ActionKind;
using Result = ClipboardBroker::Result;

void check(bool value, const std::string &message)
{
  if (!value)
    throw std::runtime_error(message);
}

void expect(Status actual, Status expected, const std::string &message)
{
  check(
      actual == expected,
      message + ": expected " + std::string(statusName(expected)) + ", got " + std::string(statusName(actual))
  );
}

ClipboardBroker::Options options()
{
  ClipboardBroker::Options value;
  value.localOriginId = "ubuntu";
  value.ownerId = "deskflow-x11-owner";
  return value;
}

void begin(ClipboardBroker &broker, const std::string &epoch = "epoch-1")
{
  expect(broker.beginSession(epoch, "mac").status, Status::Ok, "begin session");
}

OfferInfo offer(std::string id, const std::string &epoch = "epoch-1")
{
  OfferInfo value;
  value.sessionEpoch = epoch;
  value.offerId = std::move(id);
  value.offerVersion = "manifest-v1";
  value.immutableVersion = true;
  value.roots = {"dir"};
  value.files = {
      {"dir", "", "directory", "directory", "dir-v1", FileKind::Directory, 0},
      {"first", "dir", "first.bin", "directory/first.bin", "file-v1", FileKind::File, 4096},
      {"later", "dir", "later.bin", "directory/later.bin", "file-v1", FileKind::File, 8192},
  };
  return value;
}

OfferAnnouncement remote(OfferInfo value, std::uint64_t revision)
{
  return {std::move(value), "mac", revision, OfferDirection::Remote};
}

LeaseResult lease(const std::string &id, const std::string &version = "manifest-v1", std::uint32_t ttl = 30000)
{
  return {Status::Ok, id, version, ttl, LeaseState::Active};
}

std::size_t count(const Result &result, Kind kind)
{
  return static_cast<std::size_t>(std::count_if(result.actions.begin(), result.actions.end(), [&](const auto &action) {
    return action.kind == kind;
  }));
}

ClipboardBroker::Action action(const Result &result, Kind kind)
{
  check(count(result, kind) == 1, "expected exactly one action of kind " + std::to_string(static_cast<int>(kind)));
  return *std::find_if(result.actions.begin(), result.actions.end(), [&](const auto &item) {
    return item.kind == kind;
  });
}

ClipboardBroker::Action publish(ClipboardBroker &broker, std::string id, std::uint64_t revision)
{
  const auto accepted = broker.receiveRemote(remote(offer(id), revision));
  expect(accepted.status, Status::Ok, "receive " + id);
  const auto acquire = action(accepted, Kind::AcquireLease);
  check(count(accepted, Kind::Publish) == 0, "publication must await successful lease");
  const auto acquired = broker.completeAcquire("epoch-1", acquire.requestId, lease("lease-" + id));
  expect(acquired.status, Status::Ok, "acquire " + id);
  const auto publication = action(acquired, Kind::Publish);
  check(broker.canPublish(publication.owner.publicationToken), "publication is current");
  expect(
      broker.completePublication(publication.owner.publicationToken, Status::Ok).status, Status::Ok, "publish " + id
  );
  return publication;
}

void leaseBeforePublication()
{
  ClipboardBroker broker(options());
  begin(broker);
  const auto received = broker.receiveRemote(remote(offer("A"), 1));
  expect(received.status, Status::Ok, "receive");
  check(received.actions.size() == 1 && count(received, Kind::Publish) == 0, "only lease acquisition emitted");
  check(broker.retainedPublications().empty(), "no file entry published before acquisition");
  const auto acquire = action(received, Kind::AcquireLease);
  check(acquire.requestedTtlMs == 30000, "requested lease TTL exposed");
  const auto completed = broker.completeAcquire("epoch-1", acquire.requestId, lease("lease-A", "manifest-v1", 9000));
  const auto publication = action(completed, Kind::Publish);
  check(publication.grantedTtlMs == 9000, "runtime receives actual granted TTL");
  check(publication.announcement->offer.files.size() == 3, "whole directory manifest retained");
  check(broker.retainedPublications().front().grantedTtlMs == 9000, "actual TTL in retention record");
}

void lateAcquisitionCannotReplaceNewerOffer()
{
  ClipboardBroker broker(options());
  begin(broker);
  const auto requestA = action(broker.receiveRemote(remote(offer("A"), 1)), Kind::AcquireLease);
  const auto requestB = action(broker.receiveRemote(remote(offer("B"), 2)), Kind::AcquireLease);
  const auto publishedB =
      action(broker.completeAcquire("epoch-1", requestB.requestId, lease("lease-B")), Kind::Publish);
  expect(broker.completePublication(publishedB.owner.publicationToken, Status::Ok).status, Status::Ok, "publish B");
  const auto lateA = broker.completeAcquire("epoch-1", requestA.requestId, lease("lease-A"));
  check(
      lateA.ignored && count(lateA, Kind::Publish) == 0 && count(lateA, Kind::ReleaseLease) == 1,
      "late lease A released without publication"
  );
  check(broker.currentOffer()->offer.offerId == "B", "B remains selected");
  check(broker.retainedOfferCount() == 1 && broker.pendingAcquireCount() == 0, "superseded pending A removed");

  const auto requestC = action(broker.receiveRemote(remote(offer("C"), 3)), Kind::AcquireLease);
  broker.observeLocal(offer("local-D"), {"application-owner", {}, {}});
  const auto lateC = broker.completeAcquire("epoch-1", requestC.requestId, lease("lease-C"));
  check(
      lateC.ignored && count(lateC, Kind::Publish) == 0 && count(lateC, Kind::ReleaseLease) == 1,
      "late remote acquisition cannot cover newer local user copy"
  );
  check(broker.currentOffer()->offer.offerId == "local-D", "new local selection preserved");
}

void delayedBackendPublicationMustRecheckSelection()
{
  ClipboardBroker broker(options());
  begin(broker);
  const auto requestA = action(broker.receiveRemote(remote(offer("A"), 1)), Kind::AcquireLease);
  const auto publicationA =
      action(broker.completeAcquire("epoch-1", requestA.requestId, lease("lease-A")), Kind::Publish);
  broker.receiveRemote(remote(offer("B"), 2));
  check(!broker.canPublish(publicationA.owner.publicationToken), "deferred A must not claim the clipboard");
  const auto cancelled = broker.completePublication(publicationA.owner.publicationToken, Status::Cancelled);
  expect(cancelled.status, Status::Cancelled, "discard deferred A");
  check(
      count(cancelled, Kind::ReleaseLease) == 1 && broker.currentOffer()->offer.offerId == "B",
      "discarding A leaves B selected"
  );
}

void ownerMarkersAndIdenticalUserCopies()
{
  ClipboardBroker broker(options());
  begin(broker);
  const auto published = publish(broker, "A", 1);
  const auto echo = broker.observeLocal({}, published.owner);
  check(echo.ignored && echo.actions.empty(), "self owner, origin and token suppress exact publication echo");
  auto badMarker = published.owner;
  badMarker.originId = "other-device";
  expect(broker.observeLocal({}, badMarker).status, Status::InvalidArgument, "wrong origin is not a valid self event");
  badMarker = published.owner;
  badMarker.publicationToken = "unknown-token";
  expect(broker.observeLocal({}, badMarker).status, Status::InvalidArgument, "unknown self token rejected");

  // An application may preserve the MIME marker when copying the same content.
  // Its actual clipboard owner changed, so this is still a new user operation.
  auto user = published.owner;
  user.ownerId = "files-window-owner";
  const auto copied = broker.observeLocal(offer("local-A"), user);
  expect(copied.status, Status::Ok, "application copies identical directory");
  check(!copied.ignored && action(copied, Kind::Announce).announcement->originId == "ubuntu", "new local origin");
  const auto copiedAgain = broker.observeLocal(offer("local-B"), user);
  check(
      !copiedAgain.ignored && action(copiedAgain, Kind::Announce).announcement->revision == 2,
      "same owner copies identical content again: no content-hash suppression"
  );
  expect(
      broker.observeLocal(offer("local-B"), user).status, Status::VersionMismatch, "offer identities cannot be reused"
  );
  check(broker.retainedPublications().size() == 1, "local copies do not retire old published remote directory");
}

void oldDirectoryAndRenewalAreRetained()
{
  ClipboardBroker broker(options());
  begin(broker);
  const auto a = publish(broker, "A", 1);
  const auto b = publish(broker, "B", 2);
  const auto retained = broker.retainedPublications();
  check(retained.size() == 2, "A remains after B replaces clipboard");
  check(
      broker.observeLocal({}, a.owner).ignored && broker.currentOffer()->offer.offerId == "B",
      "late owner notification for retained A cannot replace B"
  );
  check(retained.front().announcement->offer.files.back().fileId == "later", "unopened directory descendant retained");
  const auto renew = broker.renewPublications();
  check(count(renew, Kind::RenewLease) == 2, "renew every old and current publication, independent of open handles");
  auto renewed = lease(a.leaseId, "manifest-v1", 12000);
  expect(broker.completeRenew(a.owner.publicationToken, renewed).status, Status::Ok, "renew A");
  check(broker.retainedPublications().front().grantedTtlMs == 12000, "renewed TTL exposed");
  const auto released = broker.releasePublication(a.owner.publicationToken);
  check(
      count(released, Kind::RetirePublication) == 1 && action(released, Kind::ReleaseLease).leaseId == a.leaseId,
      "only explicit release retires A and releases its whole-offer lease"
  );
  check(broker.currentOffer()->offer.offerId == "B" && broker.retainedPublications().size() == 1, "B unaffected");
  expect(
      broker.completePublication(b.owner.publicationToken, Status::Ok).status, Status::Ok, "publication ack idempotent"
  );
}

void sessionRevisionAndDirectionValidation()
{
  ClipboardBroker broker(options());
  expect(broker.receiveRemote(remote(offer("A"), 1)).status, Status::Disconnected, "no active session");
  begin(broker);
  expect(
      broker.receiveRemote(remote(offer("wrong-epoch", "epoch-other"), 1)).status, Status::StaleSession, "wrong epoch"
  );
  auto wrong = remote(offer("wrong-origin"), 1);
  wrong.originId = "ubuntu";
  expect(broker.receiveRemote(wrong).status, Status::InvalidArgument, "remote cannot claim own origin");
  wrong.originId = "mac";
  wrong.direction = OfferDirection::Local;
  expect(broker.receiveRemote(wrong).status, Status::InvalidArgument, "wrong direction");
  expect(broker.receiveRemote(remote(offer("zero"), 0)).status, Status::InvalidArgument, "zero revision");
  expect(broker.receiveRemote(remote(offer("A"), 7)).status, Status::Ok, "first revision need not be one");
  expect(broker.receiveRemote(remote(offer("replay"), 7)).status, Status::VersionMismatch, "duplicate revision");
  expect(broker.receiveRemote(remote(offer("older"), 6)).status, Status::VersionMismatch, "out-of-order revision");
  expect(
      broker.receiveRemote(remote(offer("A"), 8)).status, Status::VersionMismatch, "offer ID cannot acquire new meaning"
  );
  check(
      broker.currentOffer()->offer.offerId == "A" && broker.pendingAcquireCount() == 1, "invalid events have no effect"
  );
}

void retainedOfferBudgetDoesNotEvictOldDirectories()
{
  auto limited = options();
  limited.maxRetainedOffers = 1;
  ClipboardBroker broker(limited);
  begin(broker);
  const auto a = publish(broker, "A", 1);
  const auto rejected = broker.receiveRemote(remote(offer("B"), 2));
  expect(rejected.status, Status::ResourceLimit, "old publication consumes a real budget slot");
  check(rejected.actions.empty() && broker.currentOffer()->offer.offerId == "A", "budget failure does not revoke A");
  broker.releasePublication(a.owner.publicationToken);
  check(broker.retainedManifestBytes() == 0, "explicit release returns manifest budget");
  expect(
      broker.receiveRemote(remote(offer("B"), 2)).status, Status::Ok,
      "rejected revision can retry after explicit release"
  );
}

void pendingAndManifestBudgets()
{
  auto limited = options();
  limited.maxPendingAcquires = 1;
  ClipboardBroker broker(limited);
  begin(broker);
  const auto request = action(broker.receiveRemote(remote(offer("A"), 1)), Kind::AcquireLease);
  expect(broker.receiveRemote(remote(offer("B"), 2)).status, Status::ResourceLimit, "bounded pending acquisitions");
  broker.completeAcquire("epoch-1", request.requestId, {Status::Timeout, {}, {}, 0, LeaseState::Closed});
  check(broker.pendingAcquireCount() == 0 && broker.retainedManifestBytes() == 0, "failed acquire returns budgets");

  ClipboardBroker measured(options());
  begin(measured);
  measured.observeLocal(offer("one"), {"application", {}, {}});
  limited = options();
  limited.maxManifestBytes = measured.retainedManifestBytes();
  limited.maxRetainedOffers = 1;
  ClipboardBroker exact(limited);
  begin(exact);
  expect(exact.observeLocal(offer("one"), {"application", {}, {}}).status, Status::Ok, "exact logical manifest budget");
  expect(
      exact.observeLocal(offer("two"), {"application", {}, {}}).status, Status::Ok,
      "replacing local observation reuses its bookkeeping slot"
  );
  auto oversized = offer("big");
  oversized.files.front().relativePath.append(100, 'x');
  expect(exact.receiveRemote(remote(oversized, 1)).status, Status::ResourceLimit, "manifest byte limit");
  check(exact.currentOffer()->offer.offerId == "two", "oversized replacement leaves old selection");
}

void invalidAcquireAndDuplicateLease()
{
  ClipboardBroker broker(options());
  begin(broker);
  const auto first = action(broker.receiveRemote(remote(offer("A"), 1)), Kind::AcquireLease);
  const auto wrong = broker.completeAcquire("epoch-1", first.requestId, lease("lease-wrong", "new-version"));
  expect(wrong.status, Status::InvalidSourceResponse, "wrong manifest version");
  check(count(wrong, Kind::Publish) == 0 && count(wrong, Kind::ReleaseLease) == 1, "invalid acquired lease released");
  const auto zero = action(broker.receiveRemote(remote(offer("zero"), 2)), Kind::AcquireLease);
  expect(
      broker.completeAcquire("epoch-1", zero.requestId, lease("lease-zero", "manifest-v1", 0)).status,
      Status::InvalidSourceResponse, "zero granted TTL"
  );
  const auto a = publish(broker, "live", 3);
  const auto collision = action(broker.receiveRemote(remote(offer("collision"), 4)), Kind::AcquireLease);
  const auto completed = broker.completeAcquire("epoch-1", collision.requestId, lease(a.leaseId));
  expect(completed.status, Status::InvalidSourceResponse, "colliding lease identity");
  check(
      completed.actions.empty() && broker.retainedPublications().size() == 1,
      "do not release the other publication's colliding lease"
  );
  expect(
      broker.completeAcquire("epoch-1", "unknown", lease("not-ours")).status, Status::InvalidSourceResponse,
      "uncorrelated response rejected"
  );
}

void renewFailureAndPublicationFailure()
{
  ClipboardBroker broker(options());
  begin(broker);
  const auto a = publish(broker, "A", 1);
  const auto b = publish(broker, "B", 2);
  const auto expired =
      broker.completeRenew(a.owner.publicationToken, {Status::LeaseExpired, {}, {}, 0, LeaseState::Expired});
  expect(expired.status, Status::LeaseExpired, "expired old lease reported");
  check(
      count(expired, Kind::RetirePublication) == 1 && broker.currentOffer()->offer.offerId == "B",
      "expiry retires old directory without touching current B"
  );
  auto invalid = lease("wrong-lease");
  expect(
      broker.completeRenew(b.owner.publicationToken, invalid).status, Status::InvalidSourceResponse,
      "renewal must match lease and version"
  );
  check(!broker.currentOffer() && broker.retainedPublications().empty(), "invalid current renewal stops publication");
  const auto request = action(broker.receiveRemote(remote(offer("C"), 3)), Kind::AcquireLease);
  const auto publication =
      action(broker.completeAcquire("epoch-1", request.requestId, lease("lease-C")), Kind::Publish);
  const auto failed = broker.completePublication(publication.owner.publicationToken, Status::IoError);
  check(
      count(failed, Kind::ReleaseLease) == 1 && broker.retainedOfferCount() == 0,
      "failed platform publication returns lease and metadata budget"
  );
}

void sessionCleanupAndNoEpochReuse()
{
  ClipboardBroker broker(options());
  begin(broker);
  const auto a = publish(broker, "A", 1);
  publish(broker, "B", 2);
  const auto pending = action(broker.receiveRemote(remote(offer("C"), 3)), Kind::AcquireLease);
  const auto closed = broker.closeSession("epoch-1");
  check(
      count(closed, Kind::RetirePublication) == 2 && count(closed, Kind::ReleaseLease) == 2 &&
          count(closed, Kind::CancelAcquire) == 1,
      "session close retires all old/current publications and cancels pending acquisition"
  );
  check(
      !broker.currentOffer() && broker.retainedOfferCount() == 0 && broker.retainedManifestBytes() == 0,
      "session close releases all broker records"
  );
  check(!broker.canPublish(a.owner.publicationToken), "closed token cannot be published");
  expect(broker.beginSession("epoch-1", "mac").status, Status::StaleSession, "epoch reuse forbidden");
  begin(broker, "epoch-2");
  expect(
      broker.completeAcquire("epoch-1", pending.requestId, lease("late-C")).status, Status::StaleSession,
      "late old-session completion cannot affect new session"
  );
  expect(broker.receiveRemote(remote(offer("old", "epoch-1"), 9)).status, Status::StaleSession, "old offer rejected");
  const auto local = broker.observeLocal(offer("fresh", "epoch-2"), {"application", {}, {}});
  expect(local.status, Status::Ok, "fresh session local offer");
  check(action(local, Kind::Announce).announcement->revision == 1, "origin revisions restart in new epoch");
}

void manifestAndReplayBookkeepingLimits()
{
  auto bounded = options();
  bounded.maxAnnouncementsPerSession = 1;
  bounded.maxSessionEpochs = 1;
  ClipboardBroker broker(bounded);
  begin(broker);
  expect(broker.observeLocal(offer("A"), {"application", {}, {}}).status, Status::Ok, "first announcement");
  expect(
      broker.observeLocal(offer("B"), {"application", {}, {}}).status, Status::ResourceLimit,
      "replay history has explicit bound"
  );
  broker.closeSession("epoch-1");
  expect(broker.beginSession("epoch-2", "mac").status, Status::ResourceLimit, "epoch history bounded");

  ClipboardBroker validator(options());
  begin(validator);
  auto invalid = offer("bad-root");
  invalid.roots.push_back("absent");
  expect(validator.receiveRemote(remote(invalid, 1)).status, Status::InvalidArgument, "unknown root");
  invalid = offer("cycle");
  invalid.files.push_back({"cycle-a", "cycle-b", "a", "a", "v1", FileKind::Directory, 0});
  invalid.files.push_back({"cycle-b", "cycle-a", "b", "b", "v1", FileKind::Directory, 0});
  expect(validator.receiveRemote(remote(invalid, 1)).status, Status::InvalidArgument, "disconnected directory cycle");
  invalid = offer("bad-size");
  invalid.files.back().size = std::numeric_limits<std::uint64_t>::max();
  expect(validator.receiveRemote(remote(invalid, 1)).status, Status::UnsupportedSize, "POSIX cannot represent size");
  invalid = offer("duplicate-name");
  invalid.files.back().name = invalid.files[1].name;
  expect(validator.receiveRemote(remote(invalid, 1)).status, Status::InvalidArgument, "ambiguous siblings");
  check(validator.retainedOfferCount() == 0, "invalid manifest never reaches publication");
}

} // namespace

int main()
{
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"lease-before-publication", leaseBeforePublication},
      {"late-acquisition", lateAcquisitionCannotReplaceNewerOffer},
      {"delayed-platform-publication", delayedBackendPublicationMustRecheckSelection},
      {"owner-markers-and-identical-user-copies", ownerMarkersAndIdenticalUserCopies},
      {"old-directory-retention-and-renewal", oldDirectoryAndRenewalAreRetained},
      {"epoch-revision-direction", sessionRevisionAndDirectionValidation},
      {"retained-offer-budget", retainedOfferBudgetDoesNotEvictOldDirectories},
      {"pending-and-manifest-budgets", pendingAndManifestBudgets},
      {"invalid-acquire-and-lease-collision", invalidAcquireAndDuplicateLease},
      {"renewal-and-publication-failures", renewFailureAndPublicationFailure},
      {"session-cleanup", sessionCleanupAndNoEpochReuse},
      {"manifest-and-replay-limits", manifestAndReplayBookkeepingLimits},
  };
  try {
    for (const auto &[name, test] : tests) {
      test();
      std::cout << "PASS " << name << '\n';
    }
    std::cout << tests.size() << " broker tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
