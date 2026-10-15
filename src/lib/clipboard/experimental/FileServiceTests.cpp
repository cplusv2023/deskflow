/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "FileService.h"
#include "LocalFileSource.h"

#include <algorithm>
#include <condition_variable>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

using namespace deskflow::clipboard::experimental;
using namespace std::chrono_literals;

namespace {

void check(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}

void expect(Status actual, Status expected, const char *message)
{
  if (actual != expected)
    throw std::runtime_error(
        std::string(message) + ": expected " + statusName(expected) + ", got " + statusName(actual)
    );
}

struct Requests
{
  unsigned int next{1};
  RequestContext operator()()
  {
    return {.sessionEpoch = "session-1", .requestId = "request-" + std::to_string(next++)};
  }
};

class GeneratedSource : public FileSource
{
public:
  std::uint32_t chunk{262144};
  std::function<void()> beforeRead;
  std::function<void(const RequestContext &)> readContext;
  std::function<void()> beforeValidate;
  std::function<void(SourceRead &)> corrupt;
  bool immutable() const override
  {
    return true;
  }
  Status validate(const Entry &) override
  {
    if (beforeValidate)
      beforeValidate();
    return Status::Ok;
  }
  SourceRead
  read(const Entry &entry, std::uint64_t offset, std::uint32_t length, const RequestContext &context) override
  {
    if (readContext)
      readContext(context);
    if (beforeRead)
      beforeRead();
    SourceRead result;
    const auto count = offset >= entry.size ? 0 : std::min<std::uint64_t>({length, chunk, entry.size - offset});
    result.bytes.resize(count);
    for (std::uint64_t i = 0; i < count; ++i)
      result.bytes[i] = static_cast<std::uint8_t>(((offset % 251 + i % 251) % 251 + 97) % 251);
    result.eof = offset >= entry.size || count == entry.size - offset;
    if (corrupt)
      corrupt(result);
    return result;
  }
};

Offer fixture(const std::shared_ptr<FileSource> &source, std::string id = "offer-A")
{
  return {
      std::move(id),
      "v1",
      {{"root", "", "", "v1", true, 0},
       {"directory", "root", "directory", "v1", true, 0},
       {"large", "directory", "large.bin", "v1", false, (std::uint64_t{4} << 30) + 123},
       {"empty", "directory", "empty.txt", "v1", false, 0}},
      source
  };
}

struct Session
{
  std::shared_ptr<GeneratedSource> source{std::make_shared<GeneratedSource>()};
  FileService service{"session-1"};
  Requests request;
  std::string lease;
  std::string handle;
  Session()
  {
    expect(service.publishOffer(fixture(source)), Status::Ok, "publish");
    auto acquired = service.acquireLease(request(), "offer-A", "v1");
    expect(acquired.status, Status::Ok, "acquire");
    lease = acquired.leaseId;
    auto opened = service.open(request(), lease, "large", "v1");
    expect(opened.status, Status::Ok, "open");
    handle = opened.handleId;
  }
};

void testRangesAndResponses()
{
  Session s;
  s.source->chunk = 4093;
  constexpr auto size = (std::uint64_t{4} << 30) + 123;
  for (auto offset : {std::uint64_t{0}, std::uint64_t{4091}, (std::uint64_t{4} << 30) + 7, size, UINT64_MAX}) {
    auto result = s.service.read(s.request(), s.lease, s.handle, offset, 8192);
    expect(result.status, Status::Ok, "range");
    const auto count = offset >= size ? 0 : std::min<std::uint64_t>(4093, size - offset);
    check(result.bytes.size() == count, "short response length");
    check(result.eof == (offset >= size || count == size - offset), "explicit EOF");
    for (std::size_t i = 0; i < result.bytes.size(); ++i)
      check(result.bytes[i] == ((offset % 251 + i % 251) % 251 + 97) % 251, "generated range data");
  }
  expect(s.service.read(s.request(), s.lease, s.handle, 0, 0).status, Status::InvalidArgument, "zero length");
  expect(s.service.read(s.request(), s.lease, s.handle, 0, 262145).status, Status::InvalidArgument, "large length");
  auto empty = s.service.open(s.request(), s.lease, "empty", "v1");
  auto emptyRead = s.service.read(s.request(), s.lease, empty.handleId, 0, 1);
  check(emptyRead.status == Status::Ok && emptyRead.bytes.empty() && emptyRead.eof, "empty file EOF");

  s.source->corrupt = [](SourceRead &r) {
    r.bytes.clear();
    r.eof = false;
  };
  auto invalid = s.service.read(s.request(), s.lease, s.handle, 0, 4096);
  expect(invalid.status, Status::InvalidSourceResponse, "zero success before EOF");
  check(invalid.bytes.empty() && !invalid.eof, "error cannot contain bytes");
  s.source->corrupt = [](SourceRead &r) { r.eof = true; };
  expect(s.service.read(s.request(), s.lease, s.handle, 0, 4096).status, Status::InvalidSourceResponse, "false EOF");
  s.source->corrupt = [](SourceRead &r) { r.bytes.resize(4097); };
  expect(
      s.service.read(s.request(), s.lease, s.handle, 0, 4096).status, Status::InvalidSourceResponse, "oversized result"
  );
  s.source->corrupt = [](SourceRead &r) { r.status = Status::Disconnected; };
  expect(
      s.service.read(s.request(), s.lease, s.handle, 0, 4096).status, Status::InvalidSourceResponse,
      "partial error response"
  );
  s.source->corrupt = [](SourceRead &r) {
    r.status = Status::SourceChanged;
    r.bytes.clear();
    r.eof = false;
  };
  expect(s.service.read(s.request(), s.lease, s.handle, 0, 4096).status, Status::SourceChanged, "version fault");

  s.source->corrupt = {};
  auto response = s.service.read(s.request(), s.lease, s.handle, 0, 4096);
  const auto expected = response;
  response.requestId = "another-request";
  expect(validateReadResult(response, expected, size), Status::InvalidSourceResponse, "request correlation");
  response = expected;
  response.version = "v2";
  expect(validateReadResult(response, expected, size), Status::InvalidSourceResponse, "version correlation");
  response = expected;
  response.sessionEpoch = "old-session";
  expect(validateReadResult(response, expected, size), Status::InvalidSourceResponse, "session correlation");
  check(s.service.usage().activeRequests == 0 && s.service.usage().inFlightBytes == 0, "read budgets returned");
}

void testLeaseLifetimeAndOldDirectory()
{
  Session s;
  expect(s.service.publishOffer(fixture(s.source, "offer-B")), Status::Ok, "replace A with B");
  expect(s.service.close(s.request(), s.handle), Status::Ok, "close A first child");
  auto listing = s.service.listDirectory(s.request(), s.lease, "directory", "v1", 0, 1);
  check(listing.status == Status::Ok && listing.entries.size() == 1 && listing.nextCursor == 1, "old directory page 1");
  listing = s.service.listDirectory(s.request(), s.lease, "directory", "v1", listing.nextCursor, 1);
  check(listing.status == Status::Ok && listing.entries.size() == 1 && listing.nextCursor == 0, "old directory page 2");
  auto oldChild = s.service.open(s.request(), s.lease, "empty", "v1");
  expect(oldChild.status, Status::Ok, "unopened A child survives replacement");
  auto released = s.service.releaseLease(s.request(), s.lease);
  check(released.status == Status::Ok && released.state == LeaseState::Draining, "release drains");
  expect(
      s.service.open(s.request(), s.lease, "large", "v1").status, Status::LeaseReleased, "draining rejects new opens"
  );
  expect(
      s.service.listDirectory(s.request(), s.lease, "directory", "v1").status, Status::LeaseReleased,
      "draining rejects directory access"
  );
  auto renewed = s.service.renewLease(s.request(), s.lease);
  check(
      renewed.status == Status::Ok && renewed.state == LeaseState::Draining, "renew cannot reactivate draining lease"
  );
  expect(s.service.read(s.request(), s.lease, oldChild.handleId, 0, 1).status, Status::Ok, "draining read");
  expect(s.service.close(s.request(), oldChild.handleId), Status::Ok, "draining close");
  expect(s.service.close(s.request(), oldChild.handleId), Status::Ok, "idempotent close");
  auto closed = s.service.releaseLease(s.request(), s.lease);
  check(closed.status == Status::Ok && closed.state == LeaseState::Closed, "idempotent release");
  expect(
      s.service.read(s.request(), s.lease, oldChild.handleId, 0, 1).status, Status::HandleClosed,
      "closed handle remains closed after draining lease closes"
  );
  check(s.service.usage().activeLeases == 0 && s.service.usage().activeHandles == 0, "lease resources returned");
}

void testExpiryBudgetsAndAuthorization()
{
  auto now = Clock::now();
  Limits limits;
  limits.maxActiveLeases = 1;
  limits.maxActiveHandles = 1;
  FileService service("session-1", limits, [&] { return now; });
  Requests request;
  auto source = std::make_shared<GeneratedSource>();
  expect(service.publishOffer(fixture(source)), Status::Ok, "publish");
  expect(service.acquireLease(request(), "offer-A", "wrong").status, Status::VersionMismatch, "offer version");
  auto lease = service.acquireLease(request(), "offer-A", "v1", 5ms);
  expect(lease.status, Status::Ok, "short lease");
  expect(service.acquireLease(request(), "offer-A", "v1").status, Status::ResourceLimit, "lease cap");
  expect(service.open(request(), lease.leaseId, "/etc/passwd", "v1").status, Status::NotFound, "no path authority");
  expect(service.open(request(), lease.leaseId, "large", "v2").status, Status::VersionMismatch, "fixed file version");
  auto opened = service.open(request(), lease.leaseId, "large", "v1");
  expect(opened.status, Status::Ok, "open");
  expect(service.open(request(), lease.leaseId, "empty", "v1").status, Status::ResourceLimit, "handle cap");
  auto wrongSession = request();
  wrongSession.sessionEpoch = "session-previous";
  expect(
      service.read(wrongSession, lease.leaseId, opened.handleId, 0, 10).status, Status::StaleSession, "stale session"
  );
  auto duplicate = request();
  expect(service.read(duplicate, lease.leaseId, opened.handleId, 0, 10).status, Status::Ok, "first request");
  expect(
      service.read(duplicate, lease.leaseId, opened.handleId, 0, 10).status, Status::InvalidArgument, "request replay"
  );
  expect(service.publishOffer(fixture(source, "offer-B")), Status::Ok, "replace");
  now += 6ms;
  expect(service.read(request(), lease.leaseId, opened.handleId, 0, 10).status, Status::LeaseExpired, "expired read");
  expect(service.renewLease(request(), lease.leaseId).status, Status::LeaseExpired, "expired renewal");
  check(service.usage().activeHandles == 0, "expiry closes handles");
  auto acquiredDuringWindow = service.acquireLease(request(), "offer-A", "v1", 5ms);
  expect(acquiredDuringWindow.status, Status::Ok, "old offer acquisition window");
  now += 31s;
  expect(service.acquireLease(request(), "offer-A", "v1").status, Status::OfferExpired, "old offer collected");
  check(service.usage().offers == 1, "old offer source reclaimed");
  auto invalid = fixture(source, "bad");
  invalid.entries.back().size = UINT64_MAX;
  expect(service.publishOffer(invalid), Status::UnsupportedSize, "size representability");
  invalid = fixture(source, "bad");
  invalid.entries[1].parentId = "directory";
  expect(service.publishOffer(invalid), Status::InvalidArgument, "directory cycle");
  service.disconnect();
  expect(service.acquireLease(request(), "offer-B", "v1").status, Status::Disconnected, "revoked session");
}

struct Gate
{
  std::mutex mutex;
  std::condition_variable changed;
  bool entered{false};
  bool released{false};
  void block()
  {
    std::unique_lock lock(mutex);
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] { return released; });
  }
  void awaitEntry()
  {
    std::unique_lock lock(mutex);
    check(changed.wait_for(lock, 5s, [&] { return entered; }), "source entered deadline");
  }
  void release()
  {
    std::lock_guard lock(mutex);
    released = true;
    changed.notify_all();
  }
};

