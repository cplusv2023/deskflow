/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "ClipboardBroker.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace deskflow::clipboard::experimental;

namespace {

void check(bool condition, const std::string &message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ClipboardAnnouncement remote(std::string id, std::uint64_t sequence, std::string origin = "ubuntu/process-1")
{
  return {std::move(id), "version-1", std::move(origin), sequence, {"text/plain"}};
}

class RecordingBackend final : public ClipboardBackend
{
public:
  std::optional<std::uint64_t>
  publish(const ClipboardAnnouncement &announcement, const PublicationMarker &marker) noexcept override
  {
    ++calls;
    if (fail) {
      return std::nullopt;
    }
    lastOffer = announcement;
    lastMarker = marker;
    return ++generation;
  }

  bool fail{false};
  unsigned calls{0};
  std::uint64_t generation{100};
  std::optional<ClipboardAnnouncement> lastOffer;
  std::optional<PublicationMarker> lastMarker;
};

void asyncPublicationOrdering()
{
  ClipboardBroker broker("mac/process-1", "publication-nonce");
  RecordingBackend backend;
  const auto a = broker.receiveRemote(remote("A", 10));
  const auto b = broker.receiveRemote(remote("B", 11));
  check(a.publication && b.publication, "new offers must create preparation tickets");

  // A's lease response arrives after B. Rejection must happen BEFORE any
  // clipboard write, not as a check on an already completed publication.
  check(broker.publishIfCurrent(*a.publication, backend) == BrokerStatus::StalePublication, "reject delayed A");
  check(backend.calls == 0, "stale completion must not touch the clipboard");
  check(broker.publishIfCurrent(*b.publication, backend) == BrokerStatus::Accepted, "publish current B");
  check(backend.lastOffer->offerId == "B", "clipboard contains B");
  check(broker.publishIfCurrent(*b.publication, backend) == BrokerStatus::StalePublication, "ticket is single use");
  check(backend.calls == 1, "duplicate completion must not republish");

  const auto c = broker.receiveRemote(remote("C", 12));
  auto forged = *c.publication;
  forged.announcement.offerId = "unprepared-file";
  check(broker.publishIfCurrent(forged, backend) == BrokerStatus::StalePublication, "ticket binds the complete offer");
  check(backend.calls == 1, "changed ticket must not reach backend");

  const auto local = broker.observeLocal(102, std::nullopt, "local-copy", "v1", {"text/plain"});
  check(local.status == BrokerStatus::Accepted, "local copy supersedes pending remote offer");
  check(broker.publishIfCurrent(*c.publication, backend) == BrokerStatus::StalePublication, "local copy wins over C");
  check(broker.current()->offerId == "local-copy", "current offer remains local");
}

void publicationEchoAndUserCopies()
{
  ClipboardBroker broker("mac/process-1", "publication-nonce");
  RecordingBackend backend;
  const auto a = broker.receiveRemote(remote("A", 1));
  check(broker.publishIfCurrent(*a.publication, backend) == BrokerStatus::Accepted, "publish A");
  const auto marker = backend.lastMarker;
  const auto generation = backend.generation;

  // B may be preparing while the backend reports its earlier write of A.
  // Observing A's own write must neither echo it nor invalidate B's ticket.
  const auto b = broker.receiveRemote(remote("B", 2));
  const auto own = broker.observeLocal(generation, marker, "captured-A", "v1", {"text/plain"});
  check(own.status == BrokerStatus::OwnPublication && !own.announcement, "suppress our matching marker/generation");
  check(broker.current()->offerId == "B", "own observation does not supersede pending B");
  check(broker.publishIfCurrent(*b.publication, backend) == BrokerStatus::Accepted, "B still publishable");

  // Some applications carry custom representations when copying. A preserved
  // token at a new generation must be treated as a new user copy.
  const auto user = broker.observeLocal(backend.generation + 1, backend.lastMarker, "user-copy", "v1", {"text/plain"});
  check(user.status == BrokerStatus::Accepted && user.announcement->sequence == 1, "copied token is not our write");
  const auto repeated = broker.observeLocal(backend.generation + 1, std::nullopt, "ignored-id", "v1", {"text/plain"});
  check(repeated.status == BrokerStatus::Duplicate, "polling the same generation must not create another offer");
  const auto sameContent =
      broker.observeLocal(backend.generation + 2, std::nullopt, "user-copy-2", "v1", {"text/plain"});
  check(
      sameContent.status == BrokerStatus::Accepted && sameContent.announcement->sequence == 2, "new copy gets sequence"
  );

  ClipboardBroker wrongToken("mac/process-2", "another-nonce");
  RecordingBackend otherBackend;
  auto incoming = wrongToken.receiveRemote(remote("C", 1));
  wrongToken.publishIfCurrent(*incoming.publication, otherBackend);
  auto foreignMarker = *otherBackend.lastMarker;
  foreignMarker.token = "foreign-process";
  check(
      wrongToken.observeLocal(otherBackend.generation, foreignMarker, "external", "v1", {"text/plain"}).status ==
          BrokerStatus::Accepted,
      "matching generation without our token must not suppress an external observation"
  );

  ClipboardBroker wrongRevision("mac/process-3", "nonce-three");
  RecordingBackend thirdBackend;
  incoming = wrongRevision.receiveRemote(remote("D", 1));
  wrongRevision.publishIfCurrent(*incoming.publication, thirdBackend);
  auto oldMarker = *thirdBackend.lastMarker;
  ++oldMarker.revision;
  check(
      wrongRevision.observeLocal(thirdBackend.generation, oldMarker, "external-2", "v1", {"text/plain"}).status ==
          BrokerStatus::Accepted,
      "matching token and generation still require the publication revision"
  );
}

void originOrderingAndLimits()
{
  BrokerLimits limits;
  limits.maxOrigins = 2;
  ClipboardBroker broker("mac/process-1", "publication-nonce", limits);
  check(broker.receiveRemote(remote("A", 20)).status == BrokerStatus::Accepted, "first origin");
  const auto revision = broker.revision();
  check(broker.receiveRemote(remote("duplicate", 20)).status == BrokerStatus::Duplicate, "deduplicate origin sequence");
  check(
      broker.receiveRemote(remote("old", 19)).status == BrokerStatus::OutOfOrder, "reject reordering from one origin"
  );
  check(broker.revision() == revision && broker.current()->offerId == "A", "replays do not alter the selection");
  check(
      broker.receiveRemote(remote("B", 1, "ubuntu/process-2")).status == BrokerStatus::Accepted, "new epoch accepted"
  );
  check(broker.current()->offerId == "B", "sequences from different origins are not compared");
  check(
      broker.receiveRemote(remote("third", 1, "third-device/process-1")).status == BrokerStatus::ResourceLimit,
      "origin replay tracking is bounded without silent eviction"
  );
  check(
      broker.receiveRemote(remote("A-new", 21)).status == BrokerStatus::Accepted, "existing origin advances at limit"
  );
  check(
      broker.receiveRemote(remote("echo", 300, "mac/process-1")).status == BrokerStatus::LocalOrigin,
      "suppress relay echo"
  );
  auto invalid = remote("invalid", 22);
  invalid.formats = {""};
  check(broker.receiveRemote(invalid).status == BrokerStatus::InvalidAnnouncement, "reject invalid format");
  check(
      broker.receiveRemote(remote("valid", 22)).status == BrokerStatus::Accepted,
      "invalid input does not consume sequence"
  );
  invalid = remote("too-large", 23);
  invalid.offerId.assign(65537, 'x');
  check(broker.receiveRemote(invalid).status == BrokerStatus::InvalidAnnouncement, "metadata bytes are bounded");
  invalid = remote("too-many-formats", 23);
  invalid.formats.assign(65, "text/plain");
  check(broker.receiveRemote(invalid).status == BrokerStatus::InvalidAnnouncement, "format count is bounded");
}

void failureClearAndDisconnect()
{
  ClipboardBroker broker("mac/process-1", "publication-nonce");
  RecordingBackend backend;
  const auto a = broker.receiveRemote(remote("A", 1));
  backend.fail = true;
  check(
      broker.publishIfCurrent(*a.publication, backend) == BrokerStatus::BackendFailure, "surface publication failure"
  );
  check(!backend.lastOffer, "failed publication is not successful");
  backend.fail = false;
  check(
      broker.publishIfCurrent(*a.publication, backend) == BrokerStatus::Accepted, "current preparation can be retried"
  );

  const auto b = broker.receiveRemote(remote("B", 2));
  const auto writes = backend.calls;
  broker.clear();
  check(!broker.current(), "clear removes current selection");
  check(
      broker.publishIfCurrent(*b.publication, backend) == BrokerStatus::StalePublication, "clear invalidates prepared B"
  );
  check(backend.calls == writes, "clear and stale completion do not modify external clipboard");
  check(
      broker.receiveRemote(remote("replayed-B", 2)).status == BrokerStatus::Duplicate,
      "clear preserves replay protection"
  );

  // Clearing the actual platform clipboard is an ordinary local change. It
  // propagates as an offer with no formats, with its own increasing sequence.
  const auto empty = broker.observeLocal(backend.generation + 1, std::nullopt, "empty", "v1", {});
  check(
      empty.status == BrokerStatus::Accepted && empty.announcement->formats.empty(), "propagate local clipboard clear"
  );
  const auto c = broker.receiveRemote(remote("C", 3));
  broker.disconnect();
  const auto stoppedRevision = broker.revision();
  broker.disconnect();
  check(!broker.connected() && !broker.current(), "disconnect clears and closes broker");
  check(broker.revision() == stoppedRevision, "disconnect is idempotent");
  check(
      broker.publishIfCurrent(*c.publication, backend) == BrokerStatus::Disconnected,
      "disconnect rejects late preparation"
  );
  check(
      broker.receiveRemote(remote("D", 4)).status == BrokerStatus::Disconnected,
      "disconnected session cannot accept offers"
  );
  check(
      broker.observeLocal(999, std::nullopt, "late-local", "v1", {"text/plain"}).status == BrokerStatus::Disconnected,
      "disconnected session does not originate offers"
  );
  check(backend.calls == writes, "disconnect performs no platform clipboard writes");
}

} // namespace

int main()
{
  try {
    asyncPublicationOrdering();
    publicationEchoAndUserCopies();
    originOrderingAndLimits();
    failureClearAndDisconnect();
    std::cout << "clipboard broker tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "clipboard broker test failed: " << error.what() << '\n';
    return 1;
  }
}
