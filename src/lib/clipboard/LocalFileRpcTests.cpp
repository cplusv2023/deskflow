/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "LocalFileRpc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace deskflow::clipboard;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

void check(bool condition, const std::string &message)
{
  if (!condition)
    throw std::runtime_error(message);
}

void expect(Status actual, Status expected, const std::string &message)
{
  check(
      actual == expected,
      message + ": expected " + std::string(statusName(expected)) + ", got " + std::string(statusName(actual))
  );
}

template <class Operation> void expectException(Operation operation, Status expected, const std::string &message)
{
  try {
    operation();
  } catch (const RpcError &error) {
    expect(error.status, expected, message);
    return;
  }
  throw std::runtime_error(message + ": expected RpcError");
}

struct TemporaryDirectory
{
  fs::path path;

  TemporaryDirectory()
  {
    char pattern[] = "/tmp/deskflow-rpc-client-XXXXXX";
    const auto created = ::mkdtemp(pattern);
    if (!created)
      throw std::runtime_error("mkdtemp failed");
    path = created;
    struct stat attributes{};
    check(
        ::stat(path.c_str(), &attributes) == 0 && attributes.st_uid == ::getuid() &&
            (attributes.st_mode & 0777) == 0700,
        "test socket directory must be private and owned by this user"
    );
  }

  ~TemporaryDirectory()
  {
    std::error_code error;
    fs::remove_all(path, error);
  }
};