void testConcurrentCancellationAndLateResults()
{
  for (int scenario = 0; scenario != 4; ++scenario) {
    auto now = Clock::now();
    Limits limits;
    limits.maxActiveRequests = 1;
    limits.maxInFlightBytes = 8192;
    FileService service("session-1", limits, [&] { return now; });
    Requests request;
    auto source = std::make_shared<GeneratedSource>();
    expect(service.publishOffer(fixture(source)), Status::Ok, "publish");
    auto lease = service.acquireLease(request(), "offer-A", "v1");
    auto handle = service.open(request(), lease.leaseId, "large", "v1");
    Gate gate;
    source->beforeRead = [&] { gate.block(); };
    auto context = request();
    context.deadline = now + 20ms;
    std::stop_source cancellation;
    context.stop = cancellation.get_token();
    auto pending =
        std::async(std::launch::async, [&] { return service.read(context, lease.leaseId, handle.handleId, 0, 8192); });
    gate.awaitEntry();
    expect(
        service.read(request(), lease.leaseId, handle.handleId, 0, 1).status, Status::ResourceLimit,
        "concurrent read budget"
    );
    if (scenario == 0)
      cancellation.request_stop();
    else if (scenario == 1)
      now += 21ms;
    else if (scenario == 2)
      service.disconnect();
    else
      expect(service.close(request(), handle.handleId), Status::Ok, "close accepted pending read");
    gate.release();
    const auto result = pending.get();
    const auto expected = scenario == 0   ? Status::Cancelled
                          : scenario == 1 ? Status::Timeout
                          : scenario == 2 ? Status::Disconnected
                                          : Status::Ok;
    expect(result.status, expected, "late source result");
    if (scenario != 3)
      check(result.bytes.empty() && !result.eof, "discard cancelled/timed-out/disconnected result");
    check(service.usage().activeRequests == 0 && service.usage().inFlightBytes == 0, "concurrent budgets released");
    if (scenario == 0 || scenario == 1) {
      source->beforeRead = {};
      expect(
          service.read(request(), lease.leaseId, handle.handleId, 8192, 1).status, Status::Ok,
          "late result did not poison following request"
      );
    }
  }
}

