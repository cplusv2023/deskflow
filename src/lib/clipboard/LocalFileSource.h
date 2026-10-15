/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "FileTypes.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace deskflow::clipboard {

class LocalFileSource
{
public:
  using Clock = std::chrono::steady_clock;
  struct Options
  {
    std::string sessionEpoch; // Required; authentication/epoch generation belongs to transport.
    std::uint32_t defaultTtlMs{30000};
    std::uint32_t maxTtlMs{60000};
    std::uint32_t acquisitionWindowMs{30000};
    std::size_t maxOffers{32};
    std::size_t maxEntriesPerOffer{4096};
    std::size_t maxManifestBytes{4 * 1024 * 1024};
    std::size_t maxRootsPerOffer{64};
    std::size_t maxDepth{64};
    std::size_t maxOpenHandles{256};
    // Terminal lease records are retained for precise idempotent status in this session.
    std::size_t maxLeaseRecords{4096};
    std::uint32_t maxReadBytes{262144};
    // A bounded short response is legal; useful for testing real aggregation.
    std::uint32_t maxResponseBytes{262144};
    std::function<Clock::time_point()> now{[] { return Clock::now(); }};
  };

  explicit LocalFileSource(Options options);
  ~LocalFileSource();
  LocalFileSource(const LocalFileSource &) = delete;
  LocalFileSource &operator=(const LocalFileSource &) = delete;

  OfferResult createOffer(const std::vector<std::filesystem::path> &paths);
  LeaseResult acquire(
      const std::string &sessionEpoch, const std::string &offerId, const std::string &expectedOfferVersion,
      std::uint32_t requestedTtlMs = 0
  );
  LeaseResult renew(const std::string &sessionEpoch, const std::string &leaseId, std::uint32_t requestedTtlMs = 0);
  LeaseResult release(const std::string &sessionEpoch, const std::string &leaseId);
  LeaseResult revoke(const std::string &sessionEpoch, const std::string &leaseId);

  StatResult stat(
      const std::string &sessionEpoch, const std::string &leaseId, const std::string &fileId,
      const std::string &expectedVersion
  );
  ListResult list(
      const std::string &sessionEpoch, const std::string &leaseId, const std::string &directoryId,
      const std::string &expectedVersion
  );
  OpenResult open(
      const std::string &sessionEpoch, const std::string &leaseId, const std::string &fileId,
      const std::string &expectedVersion, ConsistencyMode consistency = ConsistencyMode::Immutable
  );
  ReadResult read(const ReadRequest &request);
  Status close(const std::string &sessionEpoch, const std::string &handleId);
  void disconnect();
  void collectExpired();
  std::string currentOfferId() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace deskflow::clipboard