class Server
{
public:
  explicit Server(const fs::path &root, std::uint32_t delayMs = 0)
      : socketPath(root / "files.sock"),
        sourcePath(root / "source.bin")
  {
    // A small deterministic fixture; no large backing allocation is necessary.
    bytes.resize(4096);
    for (std::size_t index = 0; index < bytes.size(); ++index)
      bytes[index] = static_cast<std::uint8_t>((index % 251 + 41) % 251);
    std::ofstream output(sourcePath, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    check(static_cast<bool>(output), "write source fixture");

    FileRpcServerOptions options;
    options.socketPath = socketPath;
    options.paths = {sourcePath};
    options.delayMs = delayMs;
    worker = std::thread([this, options] {
      try {
        serveLocalFiles(options, stopped);
      } catch (...) {
        std::lock_guard lock(errorMutex);
        error = std::current_exception();
      }
      finished.store(true);
    });
    try {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      for (;;) {
        struct stat attributes{};
        if (::stat(socketPath.c_str(), &attributes) == 0 && S_ISSOCK(attributes.st_mode) &&
            (attributes.st_mode & 0777) == 0600)
          break;
        check(!finished.load(), "server exited before startup");
        check(std::chrono::steady_clock::now() < deadline, "server startup deadline exceeded");
        std::this_thread::sleep_for(1ms);
      }
      struct stat attributes{};
      check(
          ::stat(socketPath.c_str(), &attributes) == 0 && S_ISSOCK(attributes.st_mode) &&
              (attributes.st_mode & 0777) == 0600,
          "server socket must have mode 0600"
      );
    } catch (...) {
      stop();
      throw;
    }
  }

  ~Server()
  {
    stop();
  }

  void finish()
  {
    stop();
    std::lock_guard lock(errorMutex);
    if (error)
      std::rethrow_exception(error);
    check(!fs::exists(socketPath), "server socket must be removed after stop/join");
  }

  fs::path socketPath;
  fs::path sourcePath;
  std::vector<std::uint8_t> bytes;

private:
  void stop()
  {
    stopped.store(true);
    if (worker.joinable())
      worker.join();
  }

  std::atomic_bool stopped{false};
  std::atomic_bool finished{false};
  std::mutex errorMutex;
  std::exception_ptr error;
  std::thread worker;
};

OfferInfo hello(FileRpcClient &client)
{
  auto result = client.hello();
  expect(result.status, Status::Ok, "hello");
  check(
      result.offer.files.size() == 1 && result.offer.files.front().kind == FileKind::File,
      "single authorized source file"
  );
  return result.offer;
}

LeaseResult acquire(FileRpcClient &client, const OfferInfo &offer, std::uint32_t ttl = 30000)
{
  const auto lease = client.acquire(offer, ttl);
  expect(lease.status, Status::Ok, "acquire");
  check(
      !lease.leaseId.empty() && lease.offerVersion == offer.offerVersion && lease.state == LeaseState::Active &&
          lease.grantedTtlMs == ttl,
      "acquired lease identity and actual TTL"
  );
  return lease;
}

OpenResult open(FileRpcClient &client, const LeaseResult &lease, const FileInfo &file)
{
  auto result = client.open(lease.leaseId, file, ConsistencyMode::Detectable);
  expect(result.status, Status::Ok, "open detectable file");
  check(
      !result.handleId.empty() && result.version == file.version && result.size == file.size,
      "handle binds fixed metadata"
  );
  return result;
}

void checkRead(
    FileRpcClient &client, const LeaseResult &lease, const OpenResult &handle, const Server &server,
    std::uint64_t offset = 17, std::uint32_t length = 97
)
{
  const auto result = client.read(lease.leaseId, handle.handleId, offset, length);
  expect(result.status, Status::Ok, "read existing handle");
  check(
      result.bytes == std::vector<std::uint8_t>(server.bytes.begin() + offset, server.bytes.begin() + offset + length),
      "read returns requested deterministic range"
  );
  check(
      !result.eof && result.version == handle.version && result.handleId == handle.handleId &&
          result.leaseId == lease.leaseId && result.offset == offset && result.length == length,
      "typed read association and EOF"
  );
}

void expiredReleaseKeepsConnectionUsable()
{
  TemporaryDirectory temporary;
  Server server(temporary.path);
  {
    FileRpcClient client(server.socketPath, 1500);
    const auto offer = hello(client);
    const auto expired = acquire(client, offer, 30);
    std::this_thread::sleep_for(80ms);
    const auto released = client.release(expired.leaseId);
    expect(released.status, Status::Ok, "release expired lease");
    check(
        released.state == LeaseState::Expired && released.leaseId == expired.leaseId &&
            released.offerVersion == offer.offerVersion,
        "release returns matching EXPIRED terminal state"
    );
    check(client.release(expired.leaseId).state == LeaseState::Expired, "expired release remains idempotent");
    const auto fresh = client.refresh();
    expect(fresh.status, Status::Ok, "refresh on same stream after expired release");
    check(
        fresh.offer.sessionEpoch == offer.sessionEpoch && fresh.offer.offerId != offer.offerId,
        "same connection has same epoch and a new offer"
    );
    const auto lease = acquire(client, fresh.offer);
    const auto opened = open(client, lease, fresh.offer.files.front());
    checkRead(client, lease, opened, server);
    expect(client.close(opened.handleId), Status::Ok, "close fresh handle");
    check(client.release(lease.leaseId).state == LeaseState::Closed, "release completed fresh lease");
  }
  server.finish();
}

void cancellationEndsBlockedReadAndIsolatesNextConnection()
{
  TemporaryDirectory temporary;
  Server server(temporary.path, 1500);
  std::string oldEpoch;
  {
    FileRpcClient client(server.socketPath, 3000);
    const auto offer = hello(client);
    oldEpoch = offer.sessionEpoch;
    const auto lease = acquire(client, offer);
    const auto opened = open(client, lease, offer.files.front());
    std::atomic_bool delivered{false};
    auto pending = std::async(std::launch::async, [&] {
      try {
        const auto result = client.read(lease.leaseId, opened.handleId, 0, 97);
        delivered.store(true);
        return result.status;
      } catch (const RpcError &error) {
        return error.status;
      }
    });
    check(pending.wait_for(50ms) == std::future_status::timeout, "injected delay keeps read in flight");
    const auto start = std::chrono::steady_clock::now();
    client.cancel();
    check(pending.wait_for(1s) == std::future_status::ready, "cancel must wake blocked read within one second");
    expect(pending.get(), Status::Cancelled, "blocked read reports cancellation");
    check(
        !delivered.load() && std::chrono::steady_clock::now() - start < 1s,
        "no delayed successful payload delivered after cancellation"
    );
    expectException([&] { client.refresh(); }, Status::Cancelled, "cancelled connection cannot be reused");
  }
  {
    FileRpcClient next(server.socketPath, 1500);
    const auto fresh = hello(next);
    check(fresh.sessionEpoch != oldEpoch, "new connection cannot consume old epoch or late response");
    expect(next.refresh().status, Status::Ok, "next stream usable after predecessor cancellation");
  }
  server.finish();
}

void renewalAndDrainingLeaseLifecycle()
{
  TemporaryDirectory temporary;
  Server server(temporary.path);
  {
    FileRpcClient client(server.socketPath, 1500);
    const auto offer = hello(client);
    const auto lease = acquire(client, offer, 1500);
    const auto opened = open(client, lease, offer.files.front());
    const auto renewed = client.renew(lease.leaseId, 2000);
    expect(renewed.status, Status::Ok, "renew active lease");
    check(
        renewed.leaseId == lease.leaseId && renewed.offerVersion == offer.offerVersion &&
            renewed.grantedTtlMs == 2000 && renewed.state == LeaseState::Active,
        "renew preserves identity and returns actual TTL"
    );
    const auto draining = client.release(lease.leaseId);
    expect(draining.status, Status::Ok, "release with open handle");
    check(draining.state == LeaseState::Draining, "open handle keeps released lease draining");
    const auto drainingRenewal = client.renew(lease.leaseId, 2500);
    expect(drainingRenewal.status, Status::Ok, "renew draining lease");
    check(
        drainingRenewal.leaseId == lease.leaseId && drainingRenewal.offerVersion == offer.offerVersion &&
            drainingRenewal.grantedTtlMs == 2500 && drainingRenewal.state == LeaseState::Draining,
        "renewal does not reopen draining lease"
    );
    expect(
        client.open(lease.leaseId, offer.files.front(), ConsistencyMode::Detectable).status, Status::LeaseReleased,
        "new open on draining lease rejected without poisoning stream"
    );
    checkRead(client, lease, opened, server);
    expect(client.close(opened.handleId), Status::Ok, "close last draining handle");
    const auto closed = client.release(lease.leaseId);
    expect(closed.status, Status::Ok, "release already closed lease");
    check(
        closed.state == LeaseState::Closed && closed.leaseId == lease.leaseId &&
            closed.offerVersion == offer.offerVersion,
        "last close transitions lease to CLOSED"
    );
    check(client.release(lease.leaseId).state == LeaseState::Closed, "closed release is idempotent");
    expect(client.renew(lease.leaseId, 2000).status, Status::LeaseReleased, "closed lease cannot be renewed");
    expect(client.refresh().status, Status::Ok, "business errors leave stream usable");
  }
  server.finish();
}

void localHandleBudgetDoesNotPoisonStream()
{
  TemporaryDirectory temporary;
  Server server(temporary.path);
  {
    FileRpcClient client(server.socketPath, 1500);
    const auto offer = hello(client);
    const auto lease = acquire(client, offer);
    std::vector<OpenResult> handles;
    std::set<std::string> identities;
    for (std::size_t index = 0; index < 256; ++index) {
      auto opened = open(client, lease, offer.files.front());
      check(identities.insert(opened.handleId).second, "each open has a unique handle");
      handles.push_back(std::move(opened));
    }
    expectException(
        [&] { client.open(lease.leaseId, offer.files.front(), ConsistencyMode::Detectable); }, Status::ResourceLimit,
        "local 257th handle rejected"
    );
    checkRead(client, lease, handles.front(), server);
    expect(client.close(handles.back().handleId), Status::Ok, "free local handle budget");
    handles.pop_back();
    expectException(
        [&] { client.open(lease.leaseId, offer.files.front(), static_cast<ConsistencyMode>(99)); },
        Status::InvalidArgument, "invalid local consistency does not poison stream"
    );
    handles.push_back(open(client, lease, offer.files.front()));
    checkRead(client, lease, handles.back(), server);
    for (const auto &handle : handles)
      expect(client.close(handle.handleId), Status::Ok, "close bounded handles");
    check(client.release(lease.leaseId).state == LeaseState::Closed, "all handles and lease cleanly released");
  }
  server.finish();
}

} // namespace

int main()
{
  // Invalid wire structures and response identity mutations remain covered by
  // test_local_rpc.py's fault proxies. These exercise the public typed client.
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"expired-release-keeps-stream", expiredReleaseKeepsConnectionUsable},
      {"cancel-blocked-read-and-next-stream", cancellationEndsBlockedReadAndIsolatesNextConnection},
      {"renew-and-draining-lifecycle", renewalAndDrainingLeaseLifecycle},
      {"local-handle-budget-keeps-stream", localHandleBudgetDoesNotPoisonStream},
  };
  try {
    for (const auto &[name, test] : tests) {
      test();
      std::cout << "PASS " << name << '\n';
    }
    std::cout << tests.size() << " typed RPC client tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
