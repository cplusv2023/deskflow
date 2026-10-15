/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "LocalFileSource.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace deskflow::clipboard;
namespace fs = std::filesystem;

namespace {

void check(bool value, const std::string &description)
{
  if (!value)
    throw std::runtime_error(description);
}

void expect(Status actual, Status expected, const std::string &context)
{
  check(
      actual == expected,
      context + ": expected " + std::string(statusName(expected)) + ", got " + std::string(statusName(actual))
  );
}

struct TemporaryDirectory
{
  fs::path path;
  TemporaryDirectory()
  {
    char pattern[] = "/tmp/deskflow-local-source-XXXXXX";
    const auto *created = ::mkdtemp(pattern);
    if (!created)
      throw std::runtime_error("mkdtemp failed");
    path = created;
  }
  ~TemporaryDirectory()
  {
    std::error_code error;
    fs::remove_all(path, error);
  }
};

LocalFileSource::Options options()
{
  LocalFileSource::Options value;
  value.sessionEpoch = "session-test-1";
  return value;
}

void write(const fs::path &path, const std::string &contents)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.close();
  check(static_cast<bool>(output), "write fixture");
}

std::string pattern(std::size_t length, unsigned int seed = 41)
{
  std::string bytes(length, '\0');
  for (std::size_t index = 0; index < length; ++index)
    bytes[index] = static_cast<char>((index % 251 + seed) % 251);
  return bytes;
}

const FileInfo &file(const OfferInfo &offer, std::string_view relativePath)
{
  const auto found = std::find_if(offer.files.begin(), offer.files.end(), [&](const auto &entry) {
    return entry.relativePath == relativePath;
  });
  check(found != offer.files.end(), "manifest entry " + std::string(relativePath));
  return *found;
}

LeaseResult acquire(LocalFileSource &source, const OfferInfo &offer, std::uint32_t ttl = 0)
{
  const auto lease = source.acquire(offer.sessionEpoch, offer.offerId, offer.offerVersion, ttl);
  expect(lease.status, Status::Ok, "acquire");
  return lease;
}

OpenResult open(LocalFileSource &source, const LeaseResult &lease, const FileInfo &entry)
{
  auto result = source.open("session-test-1", lease.leaseId, entry.fileId, entry.version, ConsistencyMode::Detectable);
  expect(result.status, Status::Ok, "open detectable");
  return result;
}

ReadResult read(
    LocalFileSource &source, const LeaseResult &lease, const OpenResult &handle, std::uint64_t offset,
    std::uint32_t length
)
{
  static std::atomic<std::uint64_t> sequence{1};
  return source.read(
      {"session-test-1", lease.leaseId, handle.handleId, "request-" + std::to_string(sequence.fetch_add(1)), offset,
       length}
  );
}

std::string bytes(const ReadResult &result)
{
  return {result.bytes.begin(), result.bytes.end()};
}