void testOpenValidationLifetime()
{
  for (int scenario = 0; scenario != 3; ++scenario) {
    auto now = Clock::now();
    FileService service("session-1", {}, [&] { return now; });
    Requests request;
    auto source = std::make_shared<GeneratedSource>();
    expect(service.publishOffer(fixture(source)), Status::Ok, "publish validation test");
    auto lease = service.acquireLease(request(), "offer-A", "v1", 5ms);
    Gate gate;
    source->beforeValidate = [&] { gate.block(); };
    auto context = request();
    auto pending = std::async(std::launch::async, [&] { return service.open(context, lease.leaseId, "large", "v1"); });
    gate.awaitEntry();
    if (scenario == 0)
      now += 6ms;
    else if (scenario == 1)
      service.disconnect();
    else
      expect(service.releaseLease(request(), lease.leaseId).status, Status::Ok, "release during pending validation");
    gate.release();
    const auto result = pending.get();
    expect(
        result.status,
        scenario == 0   ? Status::LeaseExpired
        : scenario == 1 ? Status::Disconnected
                        : Status::LeaseReleased,
        "open cannot commit after expiry/disconnect/release"
    );
    check(service.usage().activeRequests == 0 && service.usage().activeHandles == 0, "pending open resources released");
  }
}

void testDisconnectPropagatesStop()
{
  Session s;
  std::promise<void> entered;
  s.source->readContext = [&](const RequestContext &context) {
    std::mutex mutex;
    std::condition_variable changed;
    std::stop_callback cancellation(context.stop, [&] {
      std::lock_guard lock(mutex);
      changed.notify_all();
    });
    entered.set_value();
    std::unique_lock lock(mutex);
    check(changed.wait_for(lock, 5s, [&] { return context.stop.stop_requested(); }), "disconnect source stop deadline");
  };
  auto context = s.request();
  auto pending = std::async(std::launch::async, [&] { return s.service.read(context, s.lease, s.handle, 0, 8); });
  check(entered.get_future().wait_for(5s) == std::future_status::ready, "source entered");
  s.service.disconnect();
  check(pending.wait_for(1s) == std::future_status::ready, "cooperative source exits on disconnect");
  expect(pending.get().status, Status::Disconnected, "disconnected result");
}

