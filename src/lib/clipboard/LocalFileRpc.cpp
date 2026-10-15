/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "LocalFileRpc.h"
#include "ClipboardBroker.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <exception>
#include <fstream>
#include <mutex>
#include <poll.h>
#include <span>
#ifdef __linux__
#include <sys/random.h>
#endif
#include <cstdlib>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace deskflow::clipboard {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t maxFrame = 1024 * 1024;
constexpr std::size_t maxFields = 16384;
using Message = RpcMessage;

struct Fd
{
  int value{-1};
  explicit Fd(int value = -1) : value(value)
  {
  }
  ~Fd()
  {
    if (value >= 0)
      ::close(value);
  }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
};

struct PoisonOnException
{
  int fd;
  int exceptions{std::uncaught_exceptions()};
  ~PoisonOnException()
  {
    if (std::uncaught_exceptions() > exceptions)
      ::shutdown(fd, SHUT_RDWR);
  }
};

[[noreturn]] void fail(Status status, const std::string &message)
{
  throw RpcError(status, message);
}

std::uint64_t number(const std::string &text)
{
  std::uint64_t value{};
  auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
    fail(Status::InvalidSourceResponse, "Invalid unsigned decimal field");
  return value;
}

std::uint32_t number32(const std::string &text)
{
  const auto value = number(text);
  if (value > UINT32_MAX)
    fail(Status::InvalidSourceResponse, "32-bit field overflow");
  return static_cast<std::uint32_t>(value);
}

void append32(std::vector<std::uint8_t> &out, std::uint32_t value)
{
  for (int shift = 24; shift >= 0; shift -= 8)
    out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint32_t take32(std::span<const std::uint8_t> &data)
{
  if (data.size() < 4)
    fail(Status::InvalidSourceResponse, "Truncated length field");
  std::uint32_t value{};
  for (unsigned int i = 0; i < 4; ++i)
    value = (value << 8) | data[i];
  data = data.subspan(4);
  return value;
}

void waitFor(int fd, short events, Clock::time_point deadline, const std::atomic_bool *stop)
{
  for (;;) {
    if (stop && stop->load())
      fail(Status::Cancelled, "Transport cancelled");
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0)
      fail(Status::Timeout, "Transport deadline exceeded");
    pollfd p{fd, events, 0};
    const int result = ::poll(&p, 1, static_cast<int>(std::min<std::int64_t>(left + 1, 100)));
    if (result < 0 && errno == EINTR)
      continue;
    if (result < 0)
      fail(Status::IoError, "poll failed");
    if (result == 0)
      continue;
    if ((p.revents & events) != 0)
      return;
    if ((p.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
      fail(Status::Disconnected, "Transport disconnected");
  }
}

void transfer(
    int fd, std::span<std::uint8_t> data, bool writing, Clock::time_point deadline, const std::atomic_bool *stop
)
{
  while (!data.empty()) {
    waitFor(fd, writing ? POLLOUT : POLLIN, deadline, stop);
    const auto count = writing ? ::send(
                                     fd, data.data(), data.size(),
                                     MSG_DONTWAIT
#ifndef __APPLE__
                                         | MSG_NOSIGNAL
#endif
                                 )
                               : ::recv(fd, data.data(), data.size(), MSG_DONTWAIT);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    if (count <= 0)
      fail(Status::Disconnected, "Incomplete transport frame");
    data = data.subspan(static_cast<std::size_t>(count));
  }
}

std::vector<std::uint8_t> encode(const Message &message)
{
  if (message.fields.size() > maxFields || message.bytes.size() > maxFrame)
    fail(Status::ResourceLimit, "Frame budget exceeded");
  std::vector<std::uint8_t> out(4);
  append32(out, static_cast<std::uint32_t>(message.fields.size()));
  for (const auto &field : message.fields) {
    if (field.size() > 65536 || out.size() + 4 + field.size() > maxFrame)
      fail(Status::ResourceLimit, "Metadata frame budget exceeded");
    append32(out, static_cast<std::uint32_t>(field.size()));
    out.insert(out.end(), field.begin(), field.end());
  }
  if (out.size() + 4 + message.bytes.size() > maxFrame)
    fail(Status::ResourceLimit, "Frame budget exceeded");
  append32(out, static_cast<std::uint32_t>(message.bytes.size()));
  out.insert(out.end(), message.bytes.begin(), message.bytes.end());
  const auto length = static_cast<std::uint32_t>(out.size() - 4);
  for (unsigned int i = 0; i < 4; ++i)
    out[i] = static_cast<std::uint8_t>(length >> ((3 - i) * 8));
  return out;
}
void sendMessage(int fd, const Message &message, Clock::time_point deadline, const std::atomic_bool *stop)
{
  auto out = encode(message);
  transfer(fd, out, true, deadline, stop);
}

Message decode(std::span<const std::uint8_t> view)
{
  if (view.size() < 8 || view.size() > maxFrame)
    fail(Status::InvalidSourceResponse, "Invalid frame size");
  const auto count = take32(view);
  if (count > maxFields || count > view.size() / 4)
    fail(Status::InvalidSourceResponse, "Invalid field count");
  Message result;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto size = take32(view);
    if (size > 65536 || size > view.size())
      fail(Status::InvalidSourceResponse, "Invalid field size");
    result.fields.emplace_back(reinterpret_cast<const char *>(view.data()), size);
    view = view.subspan(size);
  }
  const auto bytes = take32(view);
  if (bytes != view.size())
    fail(Status::InvalidSourceResponse, "Invalid body length");
  result.bytes.assign(view.begin(), view.end());
  return result;
}
Message receiveMessage(int fd, Clock::time_point deadline, const std::atomic_bool *stop)
{
  std::array<std::uint8_t, 4> header{};
  transfer(fd, header, false, deadline, stop);
  std::span<const std::uint8_t> view(header);
  const auto length = take32(view);
  if (length < 8 || length > maxFrame)
    fail(Status::InvalidSourceResponse, "Invalid frame length");
  std::vector<std::uint8_t> buffer(length);
  transfer(fd, buffer, false, deadline, stop);
  return decode(buffer);
}

int configureSocket(int fd)
{
  if (fd >= 0) {
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
      ::close(fd);
      return -1;
    }
#ifdef __APPLE__
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
  }
  return fd;
}
int unixSocket()
{
  return configureSocket(::socket(AF_UNIX, SOCK_STREAM, 0));
}
int acceptSocket(int fd)
{
  return configureSocket(::accept(fd, nullptr, nullptr));
}