void testRanges()
{
  TemporaryDirectory directory;
  const auto contents = pattern((1 << 20) + 31);
  write(directory.path / "含 空格.bin", contents);
  auto config = options();
  config.maxResponseBytes = 4093;
  LocalFileSource source(config);
  const auto created = source.createOffer({directory.path / "含 空格.bin"});
  expect(created.status, Status::Ok, "create");
  check(!created.offer.immutableVersion && created.offer.roots.size() == 1, "detectable capability");
  const auto &entry = file(created.offer, "含 空格.bin");
  const auto lease = acquire(source, created.offer);
  expect(
      source.open(config.sessionEpoch, lease.leaseId, entry.fileId, entry.version).status, Status::SourceNotStable,
      "immutable default is not silently downgraded"
  );
  const auto handle = open(source, lease, entry);
  check(handle.size == contents.size() && handle.version == entry.version, "fixed handle metadata");
  const auto partial = read(source, lease, handle, 11, 10000);
  expect(partial.status, Status::Ok, "short response");
  check(
      partial.bytes.size() == 4093 && !partial.eof && bytes(partial) == contents.substr(11, 4093),
      "positive short response is not EOF"
  );
  check(
      partial.offerId == created.offer.offerId && partial.fileId == entry.fileId && partial.version == entry.version &&
          partial.offset == 11 && partial.length == 10000 && !partial.requestId.empty(),
      "read identity echo"
  );
  std::string aggregate;
  for (std::uint64_t offset = 262130; aggregate.size() < 131111;) {
    const auto count = static_cast<std::uint32_t>(131111 - aggregate.size());
    const auto response = read(source, lease, handle, offset, count);
    expect(response.status, Status::Ok, "aggregate range");
    check(!response.bytes.empty() && !response.eof, "short intermediate range");
    aggregate += bytes(response);
    offset += response.bytes.size();
  }
  check(aggregate == contents.substr(262130, 131111), "aggregate exact content");
  const auto last = read(source, lease, handle, contents.size() - 10, 8192);
  expect(last.status, Status::Ok, "last short read");
  check(last.eof && bytes(last) == contents.substr(contents.size() - 10), "only real EOF marks short read");
  for (const auto offset : {static_cast<std::uint64_t>(contents.size()), std::numeric_limits<std::uint64_t>::max()}) {
    const auto eof = read(source, lease, handle, offset, 200);
    expect(eof.status, Status::Ok, "out of range read");
    check(eof.eof && eof.bytes.empty(), "beyond EOF cannot wrap offset");
  }
  for (const auto length : {0U, 262145U}) {
    const auto invalid = read(source, lease, handle, 0, length);
    expect(invalid.status, Status::InvalidArgument, "invalid range length");
    check(invalid.bytes.empty() && !invalid.eof, "errors carry no data or EOF");
  }
  expect(
      source.open(config.sessionEpoch, lease.leaseId, entry.fileId, "wrong", ConsistencyMode::Detectable).status,
      Status::VersionMismatch, "version checked"
  );
  expect(
      source.open(config.sessionEpoch, lease.leaseId, "/etc/passwd", "wrong", ConsistencyMode::Detectable).status,
      Status::NotFound, "path is not a capability"
  );
  expect(
      source.stat("old-session", lease.leaseId, entry.fileId, entry.version).status, Status::StaleSession, "stale epoch"
  );
  const auto otherLease = acquire(source, created.offer);
  expect(read(source, otherLease, handle, 0, 10).status, Status::NotFound, "handle bound to exact lease");
  expect(source.close(config.sessionEpoch, handle.handleId), Status::Ok, "close");
  expect(source.close(config.sessionEpoch, handle.handleId), Status::Ok, "idempotent close");
  expect(read(source, lease, handle, 0, 10).status, Status::HandleClosed, "closed handle read");
  expect(source.close(config.sessionEpoch, "never-issued"), Status::NotFound, "unknown close");
}

void testSparse64BitAndEmpty()
{
  TemporaryDirectory directory;
  const auto path = directory.path / "large.bin";
  const auto size = (std::uint64_t{4} << 30) + 123;
  const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  check(fd >= 0 && ::ftruncate(fd, static_cast<off_t>(size)) == 0, "create sparse large fixture");
  const auto tail = pattern(114, 97);
  check(
      ::pwrite(fd, tail.data(), tail.size(), static_cast<off_t>(size - tail.size())) ==
          static_cast<ssize_t>(tail.size()),
      "sparse tail fixture"
  );
  ::close(fd);
  write(directory.path / "empty.txt", {});
  LocalFileSource source(options());
  const auto created = source.createOffer({path, directory.path / "empty.txt"});
  expect(created.status, Status::Ok, "create sparse offer");
  const auto lease = acquire(source, created.offer);
  const auto handle = open(source, lease, file(created.offer, "large.bin"));
  check(handle.size == size, "64-bit size is preserved");
  const auto result = read(source, lease, handle, size - tail.size(), 1000);
  expect(result.status, Status::Ok, "read above 4GiB");
  check(result.eof && bytes(result) == tail, "64-bit offset exact bytes");
  const auto empty = open(source, lease, file(created.offer, "empty.txt"));
  const auto eof = read(source, lease, empty, 0, 1);
  expect(eof.status, Status::Ok, "empty file");
  check(eof.eof && eof.bytes.empty(), "empty file explicit EOF");
}