void testRetainedStateBudget()
{
  Limits limits;
  limits.maxRetainedBytes = 512;
  FileService service("session-1", limits);
  Requests request;
  auto source = std::make_shared<GeneratedSource>();
  expect(service.publishOffer(fixture(source)), Status::Ok, "retained budget publish");
  Status status = Status::Ok;
  for (unsigned int count = 0; count < 10 && status != Status::ResourceLimit; ++count)
    status = service.acquireLease(request(), "missing", "v1").status;
  expect(status, Status::ResourceLimit, "request replay storage bounded by bytes");
  check(service.usage().retainedBytes <= limits.maxRetainedBytes, "retained byte limit respected");
  FileService other("session-1");
  auto oversized = request();
  oversized.requestId.assign(257, 'x');
  expect(other.acquireLease(oversized, "missing", "v1").status, Status::InvalidArgument, "request ID bound");
  auto offer = fixture(source);
  offer.entries[1].version.assign(257, 'x');
  expect(other.publishOffer(offer), Status::InvalidArgument, "metadata identifier bound");
  offer = fixture(source);
  offer.acquisitionWindow = std::chrono::milliseconds::max();
  expect(other.publishOffer(offer), Status::InvalidArgument, "acquisition duration overflow rejected");
}

class TempDirectory
{
public:
  TempDirectory()
  {
    auto pattern = (std::filesystem::temp_directory_path() / "deskflow-clipboard-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back(0);
    const auto path = ::mkdtemp(writable.data());
    if (!path)
      throw std::runtime_error("mkdtemp failed");
    directory = path;
  }
  ~TempDirectory()
  {
    std::filesystem::remove_all(directory);
  }
  std::filesystem::path directory;
};

void writeFile(const std::filesystem::path &path, const std::string &data)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << data;
  if (!out)
    throw std::runtime_error("write fixture failed");
}