sockaddr_un address(const std::filesystem::path &path)
{
  sockaddr_un result{};
  result.sun_family = AF_UNIX;
  const auto text = path.string();
  if (text.empty() || text.size() >= sizeof(result.sun_path) || text.find('\0') != std::string::npos)
    fail(Status::InvalidArgument, "Invalid Unix socket path");
  std::memcpy(result.sun_path, text.c_str(), text.size() + 1);
  return result;
}

void checkPeer(int fd)
{
#ifdef __APPLE__
  uid_t uid;
  gid_t gid;
  if (::getpeereid(fd, &uid, &gid) != 0 || uid != ::getuid())
    fail(Status::PermissionDenied, "Unix socket peer must be the same user");
#else
  ucred peer{};
  socklen_t size = sizeof(peer);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != ::getuid())
    fail(Status::PermissionDenied, "Unix socket peer must be the same user");
#endif
}

std::string epoch()
{
  std::array<unsigned char, 16> bytes{};
#ifdef __APPLE__
  ::arc4random_buf(bytes.data(), bytes.size());
#else
  if (::getrandom(bytes.data(), bytes.size(), 0) != static_cast<ssize_t>(bytes.size()))
    fail(Status::IoError, "Cannot create session epoch");
#endif
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (const auto byte : bytes) {
    result += digits[byte >> 4];
    result += digits[byte & 15];
  }
  return result;
}

Status parseStatus(const std::string &text)
{
  for (int i = 0; i <= static_cast<int>(Status::IoError); ++i)
    if (statusName(static_cast<Status>(i)) == text)
      return static_cast<Status>(i);
  fail(Status::InvalidSourceResponse, "Unknown status");
}