void testWholeOfferAndManifest()
{
  TemporaryDirectory directory;
  const auto tree = directory.path / "A folder";
  fs::create_directories(tree / "later");
  fs::create_directory(tree / "empty dir");
  write(tree / "first.txt", "first-A");
  write(tree / "later" / "second.txt", "second-A");
  auto now = LocalFileSource::Clock::now();
  auto config = options();
  config.now = [&] { return now; };
  config.acquisitionWindowMs = 20;
  LocalFileSource source(config);
  const auto a = source.createOffer({tree});
  expect(a.status, Status::Ok, "directory A");
  const auto leaseA = acquire(source, a.offer);
  const auto first = open(source, leaseA, file(a.offer, "A folder/first.txt"));
  check(bytes(read(source, leaseA, first, 0, 100)) == "first-A", "read A first");
  expect(source.close(config.sessionEpoch, first.handleId), Status::Ok, "zero active handles");
  write(tree / "new.txt", "new-B"); // The old manifest must not expand with the directory mtime.
  const auto b = source.createOffer({tree});
  expect(b.status, Status::Ok, "B replaces same selected directory");
  check(source.currentOfferId() == b.offer.offerId, "current is B");
  const auto &rootA = file(a.offer, "A folder");
  const auto oldList = source.list(config.sessionEpoch, leaseA.leaseId, rootA.fileId, rootA.version);
  expect(oldList.status, Status::Ok, "A fixed manifest after local additions");
  check(
      oldList.entries.size() == 3 &&
          std::none_of(
              oldList.entries.begin(), oldList.entries.end(), [](const auto &entry) { return entry.name == "new.txt"; }
          ),
      "old members never mixed with B"
  );
  now += std::chrono::milliseconds(21);
  const auto later = open(source, leaseA, file(a.offer, "A folder/later/second.txt"));
  check(
      bytes(read(source, leaseA, later, 0, 100)) == "second-A", "previously unopened descendant survives replacement"
  );
  const auto &newFile = file(b.offer, "A folder/new.txt");
  expect(
      source.open(config.sessionEpoch, leaseA.leaseId, newFile.fileId, newFile.version, ConsistencyMode::Detectable)
          .status,
      Status::NotFound, "B ID not authorized by A lease"
  );
  expect(
      source.acquire(config.sessionEpoch, a.offer.offerId, a.offer.offerVersion).status, Status::OfferExpired,
      "late acquisition after old offer window"
  );
  source.close(config.sessionEpoch, later.handleId);
  expect(source.release(config.sessionEpoch, leaseA.leaseId).status, Status::Ok, "release old offer");
  source.collectExpired();
  expect(
      source.acquire(config.sessionEpoch, a.offer.offerId, a.offer.offerVersion).status, Status::OfferExpired,
      "reclaimed A is expired, never redirected to B"
  );
}