void testLocalDetectableSource()
{
  TempDirectory temp;
  const auto path = temp.directory / "含 空格.txt";
  std::string content;
  for (unsigned int i = 0; i < 65539; ++i)
    content.push_back(static_cast<char>(i % 251));
  writeFile(path, content);
  auto captured = LocalFileSource::capture({path}, "local-A", "v1");
  expect(captured.status, Status::Ok, "local capture");
  const auto entry = captured.offer.entries.at(1);
  FileService service("session-1");
  Requests request;
  expect(service.publishOffer(std::move(captured.offer)), Status::Ok, "local publish");
  auto lease = service.acquireLease(request(), "local-A", "v1");
  expect(
      service.open(request(), lease.leaseId, entry.id, entry.version).status, Status::SourceNotStable,
      "local immutable refused"
  );
  auto handle = service.open(request(), lease.leaseId, entry.id, entry.version, Consistency::Detectable);
  expect(handle.status, Status::Ok, "explicit detectable open");
  for (const auto offset : {0u, 65530u, 4091u, 15000u}) {
    auto result = service.read(request(), lease.leaseId, handle.handleId, offset, 8192);
    expect(result.status, Status::Ok, "local random read");
    check(std::string(result.bytes.begin(), result.bytes.end()) == content.substr(offset, 8192), "local random bytes");
  }
  // Same length mutation; restoring mtime alone must not evade ctime detection.
  const auto mtime = std::filesystem::last_write_time(path);
  writeFile(path, std::string(content.size(), 'x'));
  std::filesystem::last_write_time(path, mtime);
  auto changed = service.read(request(), lease.leaseId, handle.handleId, 0, 8192);
  expect(changed.status, Status::SourceChanged, "in-place mutation");
  check(changed.bytes.empty() && !changed.eof, "mutation has no success bytes");

  auto replaced = LocalFileSource::capture({path}, "local-B", "v1");
  expect(replaced.status, Status::Ok, "replacement capture");
  const auto replacedEntry = replaced.offer.entries.at(1);
  const auto replacement = temp.directory / "new.txt";
  writeFile(replacement, "must never read replacement bytes");
  std::filesystem::rename(replacement, path);
  auto replacementRead = replaced.offer.source->read(replacedEntry, 0, 8192, request());
  check(
      replacementRead.status == Status::SourceChanged ||
          (replacementRead.status == Status::Ok && replacementRead.bytes == std::vector<std::uint8_t>(8192, 'x')),
      "replacement cannot retarget pinned descriptor"
  );

  const auto link = temp.directory / "symlink";
  std::filesystem::create_symlink(path, link);
  expect(LocalFileSource::capture({link}, "symlink", "v1").status, Status::InvalidArgument, "symlink refused");
  const auto fifo = temp.directory / "fifo";
  check(::mkfifo(fifo.c_str(), 0600) == 0, "create FIFO");
  expect(
      LocalFileSource::capture({fifo}, "fifo", "v1").status, Status::InvalidArgument, "FIFO refused without blocking"
  );
  const auto denied = temp.directory / "denied.txt";
  writeFile(denied, "private");
  ::chmod(denied.c_str(), 0000);
  if (::geteuid() != 0)
    expect(LocalFileSource::capture({denied}, "denied", "v1").status, Status::PermissionDenied, "permission denied");
  else
    std::cout << "SKIP permission-denied capture: effective uid is root\n";
  ::chmod(denied.c_str(), 0600);
  expect(
      LocalFileSource::capture({temp.directory / "missing"}, "missing", "v1").status, Status::SourceMissing,
      "missing file"
  );
  const auto folder = temp.directory / "folder";
  std::filesystem::create_directory(folder);
  writeFile(folder / "one", "1");
  std::filesystem::create_directory(folder / "empty");
  auto tree = LocalFileSource::capture({folder}, "directory", "v1");
  expect(tree.status, Status::Ok, "directory capture");
  writeFile(folder / "later", "2");
  check(tree.offer.entries.size() == 4, "captured membership remains fixed");
  expect(
      LocalFileSource::capture({folder}, "limited", "v1", 2).status, Status::ResourceLimit, "capture descriptor cap"
  );

  const auto actualParent = temp.directory / "actual";
  std::filesystem::create_directories(actualParent / "child");
  writeFile(actualParent / "selected.txt", "selected through OS path resolution");
  writeFile(temp.directory / "selected.txt", "wrong lexical-normalized path");
  std::filesystem::create_directory_symlink(actualParent / "child", temp.directory / "parent-link");
  auto resolved = LocalFileSource::capture({temp.directory / "parent-link" / ".." / "selected.txt"}, "resolved", "v1");
  expect(resolved.status, Status::Ok, "capture preserves parent path semantics");
  auto resolvedRead = resolved.offer.source->read(resolved.offer.entries.at(1), 0, 64, request());
  check(
      std::string(resolvedRead.bytes.begin(), resolvedRead.bytes.end()) == "selected through OS path resolution",
      "lexical normalization must not select another file"
  );

  // Sparse local file exercises pread's actual 64-bit offset, without writing 4 GiB.
  const auto largePath = temp.directory / "large-sparse.bin";
  const auto largeOffset = (std::uint64_t{4} << 30) + 7;
  const int largeFd = ::open(largePath.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
  check(largeFd >= 0, "create sparse file");
  const auto count = ::pwrite(largeFd, "beyond-4GiB", 11, static_cast<off_t>(largeOffset));
  ::close(largeFd);
  check(count == 11, "write sparse tail");
  auto sparse = LocalFileSource::capture({largePath}, "sparse", "v1");
  expect(sparse.status, Status::Ok, "sparse capture");
  const auto sparseEntry = sparse.offer.entries.at(1);
  auto sparseRead = sparse.offer.source->read(sparseEntry, largeOffset, 32, request());
  check(
      sparseRead.status == Status::Ok && sparseRead.eof &&
          std::string(sparseRead.bytes.begin(), sparseRead.bytes.end()) == "beyond-4GiB",
      "local offset beyond 4 GiB"
  );
}

} // namespace

int main()
{
  try {
    testRangesAndResponses();
    testLeaseLifetimeAndOldDirectory();
    testExpiryBudgetsAndAuthorization();
    testConcurrentCancellationAndLateResults();
    testOpenValidationLifetime();
    testDisconnectPropagatesStop();
    testRetainedStateBudget();
    testLocalDetectableSource();
    std::cout << "PASS clipboard service: ranges, response validation, leases, old directories, budgets, "
                 "concurrency, cancellation, timeouts, disconnect, detectable local files\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL clipboard service: " << error.what() << '\n';
    return 1;
  }
}