struct Fields
{
  const std::vector<std::string> &fields;
  std::size_t position{0};
  const std::string &get()
  {
    if (position >= fields.size())
      fail(Status::InvalidSourceResponse, "Missing field");
    return fields[position++];
  }
  void end() const
  {
    if (position != fields.size())
      fail(Status::InvalidSourceResponse, "Unexpected fields");
  }
};

void putOffer(Message &message, const OfferInfo &offer)
{
  auto &f = message.fields;
  f.insert(
      f.end(), {offer.sessionEpoch, offer.offerId, offer.offerVersion, std::to_string(offer.acquisitionWindowMs),
                offer.immutableVersion ? "1" : "0", std::to_string(offer.roots.size())}
  );
  f.insert(f.end(), offer.roots.begin(), offer.roots.end());
  f.push_back(std::to_string(offer.files.size()));
  for (const auto &file : offer.files)
    f.insert(
        f.end(), {file.fileId, file.parentId, file.name, file.relativePath, file.version,
                  file.kind == FileKind::Directory ? "directory" : "file", std::to_string(file.size)}
    );
}

OfferInfo getOffer(Fields &f)
{
  OfferInfo result;
  result.sessionEpoch = f.get();
  result.offerId = f.get();
  result.offerVersion = f.get();
  result.acquisitionWindowMs = number32(f.get());
  const auto immutable = f.get();
  if (immutable != "0" && immutable != "1")
    fail(Status::InvalidSourceResponse, "Invalid capability");
  result.immutableVersion = immutable == "1";
  const auto roots = number(f.get());
  if (roots > 64)
    fail(Status::InvalidSourceResponse, "Root budget exceeded");
  for (std::uint64_t i = 0; i < roots; ++i)
    result.roots.push_back(f.get());
  const auto count = number(f.get());
  if (count > 1024)
    fail(Status::InvalidSourceResponse, "Manifest budget exceeded");
  for (std::uint64_t i = 0; i < count; ++i) {
    FileInfo file;
    file.fileId = f.get();
    file.parentId = f.get();
    file.name = f.get();
    file.relativePath = f.get();
    file.version = f.get();
    const auto kind = f.get();
    if (kind != "file" && kind != "directory")
      fail(Status::InvalidSourceResponse, "Invalid file kind");
    file.kind = kind == "directory" ? FileKind::Directory : FileKind::File;
    file.size = number(f.get());
    result.files.push_back(std::move(file));
  }
  if (result.sessionEpoch.empty() || result.offerId.empty() || result.offerVersion.empty())
    fail(Status::InvalidSourceResponse, "Missing offer identity");
  std::unordered_set<std::string> ids;
  std::unordered_set<std::string> paths;
  for (const auto &file : result.files) {
    if (file.fileId.empty() || file.version.empty() || !ids.insert(file.fileId).second || file.relativePath.empty() ||
        !paths.insert(file.relativePath).second || file.relativePath.find('\0') != std::string::npos ||
        std::filesystem::path(file.relativePath).is_absolute())
      fail(Status::InvalidSourceResponse, "Invalid manifest identity or path");
    for (const auto &component : std::filesystem::path(file.relativePath))
      if (component == ".." || component == ".")
        fail(Status::InvalidSourceResponse, "Invalid manifest path component");
  }
  for (const auto &root : result.roots)
    if (!ids.contains(root))
      fail(Status::InvalidSourceResponse, "Invalid root identity");
  ClipboardBroker::Options validationOptions;
  validationOptions.localOriginId = "rpc-validator";
  validationOptions.ownerId = "rpc-validator-owner";
  ClipboardBroker validator(validationOptions);
  if (validator.beginSession(result.sessionEpoch, "rpc-source").status != Status::Ok ||
      validator.receiveRemote({result, "rpc-source", 1, OfferDirection::Remote}).status != Status::Ok)
    fail(Status::InvalidSourceResponse, "Invalid manifest hierarchy, names or budgets");
  f.end();
  return result;
}

void putLease(Message &m, const LeaseResult &r)
{
  m.fields[2] = statusName(r.status);
  m.fields.insert(
      m.fields.end(), {r.leaseId, r.offerVersion, std::to_string(r.grantedTtlMs), std::string(leaseStateName(r.state))}
  );
}