void testLeaseStates()
{
  TemporaryDirectory directory;
  write(directory.path / "file.txt", "contents");
  auto now = LocalFileSource::Clock::now();
  auto config = options();
  config.now = [&] { return now; };
  config.defaultTtlMs = 50;
  config.maxTtlMs = 100;
  LocalFileSource source(config);
  const auto offer = source.createOffer({directory.path / "file.txt"}).offer;
  const auto &entry = offer.files.front();
  auto lease = acquire(source, offer, 1000);
  check(lease.grantedTtlMs == 100, "TTL cap");
  now += std::chrono::milliseconds(90);
  auto renewed = source.renew(config.sessionEpoch, lease.leaseId, 30);
  expect(renewed.status, Status::Ok, "renew active");
  auto handle = open(source, lease, entry);
  auto released = source.release(config.sessionEpoch, lease.leaseId);
  check(released.status == Status::Ok && released.state == LeaseState::Draining, "release enters DRAINING");
  expect(
      source.open(config.sessionEpoch, lease.leaseId, entry.fileId, entry.version, ConsistencyMode::Detectable).status,
      Status::LeaseReleased, "draining rejects new open"
  );
  expect(
      source.stat(config.sessionEpoch, lease.leaseId, entry.fileId, entry.version).status, Status::LeaseReleased,
      "draining rejects metadata access"
  );
  now += std::chrono::milliseconds(20);
  renewed = source.renew(config.sessionEpoch, lease.leaseId, 50);
  check(renewed.status == Status::Ok && renewed.state == LeaseState::Draining, "draining renew does not reactivate");
  check(bytes(read(source, lease, handle, 0, 20)) == "contents", "already opened draining handle reads");
  expect(source.close(config.sessionEpoch, handle.handleId), Status::Ok, "close last draining handle");
  released = source.release(config.sessionEpoch, lease.leaseId);
  check(released.status == Status::Ok && released.state == LeaseState::Closed, "release idempotent CLOSED");
  expect(source.renew(config.sessionEpoch, lease.leaseId).status, Status::LeaseReleased, "closed cannot renew");

  lease = acquire(source, offer, 10);
  handle = open(source, lease, entry);
  now += std::chrono::milliseconds(10);
  const auto expired = read(source, lease, handle, 0, 20);
  expect(expired.status, Status::LeaseExpired, "exact monotonic expiry boundary");
  check(expired.bytes.empty() && !expired.eof, "expired reads are errors");
  expect(source.renew(config.sessionEpoch, lease.leaseId).status, Status::LeaseExpired, "expiry cannot revive");
  expect(source.close(config.sessionEpoch, handle.handleId), Status::Ok, "expired handle close idempotent");
  lease = acquire(source, offer);
  handle = open(source, lease, entry);
  check(source.revoke(config.sessionEpoch, lease.leaseId).state == LeaseState::Revoked, "explicit revoke");
  expect(read(source, lease, handle, 0, 20).status, Status::LeaseRevoked, "revoked read");
  lease = acquire(source, offer);
  handle = open(source, lease, entry);
  source.disconnect();
  expect(read(source, lease, handle, 0, 20).status, Status::Disconnected, "session disconnect");
  expect(
      source.acquire(config.sessionEpoch, offer.offerId, offer.offerVersion).status, Status::Disconnected,
      "disconnected acquire"
  );
  expect(source.createOffer({directory.path / "file.txt"}).status, Status::Disconnected, "disconnected new offer");
  expect(source.close("old-epoch", handle.handleId), Status::StaleSession, "old epoch still rejected");
}

void testSourceChanges()
{
  TemporaryDirectory directory;
  const auto path = directory.path / "source.bin";
  auto checkChange = [&](const auto &mutate, Status expected, const std::string &description) {
    fs::remove(path);
    write(path, "original-version");
    LocalFileSource source(options());
    const auto offer = source.createOffer({path});
    expect(offer.status, Status::Ok, "change test offer");
    const auto lease = acquire(source, offer.offer);
    const auto handle = open(source, lease, offer.offer.files.front());
    mutate();
    const auto response = read(source, lease, handle, 0, 4096);
    expect(response.status, expected, description);
    check(response.bytes.empty() && !response.eof, description + " has no successful bytes");
  };
  checkChange([&] { write(path, "modified-version-of-different-length"); }, Status::SourceChanged, "modified contents");
  checkChange([&] { fs::remove(path); }, Status::SourceMissing, "deleted source");
  checkChange(
      [&] { check(::chmod(path.c_str(), 0000) == 0, "chmod fixture"); }, Status::PermissionDenied,
      "revoked read permission"
  );
  ::chmod(path.c_str(), 0600);
  checkChange(
      [&] {
        fs::rename(path, directory.path / "old-inode");
        write(path, "original-version");
      },
      Status::SourceChanged, "replaced inode with same size and text"
  );
  checkChange(
      [&] {
        fs::rename(path, directory.path / "another-old-inode");
        fs::create_symlink(directory.path / "another-old-inode", path);
      },
      Status::SourceChanged, "symlink substitution"
  );
}