LeaseResult getLease(const Message &message)
{
  Fields f{message.fields, 2};
  LeaseResult result;
  result.status = parseStatus(f.get());
  result.leaseId = f.get();
  result.offerVersion = f.get();
  result.grantedTtlMs = number32(f.get());
  const auto state = f.get();
  bool matched = false;
  for (int i = 0; i <= static_cast<int>(LeaseState::Revoked); ++i) {
    if (leaseStateName(static_cast<LeaseState>(i)) == state) {
      result.state = static_cast<LeaseState>(i);
      matched = true;
    }
  }
  if (!matched)
    fail(Status::InvalidSourceResponse, "Invalid lease state");
  f.end();
  return result;
}

std::string quote(const std::string &value)
{
  std::string out = "\"";
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char c : value) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c < 32) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else
      out += c;
  }
  return out + '"';
}

void trace(std::ofstream &log, const ReadResult &r, Clock::time_point start)
{
  if (!log.is_open())
    return;
  const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
  log << "{\"schemaVersion\":1,\"side\":\"source\",\"operation\":\"readRange\",\"sessionEpoch\":"
      << quote(r.sessionEpoch) << ",\"offerId\":" << quote(r.offerId) << ",\"fileId\":" << quote(r.fileId)
      << ",\"version\":" << quote(r.version) << ",\"leaseId\":" << quote(r.leaseId)
      << ",\"handleId\":" << quote(r.handleId) << ",\"requestId\":" << quote(r.requestId)
      << ",\"offset\":" << quote(std::to_string(r.offset)) << ",\"length\":" << r.length
      << ",\"returnedBytes\":" << r.bytes.size() << ",\"eof\":" << (r.eof ? "true" : "false")
      << ",\"status\":" << quote(std::string(statusName(r.status))) << ",\"durationUs\":" << duration
      << ",\"activeRequests\":1,\"inFlightBytes\":" << r.length << "}\n";
  log.flush();
}

Message dispatch(
    LocalFileSource &source, const std::vector<std::filesystem::path> &paths, const Message &request,
    const FileRpcServerOptions &options, int fd, const std::atomic_bool &stop, std::ofstream &log
)
{
  Fields f{request.fields};
  const auto op = f.get();
  const auto id = f.get();
  if (id.empty() || !request.bytes.empty())
    fail(Status::InvalidSourceResponse, "Invalid request envelope");
  Message response{{op, id, "OK"}, {}};
  if (op == "hello" || op == "refresh") {
    f.end();
    const auto result = source.createOffer(paths);
    response.fields[2] = statusName(result.status);
    if (result.status == Status::Ok)
      putOffer(response, result.offer);
  } else if (op == "acquire") {
    const auto epoch = f.get();
    const auto offer = f.get();
    const auto version = f.get();
    const auto ttl = number32(f.get());
    f.end();
    putLease(response, source.acquire(epoch, offer, version, ttl));
  } else if (op == "renew" || op == "release") {
    const auto epoch = f.get();
    const auto lease = f.get();
    const auto ttl = op == "renew" ? number32(f.get()) : 0;
    f.end();
    putLease(response, op == "renew" ? source.renew(epoch, lease, ttl) : source.release(epoch, lease));
  } else if (op == "open") {
    const auto epoch = f.get();
    const auto lease = f.get();
    const auto file = f.get();
    const auto version = f.get();
    const auto mode = f.get();
    f.end();
    if (mode != "immutable" && mode != "detectable")
      fail(Status::InvalidArgument, "Invalid consistency mode");
    const auto result = source.open(
        epoch, lease, file, version, mode == "immutable" ? ConsistencyMode::Immutable : ConsistencyMode::Detectable
    );
    response.fields[2] = statusName(result.status);
    response.fields.insert(response.fields.end(), {result.handleId, result.version, std::to_string(result.size)});
  } else if (op == "read") {
    ReadRequest r;
    r.sessionEpoch = f.get();
    r.leaseId = f.get();
    r.handleId = f.get();
    r.requestId = id;
    r.offset = number(f.get());
    r.length = number32(f.get());
    f.end();
    const auto start = Clock::now();
    auto deadline = start + std::chrono::milliseconds(options.delayMs);
    while (Clock::now() < deadline) {
      if (stop.load())
        fail(Status::Cancelled, "Server stopped");
      pollfd p{fd, POLLIN, 0};
      if (::poll(&p, 1, 5) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0)
        fail(Status::Disconnected, "Peer closed or pipelined a request during injected delay");
    }
    auto result = source.read(r);
    trace(log, result, start);
    response.fields[2] = statusName(result.status);
    response.fields.insert(
        response.fields.end(),
        {result.sessionEpoch, result.offerId, result.fileId, result.version, result.leaseId, result.handleId,
         result.requestId, std::to_string(result.offset), std::to_string(result.length), result.eof ? "1" : "0"}
    );
    response.bytes = std::move(result.bytes);
  } else if (op == "close") {
    const auto epoch = f.get();
    const auto handle = f.get();
    f.end();
    response.fields[2] = statusName(source.close(epoch, handle));
  } else
    fail(Status::InvalidArgument, "Unknown operation");
  return response;
}
} // namespace

std::vector<std::uint8_t> encodeRpcFrame(const RpcMessage &message)
{
  return encode(message);
}
RpcMessage decodeRpcBody(std::span<const std::uint8_t> body)
{
  return decode(body);
}
RpcMessage offerMessage(const OfferInfo &offer)
{
  Message m;
  putOffer(m, offer);
  return m;
}
OfferInfo parseOfferMessage(const RpcMessage &message)
{
  if (!message.bytes.empty())
    fail(Status::InvalidSourceResponse, "Unexpected offer bytes");
  Fields fields{message.fields};
  auto offer = getOffer(fields);
  fields.end();
  return offer;
}
RpcMessage dispatchFileRequest(LocalFileSource &source, const RpcMessage &request)
{
  if (request.fields.size() < 2 || request.fields[0] == "hello" || request.fields[0] == "refresh")
    fail(Status::InvalidArgument, "Offers are captured by the local clipboard owner only");
  static const std::atomic_bool stop{false};
  std::ofstream noLog;
  return dispatch(source, {}, request, {}, -1, stop, noLog);
}

RpcError::RpcError(Status status, const std::string &message) : std::runtime_error(message), status(status)
{
}

void serveLocalFiles(const FileRpcServerOptions &options, const std::atomic_bool &stop)
{
  if (options.paths.empty() || options.responseBytes == 0 || options.responseBytes > 262144 || options.delayMs > 60000)
    fail(Status::InvalidArgument, "Invalid file server options");
  const auto path = std::filesystem::absolute(options.socketPath);
  struct stat attributes{};
  if (::stat(path.parent_path().c_str(), &attributes) != 0 || !S_ISDIR(attributes.st_mode) ||
      attributes.st_uid != ::getuid() || (attributes.st_mode & 0077) != 0)
    fail(Status::PermissionDenied, "Socket parent must be a private directory owned by this user (0700)");
  if (::lstat(path.c_str(), &attributes) == 0 || errno != ENOENT)
    fail(Status::InvalidArgument, "Socket path already exists");
  const auto addr = address(path);
  Fd listener(unixSocket());
  if (listener.value < 0 || ::bind(listener.value, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0)
    fail(Status::IoError, "Cannot bind Unix socket");
  struct Unlink
  {
    std::filesystem::path path;
    ~Unlink()
    {
      ::unlink(path.c_str());
    }
  } cleanup{path};
  if (::chmod(path.c_str(), 0600) != 0 || ::listen(listener.value, 4) != 0)
    fail(Status::IoError, "Cannot listen on Unix socket");
  std::ofstream log;
  if (!options.logPath.empty()) {
    log.open(options.logPath, std::ios::out | std::ios::trunc);
    if (!log)
      fail(Status::IoError, "Cannot open file request log");
  }
  while (!stop.load()) {
    try {
      waitFor(listener.value, POLLIN, Clock::now() + std::chrono::hours(24), &stop);
    } catch (const RpcError &error) {
      if (stop.load())
        break;
      throw;
    }
    Fd peer(acceptSocket(listener.value));
    if (peer.value < 0)
      continue;
    try {
      checkPeer(peer.value);
      LocalFileSource::Options sourceOptions;
      sourceOptions.sessionEpoch = epoch();
      sourceOptions.maxResponseBytes = options.responseBytes;
      sourceOptions.maxEntriesPerOffer = 1024;
      sourceOptions.maxManifestBytes = 512 * 1024;
      LocalFileSource source(sourceOptions);
      bool ready = false;
      std::uint64_t lastRequestId = 0;
      while (!stop.load()) {
        auto request = receiveMessage(peer.value, Clock::now() + std::chrono::minutes(5), &stop);
        if (request.fields.empty() || (!ready && request.fields[0] != "hello") ||
            (ready && request.fields[0] == "hello"))
          fail(Status::InvalidArgument, "Hello required exactly once before file requests");
        if (request.fields.size() < 2 || number(request.fields[1]) <= lastRequestId)
          fail(Status::InvalidArgument, "Request IDs must increase within this serial session");
        lastRequestId = number(request.fields[1]);
        auto response = dispatch(source, options.paths, request, options, peer.value, stop, log);
        ready = true;
        sendMessage(peer.value, response, Clock::now() + std::chrono::seconds(5), &stop);
      }
      source.disconnect();
    } catch (const RpcError &) {
      // Malformed/expired/disconnected clients cannot leave a partial response or retain a session.
      // LocalFileSource destruction closes all handles and leases before accepting another client.
    }
  }
}

struct FileRpcClient::Impl
{
  Fd fd;
  std::uint32_t timeoutMs;
  std::string sessionEpoch;
  std::uint64_t nextRequest{1};
  std::atomic_bool cancelled{false};
  std::mutex mutex;
  std::mutex operationMutex;
  struct Handle
  {
    FileInfo file;
    std::string leaseId;
    std::string offerId;
  };
  std::unordered_map<std::string, OfferInfo> leases;
  std::unordered_map<std::string, Handle> handles;
  explicit Impl(int fd, std::uint32_t timeout) : fd(fd), timeoutMs(timeout)
  {
  }
  std::unique_lock<std::mutex> operationLock()
  {
    std::unique_lock lock(operationMutex, std::try_to_lock);
    if (!lock.owns_lock())
      fail(Status::ResourceLimit, "One in-flight operation per local connection");
    return lock;
  }
  Message call(std::vector<std::string> fields)
  {
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock())
      fail(Status::ResourceLimit, "One in-flight request per local connection");
    if (cancelled.load())
      fail(Status::Cancelled, "Transport cancelled");
    const auto id = std::to_string(nextRequest++);
    fields.insert(fields.begin() + 1, id);
    const auto operation = fields.front();
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    try {
      sendMessage(fd.value, {std::move(fields), {}}, deadline, &cancelled);
      auto response = receiveMessage(fd.value, deadline, &cancelled);
      if (response.fields.size() < 3 || response.fields[0] != operation || response.fields[1] != id)
        fail(Status::InvalidSourceResponse, "Response correlation failed");
      parseStatus(response.fields[2]);
      if (operation != "read" && !response.bytes.empty())
        fail(Status::InvalidSourceResponse, "Unexpected response body");
      if (cancelled.load())
        fail(Status::Cancelled, "Transport cancelled before response delivery");
      return response;
    } catch (const RpcError &error) {
      ::shutdown(fd.value, SHUT_RDWR); // Discard incomplete/late responses; never reuse this stream.
      if (cancelled.load())
        fail(Status::Cancelled, "Transport cancelled");
      throw;
    }
  }
};