void testInputAndBudgets()
{
  TemporaryDirectory directory;
  const auto path = directory.path / "file.txt";
  write(path, "abc");
  write(directory.path / "second.txt", "def");
  fs::create_symlink(path, directory.path / "link");
  LocalFileSource source(options());
  expect(source.createOffer({directory.path / "link"}).status, Status::InvalidArgument, "symlink root rejected");
  expect(
      source.createOffer({directory.path / ".." / directory.path.filename() / "file.txt"}).status,
      Status::InvalidArgument, "parent traversal rejected"
  );
  check(::mkfifo((directory.path / "pipe").c_str(), 0600) == 0, "fifo fixture");
  expect(
      source.createOffer({directory.path / "pipe"}).status, Status::InvalidArgument, "FIFO rejected without blocking"
  );
  expect(source.createOffer({}).status, Status::InvalidArgument, "empty selection rejected");
  expect(
      source.createOffer({fs::path(std::string("/tmp/name\0/child", 16))}).status, Status::InvalidArgument,
      "embedded NUL in ancestor rejected before opening"
  );
  fs::create_directory(directory.path / "child");
  fs::create_symlink(path, directory.path / "child" / "link");
  expect(source.createOffer({directory.path / "child"}).status, Status::InvalidArgument, "symlink descendant rejected");
  fs::create_directory(directory.path / "same-name");
  write(directory.path / "same-name" / "file.txt", "same name");
  expect(
      source.createOffer({path, directory.path / "same-name" / "file.txt"}).status, Status::InvalidArgument,
      "colliding top level names rejected"
  );

  auto config = options();
  config.maxOpenHandles = 1;
  config.maxLeaseRecords = 1;
  config.maxOffers = 1;
  LocalFileSource bounded(config);
  const auto offer = bounded.createOffer({path}).offer;
  const auto lease = acquire(bounded, offer);
  const auto handle = open(bounded, lease, offer.files.front());
  expect(
      bounded
          .open(
              config.sessionEpoch, lease.leaseId, offer.files.front().fileId, offer.files.front().version,
              ConsistencyMode::Detectable
          )
          .status,
      Status::ResourceLimit, "open handle budget"
  );
  bounded.close(config.sessionEpoch, handle.handleId);
  const auto reopened = open(bounded, lease, offer.files.front());
  bounded.close(config.sessionEpoch, reopened.handleId);
  bounded.release(config.sessionEpoch, lease.leaseId);
  expect(
      bounded.acquire(config.sessionEpoch, offer.offerId, offer.offerVersion).status, Status::ResourceLimit,
      "bounded terminal lease records"
  );
  expect(
      bounded.createOffer({directory.path / "second.txt"}).status, Status::ResourceLimit,
      "offer budget never evicts retained state"
  );
  check(bounded.currentOfferId() == offer.offerId, "failure keeps current offer unchanged");

  config = options();
  config.maxEntriesPerOffer = 1;
  LocalFileSource smallManifest(config);
  expect(
      smallManifest.createOffer({path, directory.path / "second.txt"}).status, Status::ResourceLimit, "entry budget"
  );
  config = options();
  config.maxManifestBytes = 1;
  LocalFileSource smallBytes(config);
  expect(smallBytes.createOffer({path}).status, Status::ResourceLimit, "metadata byte budget");
}

void testConcurrentReads()
{
  TemporaryDirectory directory;
  const auto contents = pattern(1024 * 1024);
  write(directory.path / "file.bin", contents);
  LocalFileSource source(options());
  const auto offer = source.createOffer({directory.path / "file.bin"}).offer;
  const auto lease = acquire(source, offer);
  const auto handle = open(source, lease, offer.files.front());
  std::atomic<bool> valid{true};
  std::vector<std::thread> workers;
  for (std::size_t worker = 0; worker < 8; ++worker)
    workers.emplace_back([&, worker] {
      for (std::size_t index = 0; index < 20; ++index) {
        const auto offset = (worker * 17137 + index * 3119) % (contents.size() - 8192);
        const auto response = read(source, lease, handle, offset, 8192);
        if (response.status != Status::Ok || response.eof || bytes(response) != contents.substr(offset, 8192))
          valid = false;
      }
    });
  for (auto &worker : workers)
    worker.join();
  check(valid, "concurrent random reads on one version/handle");
}

} // namespace

int main()
{
  const std::vector<std::pair<const char *, void (*)()>> tests{
      {"ranges, short reads, identity and authorization", testRanges},
      {"64-bit sparse reads and empty EOF", testSparse64BitAndEmpty},
      {"whole-offer replacement and fixed directory manifest", testWholeOfferAndManifest},
      {"lease renew, drain, expire, revoke and disconnect", testLeaseStates},
      {"source changes, deletion, permissions and substitution", testSourceChanges},
      {"input restrictions and bounded resources", testInputAndBudgets},
      {"concurrent random reads", testConcurrentReads},
  };
  for (const auto &[name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception &error) {
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
      return 1;
    }
  }
  return 0;
}