FileRpcClient::FileRpcClient(const std::filesystem::path &socketPath, std::uint32_t timeoutMs)
{
  if (timeoutMs == 0 || timeoutMs > 600000)
    fail(Status::InvalidArgument, "Invalid timeout");
  const auto addr = address(socketPath);
  const int fd = unixSocket();
  if (fd < 0)
    fail(Status::IoError, "Cannot create Unix socket");
  m_impl = std::make_unique<Impl>(fd, timeoutMs);
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0)
      break;
    if (errno == EINPROGRESS) {
      waitFor(fd, POLLOUT, deadline, nullptr);
      int error = 0;
      socklen_t size = sizeof(error);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0)
        fail(Status::Disconnected, "Cannot connect to local file service");
      break;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      fail(Status::Disconnected, "Cannot connect to local file service");
    if (Clock::now() >= deadline)
      fail(Status::Timeout, "Local file service connect deadline exceeded");
    ::poll(nullptr, 0, 1); // AF_UNIX backlog full: no connection has been started.
  }
  checkPeer(fd);
}
FileRpcClient::~FileRpcClient() = default;

OfferResult FileRpcClient::hello()
{
  auto lock = m_impl->operationLock();
  PoisonOnException guard{m_impl->fd.value};
  auto message = m_impl->call({"hello"});
  OfferResult result;
  result.status = parseStatus(message.fields[2]);
  if (result.status == Status::Ok) {
    Fields fields{message.fields, 3};
    result.offer = getOffer(fields);
    m_impl->sessionEpoch = result.offer.sessionEpoch;
  } else if (message.fields.size() != 3)
    fail(Status::InvalidSourceResponse, "Invalid offer error");
  return result;
}
OfferResult FileRpcClient::refresh()
{
  auto lock = m_impl->operationLock();
  PoisonOnException guard{m_impl->fd.value};
  auto message = m_impl->call({"refresh"});
  OfferResult result;
  result.status = parseStatus(message.fields[2]);
  if (result.status == Status::Ok) {
    Fields fields{message.fields, 3};
    result.offer = getOffer(fields);
    if (result.offer.sessionEpoch != m_impl->sessionEpoch)
      fail(Status::InvalidSourceResponse, "Epoch changed");
  } else if (message.fields.size() != 3)
    fail(Status::InvalidSourceResponse, "Invalid offer error");
  return result;
}
LeaseResult FileRpcClient::acquire(const OfferInfo &offer, std::uint32_t ttlMs)
{
  auto lock = m_impl->operationLock();
  if (m_impl->leases.size() >= 4096)
    fail(Status::ResourceLimit, "Lease tracking budget exceeded");
  PoisonOnException guard{m_impl->fd.value};
  auto result =
      getLease(m_impl->call({"acquire", offer.sessionEpoch, offer.offerId, offer.offerVersion, std::to_string(ttlMs)}));
  if (result.status == Status::Ok) {
    if (result.leaseId.empty() || result.offerVersion != offer.offerVersion || result.grantedTtlMs == 0 ||
        result.state != LeaseState::Active || m_impl->leases.contains(result.leaseId))
      fail(Status::InvalidSourceResponse, "Invalid acquired lease");
    m_impl->leases.emplace(result.leaseId, offer);
  }
  return result;
}
LeaseResult FileRpcClient::renew(const std::string &leaseId, std::uint32_t ttlMs)
{
  auto lock = m_impl->operationLock();
  PoisonOnException guard{m_impl->fd.value};
  const auto result = getLease(m_impl->call({"renew", m_impl->sessionEpoch, leaseId, std::to_string(ttlMs)}));
  if (result.status == Status::Ok) {
    const auto known = m_impl->leases.find(leaseId);
    if (known == m_impl->leases.end() || result.leaseId != leaseId ||
        result.offerVersion != known->second.offerVersion || result.grantedTtlMs == 0 ||
        (result.state != LeaseState::Active && result.state != LeaseState::Draining))
      fail(Status::InvalidSourceResponse, "Renew response is not bound to a valid tracked lease");
  }
  return result;
}
LeaseResult FileRpcClient::release(const std::string &leaseId)
{
  auto lock = m_impl->operationLock();
  PoisonOnException guard{m_impl->fd.value};
  const auto result = getLease(m_impl->call({"release", m_impl->sessionEpoch, leaseId}));
  if (result.status == Status::Ok) {
    const auto known = m_impl->leases.find(leaseId);
    if (known == m_impl->leases.end() || result.leaseId != leaseId ||
        result.offerVersion != known->second.offerVersion ||
        (result.state != LeaseState::Closed && result.state != LeaseState::Draining &&
         result.state != LeaseState::Expired && result.state != LeaseState::Revoked))
      fail(Status::InvalidSourceResponse, "Release response is not bound to a tracked lease");
  }
  return result;
}
OpenResult FileRpcClient::open(const std::string &leaseId, const FileInfo &file, ConsistencyMode mode)
{
  auto lock = m_impl->operationLock();
  if (m_impl->handles.size() >= 256)
    fail(Status::ResourceLimit, "Handle tracking budget exceeded");
  if (mode != ConsistencyMode::Immutable && mode != ConsistencyMode::Detectable)
    fail(Status::InvalidArgument, "Invalid consistency mode");
  PoisonOnException guard{m_impl->fd.value};
  auto message = m_impl->call(
      {"open", m_impl->sessionEpoch, leaseId, file.fileId, file.version,
       mode == ConsistencyMode::Immutable ? "immutable" : "detectable"}
  );
  Fields f{message.fields, 2};
  OpenResult r;
  r.status = parseStatus(f.get());
  r.handleId = f.get();
  r.version = f.get();
  r.size = number(f.get());
  f.end();
  if (r.status == Status::Ok && (r.handleId.empty() || r.version != file.version || r.size != file.size))
    fail(Status::InvalidSourceResponse, "Open result does not match fixed metadata");
  if (r.status == Status::Ok) {
    const auto lease = m_impl->leases.find(leaseId);
    if (lease == m_impl->leases.end() || m_impl->handles.contains(r.handleId))
      fail(Status::InvalidSourceResponse, "Open result is not bound to a tracked lease");
    m_impl->handles.emplace(r.handleId, Impl::Handle{file, leaseId, lease->second.offerId});
  }
  return r;
}
ReadResult
FileRpcClient::read(const std::string &leaseId, const std::string &handleId, std::uint64_t offset, std::uint32_t length)
{
  auto lock = m_impl->operationLock();
  PoisonOnException guard{m_impl->fd.value};
  auto message =
      m_impl->call({"read", m_impl->sessionEpoch, leaseId, handleId, std::to_string(offset), std::to_string(length)});
  Fields f{message.fields, 2};
  ReadResult r;
  r.status = parseStatus(f.get());
  r.sessionEpoch = f.get();
  r.offerId = f.get();
  r.fileId = f.get();
  r.version = f.get();
  r.leaseId = f.get();
  r.handleId = f.get();
  r.requestId = f.get();
  r.offset = number(f.get());
  r.length = number32(f.get());
  const auto eof = f.get();
  f.end();
  if (eof != "0" && eof != "1")
    fail(Status::InvalidSourceResponse, "Invalid EOF");
  r.eof = eof == "1";
  r.bytes = std::move(message.bytes);
  if (r.sessionEpoch != m_impl->sessionEpoch || r.leaseId != leaseId || r.handleId != handleId ||
      r.requestId != message.fields[1] || r.offset != offset || r.length != length || r.bytes.size() > length ||
      (r.status != Status::Ok && (!r.bytes.empty() || r.eof)))
    fail(Status::InvalidSourceResponse, "Read response does not match request");
  if (r.status == Status::Ok) {
    const auto handle = m_impl->handles.find(handleId);
    if (handle == m_impl->handles.end() || handle->second.leaseId != leaseId)
      fail(Status::InvalidSourceResponse, "Unknown successful handle");
    const auto &known = handle->second;
    const auto remaining = offset >= known.file.size ? 0 : known.file.size - offset;
    if (length == 0 || length > 262144 || r.offerId != known.offerId || r.fileId != known.file.fileId ||
        r.version != known.file.version || r.bytes.size() > std::min<std::uint64_t>(length, remaining) ||
        r.eof != (r.bytes.size() == remaining) || (remaining != 0 && r.bytes.empty()))
      fail(Status::InvalidSourceResponse, "Read version, length or EOF is inconsistent");
  }
  return r;
}
Status FileRpcClient::close(const std::string &handleId)
{
  auto lock = m_impl->operationLock();
  PoisonOnException guard{m_impl->fd.value};
  auto message = m_impl->call({"close", m_impl->sessionEpoch, handleId});
  if (message.fields.size() != 3)
    fail(Status::InvalidSourceResponse, "Invalid close result");
  const auto status = parseStatus(message.fields[2]);
  if (status == Status::Ok)
    m_impl->handles.erase(handleId);
  return status;
}
void FileRpcClient::cancel()
{
  m_impl->cancelled.store(true);
  ::shutdown(m_impl->fd.value, SHUT_RDWR);
}
} // namespace deskflow::clipboard
