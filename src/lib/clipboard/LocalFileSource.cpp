/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "LocalFileSource.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <climits>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

namespace deskflow::clipboard {
namespace {

class Fd
{
public:
  explicit Fd(int value = -1) : m_value(value)
  {
  }
  ~Fd()
  {
    if (m_value >= 0)
      ::close(m_value);
  }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  Fd(Fd &&other) noexcept : m_value(other.m_value)
  {
    other.m_value = -1;
  }
  Fd &operator=(Fd &&other) noexcept
  {
    if (this != &other) {
      if (m_value >= 0)
        ::close(m_value);
      m_value = other.m_value;
      other.m_value = -1;
    }
    return *this;
  }
  int get() const
  {
    return m_value;
  }

private:
  int m_value;
};

Status systemStatus(int error)
{
  switch (error) {
  case ENOENT:
    return Status::SourceMissing;
  case EACCES:
  case EPERM:
    return Status::PermissionDenied;
  case ELOOP:
  case ENOTDIR:
    return Status::SourceChanged;
  case EMFILE:
  case ENFILE:
  case ENOMEM:
    return Status::ResourceLimit;
  default:
    return Status::IoError;
  }
}

bool sameIdentity(const struct stat &left, const struct stat &right)
{
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
         (left.st_mode & S_IFMT) == (right.st_mode & S_IFMT);
}

bool sameFile(const struct stat &left, const struct stat &right)
{
#if defined(__APPLE__)
  const bool timesEqual = left.st_mtimespec.tv_sec == right.st_mtimespec.tv_sec &&
                          left.st_mtimespec.tv_nsec == right.st_mtimespec.tv_nsec &&
                          left.st_ctimespec.tv_sec == right.st_ctimespec.tv_sec &&
                          left.st_ctimespec.tv_nsec == right.st_ctimespec.tv_nsec;
#else
  const bool timesEqual = left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
                          left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
                          left.st_ctim.tv_sec == right.st_ctim.tv_sec && left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
#endif
  return sameIdentity(left, right) && timesEqual && left.st_size == right.st_size && left.st_mode == right.st_mode &&
         left.st_uid == right.st_uid && left.st_gid == right.st_gid && left.st_nlink == right.st_nlink;
}

Status checkReadable(int fd, const struct stat &attributes)
{
  // Do not use an already-open descriptor to bypass a subsequent chmod(000).
  if ((attributes.st_mode & 0444) == 0 || (S_ISDIR(attributes.st_mode) && (attributes.st_mode & 0111) == 0))
    return Status::PermissionDenied;
#if defined(__linux__)
  if (::faccessat(fd, "", R_OK | (S_ISDIR(attributes.st_mode) ? X_OK : 0), AT_EACCESS | AT_EMPTY_PATH) != 0)
    return systemStatus(errno);
#else
  (void)fd;
#endif
  return Status::Ok;
}

std::string makeToken()
{
  static std::atomic<std::uint64_t> instances{1};
  std::random_device random;
  std::ostringstream out;
  out << "local-" << std::hex << random() << random() << '-' << instances.fetch_add(1);
  return out.str();
}

bool issuedId(const std::string &id, const std::string &prefix, std::uint64_t highest)
{
  if (!id.starts_with(prefix))
    return false;
  const auto suffix = std::string_view(id).substr(prefix.size());
  std::uint64_t ordinal = 0;
  const auto result = std::from_chars(suffix.data(), suffix.data() + suffix.size(), ordinal);
  return result.ec == std::errc{} && result.ptr == suffix.data() + suffix.size() && ordinal != 0 &&
         ordinal <= highest && std::to_string(ordinal) == suffix;
}

LeaseResult leaseFailure(Status status)
{
  return {status, {}, {}, 0, LeaseState::Closed};
}

} // namespace

struct LocalFileSource::Impl
{
  struct Root
  {
    Fd parent;
    std::string name;
  };
  struct Node
  {
    FileInfo info;
    std::size_t rootIndex{0};
    std::vector<std::string> components;
    struct stat original{};
    std::vector<std::string> children;
  };
  struct Offer
  {
    OfferInfo info;
    std::vector<Root> roots;
    std::unordered_map<std::string, Node> nodes;
    std::size_t manifestBytes{0};
    std::size_t activeLeases{0};
    Clock::time_point acquisitionUntil{};
  };
  struct Lease
  {
    std::string id;
    std::string offerId;
    std::string offerVersion;
    Clock::time_point expires;
    std::uint32_t ttl{0};
    LeaseState state{LeaseState::Active};
    std::size_t handles{0};
  };
  struct Handle
  {
    std::string leaseId;
    std::shared_ptr<Offer> offer;
    std::string fileId;
    Fd fd;
  };

  explicit Impl(Options value) : options(std::move(value)), token(makeToken())
  {
    if (options.sessionEpoch.empty() || !options.now || options.maxReadBytes == 0 || options.maxReadBytes > 262144 ||
        options.maxResponseBytes == 0 || options.maxResponseBytes > options.maxReadBytes || options.defaultTtlMs == 0 ||
        options.maxTtlMs < options.defaultTtlMs)
      throw std::invalid_argument("Invalid LocalFileSource options");
  }

  Options options;
  std::string token;
  mutable std::mutex mutex;
  bool connected{true};
  std::string current;
  std::uint64_t offerSerial{0};
  std::uint64_t fileSerial{0};
  std::uint64_t leaseSerial{0};
  std::uint64_t handleSerial{0};
  std::unordered_map<std::string, std::shared_ptr<Offer>> offers;
  std::unordered_map<std::string, Lease> leases;
  std::unordered_map<std::string, Handle> handles;

  Status session(const std::string &epoch) const
  {
    if (epoch != options.sessionEpoch)
      return Status::StaleSession;
    return connected ? Status::Ok : Status::Disconnected;
  }

  std::uint32_t ttl(std::uint32_t requested) const
  {
    return requested == 0 ? options.defaultTtlMs : std::min(requested, options.maxTtlMs);
  }

  void finishLease(Lease &lease, LeaseState state)
  {
    if (lease.state == LeaseState::Active || lease.state == LeaseState::Draining) {
      const auto offer = offers.find(lease.offerId);
      if (offer != offers.end() && offer->second->activeLeases != 0)
        --offer->second->activeLeases;
    }
    lease.state = state;
  }

  void closeLeaseHandles(Lease &lease)
  {
    for (auto iterator = handles.begin(); iterator != handles.end();) {
      if (iterator->second.leaseId == lease.id)
        iterator = handles.erase(iterator);
      else
        ++iterator;
    }
    lease.handles = 0;
  }

  void collect()
  {
    const auto now = options.now();
    for (auto &[id, lease] : leases) {
      (void)id;
      if ((lease.state == LeaseState::Active || lease.state == LeaseState::Draining) && now >= lease.expires) {
        finishLease(lease, LeaseState::Expired);
        closeLeaseHandles(lease);
      }
    }
    for (auto iterator = offers.begin(); iterator != offers.end();) {
      const auto &offer = *iterator->second;
      if (iterator->first != current && offer.activeLeases == 0 && now >= offer.acquisitionUntil)
        iterator = offers.erase(iterator);
      else
        ++iterator;
    }
  }

  Status validLease(const std::string &id, bool allowDraining, Lease *&lease)
  {
    const auto found = leases.find(id);
    if (found == leases.end())
      return Status::NotFound;
    lease = &found->second;
    switch (lease->state) {
    case LeaseState::Active:
      return Status::Ok;
    case LeaseState::Draining:
      return allowDraining ? Status::Ok : Status::LeaseReleased;
    case LeaseState::Closed:
      return Status::LeaseReleased;
    case LeaseState::Expired:
      return Status::LeaseExpired;
    case LeaseState::Revoked:
      return Status::LeaseRevoked;
    }
    return Status::LeaseRevoked;
  }

  LeaseResult leaseResult(const Lease &lease, Status status = Status::Ok) const
  {
    return {status, lease.id, lease.offerVersion, lease.ttl, lease.state};
  }

  // Every root ancestor and every descendant is opened without following links.
  Status openRootParent(const std::filesystem::path &input, Root &root)
  {
    if (input.empty() || input.native().find('\0') != std::string::npos)
      return Status::InvalidArgument;
    for (const auto &part : input)
      if (part == "..")
        return Status::InvalidArgument;
    std::error_code error;
    auto path = std::filesystem::absolute(input, error).lexically_normal();
    if (error)
      return Status::InvalidArgument;
    while (path != path.root_path() && path.filename().empty())
      path = path.parent_path();
    if (path == path.root_path() || path.filename().empty())
      return Status::InvalidArgument;
    Fd parent(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (parent.get() < 0)
      return systemStatus(errno);
    for (const auto &component : path.parent_path().relative_path()) {
      const std::string name = component.string();
      Fd next(::openat(parent.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (next.get() < 0)
        return systemStatus(errno);
      parent = std::move(next);
    }
    root.parent = std::move(parent);
    root.name = path.filename().string();
    return root.name.find('\0') == std::string::npos ? Status::Ok : Status::InvalidArgument;
  }

  Status addNode(
      Offer &offer, int parentFd, const std::string &name, const std::string &parentId, const std::string &relativePath,
      std::size_t rootIndex, std::vector<std::string> components, std::size_t depth, std::string &newId
  )
  {
    if (depth > options.maxDepth || offer.nodes.size() >= options.maxEntriesPerOffer)
      return Status::ResourceLimit;
    struct stat before{};
    if (::fstatat(parentFd, name.c_str(), &before, AT_SYMLINK_NOFOLLOW) != 0)
      return systemStatus(errno);
    if (!S_ISREG(before.st_mode) && !S_ISDIR(before.st_mode))
      return Status::InvalidArgument;
    const int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (S_ISDIR(before.st_mode) ? O_DIRECTORY : O_NONBLOCK);
    Fd fd(::openat(parentFd, name.c_str(), flags));
    if (fd.get() < 0)
      return systemStatus(errno);
    struct stat attributes{};
    if (::fstat(fd.get(), &attributes) != 0)
      return systemStatus(errno);
    if (!sameIdentity(before, attributes))
      return Status::SourceChanged;
    auto status = checkReadable(fd.get(), attributes);
    if (status != Status::Ok)
      return status;
    if (attributes.st_size < 0)
      return Status::UnsupportedSize;
    std::size_t componentBytes = sizeof(std::string) + name.size();
    for (const auto &component : components)
      componentBytes += sizeof(std::string) + component.size();
    // Include both the public manifest copy and internal node plus map/vector
    // overhead. Independent entry/depth/root limits additionally bound storage.
    const auto identifierBytes = token.size() + 32;
    const std::size_t bytes = sizeof(Node) + sizeof(FileInfo) + 128 + componentBytes +
                              2 * (relativePath.size() + name.size() + parentId.size() + 2 * identifierBytes);
    if (bytes > options.maxManifestBytes || offer.manifestBytes > options.maxManifestBytes - bytes)
      return Status::ResourceLimit;
    offer.manifestBytes += bytes;
    newId = token + ":f:" + std::to_string(++fileSerial);
    FileInfo info{
        newId,
        parentId,
        name,
        relativePath,
        newId + ":v1",
        S_ISDIR(attributes.st_mode) ? FileKind::Directory : FileKind::File,
        S_ISDIR(attributes.st_mode) ? 0 : static_cast<std::uint64_t>(attributes.st_size)
    };
    components.push_back(name);
    offer.nodes.emplace(newId, Node{info, rootIndex, components, attributes, {}});
    offer.info.files.push_back(info);
    if (!S_ISDIR(attributes.st_mode))
      return Status::Ok;

    const int duplicate = ::dup(fd.get());
    if (duplicate < 0)
      return systemStatus(errno);
    DIR *directory = ::fdopendir(duplicate);
    if (!directory) {
      ::close(duplicate);
      return systemStatus(errno);
    }
    std::vector<std::string> names;
    errno = 0;
    while (const auto *entry = ::readdir(directory)) {
      const std::string child = entry->d_name;
      if (child != "." && child != "..") {
        if (names.size() >= options.maxEntriesPerOffer) {
          ::closedir(directory);
          return Status::ResourceLimit;
        }
        names.push_back(child);
      }
      errno = 0;
    }
    const int directoryError = errno;
    ::closedir(directory);
    if (directoryError != 0)
      return systemStatus(directoryError);
    std::sort(names.begin(), names.end());
    const auto directoryId = newId;
    for (const auto &child : names) {
      std::string childId;
      status = addNode(
          offer, fd.get(), child, directoryId, relativePath + '/' + child, rootIndex, components, depth + 1, childId
      );
      if (status != Status::Ok)
        return status;
      offer.nodes.at(directoryId).children.push_back(childId);
    }
    struct stat after{};
    if (::fstat(fd.get(), &after) != 0)
      return systemStatus(errno);
    // Publication itself must not accidentally combine changing directory lists.
    return sameFile(attributes, after) ? Status::Ok : Status::SourceChanged;
  }

  Status openNode(const Offer &offer, const Node &node, Fd &result)
  {
    int parent = offer.roots[node.rootIndex].parent.get();
    Fd walking;
    for (std::size_t index = 0; index < node.components.size(); ++index) {
      const bool final = index + 1 == node.components.size();
      const int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
                        ((!final || node.info.kind == FileKind::Directory) ? O_DIRECTORY : O_NONBLOCK);
      Fd next(::openat(parent, node.components[index].c_str(), flags));
      if (next.get() < 0)
        return systemStatus(errno);
      walking = std::move(next);
      parent = walking.get();
    }
    struct stat currentAttributes{};
    if (::fstat(walking.get(), &currentAttributes) != 0)
      return systemStatus(errno);
    if (!sameIdentity(node.original, currentAttributes))
      return Status::SourceChanged;
    const auto readable = checkReadable(walking.get(), currentAttributes);
    if (readable != Status::Ok)
      return readable;
    // Directory membership is the saved manifest, even if unrelated entries change.
    if (node.info.kind == FileKind::File && !sameFile(node.original, currentAttributes))
      return Status::SourceChanged;
    result = std::move(walking);
    return Status::Ok;
  }

  Status boundNode(
      const std::string &leaseId, const std::string &fileId, const std::string &version, Lease *&lease,
      std::shared_ptr<Offer> &offer, Node *&node
  )
  {
    auto status = validLease(leaseId, false, lease);
    if (status != Status::Ok)
      return status;
    const auto foundOffer = offers.find(lease->offerId);
    if (foundOffer == offers.end())
      return Status::OfferExpired;
    offer = foundOffer->second;
    const auto found = offer->nodes.find(fileId);
    if (found == offer->nodes.end())
      return Status::NotFound;
    node = &found->second;
    return node->info.version == version ? Status::Ok : Status::VersionMismatch;
  }
};

LocalFileSource::LocalFileSource(Options options) : m_impl(std::make_unique<Impl>(std::move(options)))
{
}
LocalFileSource::~LocalFileSource() = default;

OfferResult LocalFileSource::createOffer(const std::vector<std::filesystem::path> &paths)
{
  auto &self = *m_impl;
  std::lock_guard lock(self.mutex);
  self.collect();
  if (!self.connected)
    return {Status::Disconnected, {}};
  if (paths.empty())
    return {Status::InvalidArgument, {}};
  if (paths.size() > self.options.maxRootsPerOffer || self.offers.size() >= self.options.maxOffers)
    return {Status::ResourceLimit, {}};
  auto offer = std::make_shared<Impl::Offer>();
  offer->info.sessionEpoch = self.options.sessionEpoch;
  offer->info.acquisitionWindowMs = self.options.acquisitionWindowMs;
  std::set<std::string> rootNames;
  for (const auto &path : paths) {
    Impl::Root root;
    auto status = self.openRootParent(path, root);
    if (status != Status::Ok)
      return {status, {}};
    if (!rootNames.insert(root.name).second)
      return {Status::InvalidArgument, {}};
    offer->roots.push_back(std::move(root));
    auto &stored = offer->roots.back();
    std::string rootId;
    status =
        self.addNode(*offer, stored.parent.get(), stored.name, {}, stored.name, offer->roots.size() - 1, {}, 0, rootId);
    if (status != Status::Ok)
      return {status, {}};
    offer->info.roots.push_back(rootId);
  }
  offer->info.offerId = self.token + ":o:" + std::to_string(++self.offerSerial);
  offer->info.offerVersion = offer->info.offerId + ":manifest-v1";
  const auto old = self.offers.find(self.current);
  if (old != self.offers.end())
    old->second->acquisitionUntil = self.options.now() + std::chrono::milliseconds(self.options.acquisitionWindowMs);
  self.current = offer->info.offerId;
  self.offers.emplace(self.current, offer);
  return {Status::Ok, offer->info};
}

LeaseResult LocalFileSource::acquire(
    const std::string &epoch, const std::string &offerId, const std::string &version, std::uint32_t requestedTtlMs
)
{
  auto &self = *m_impl;
  std::lock_guard lock(self.mutex);
  self.collect();
  auto status = self.session(epoch);
  if (status != Status::Ok)
    return leaseFailure(status);
  const auto found = self.offers.find(offerId);
  if (found == self.offers.end())
    return leaseFailure(
        issuedId(offerId, self.token + ":o:", self.offerSerial) ? Status::OfferExpired : Status::NotFound
    );
  auto &offer = *found->second;
  if (offerId != self.current && self.options.now() >= offer.acquisitionUntil)
    return leaseFailure(Status::OfferExpired);
  if (offer.info.offerVersion != version)
    return leaseFailure(Status::VersionMismatch);
  if (self.leases.size() >= self.options.maxLeaseRecords)
    return leaseFailure(Status::ResourceLimit);
  Impl::Lease lease;
  lease.id = self.token + ":l:" + std::to_string(++self.leaseSerial);
  lease.offerId = offerId;
  lease.offerVersion = version;
  lease.ttl = self.ttl(requestedTtlMs);
  lease.expires = self.options.now() + std::chrono::milliseconds(lease.ttl);
  ++offer.activeLeases;
  const auto result = self.leaseResult(lease);
  self.leases.emplace(lease.id, std::move(lease));
  return result;
}

LeaseResult LocalFileSource::renew(const std::string &epoch, const std::string &id, std::uint32_t requestedTtlMs)
{
  auto &self = *m_impl;
  std::lock_guard lock(self.mutex);
  self.collect();
  auto status = self.session(epoch);
  if (status != Status::Ok)
    return leaseFailure(status);
  Impl::Lease *lease = nullptr;
  status = self.validLease(id, true, lease);
  if (status != Status::Ok)
    return lease ? self.leaseResult(*lease, status) : leaseFailure(status);
  lease->ttl = self.ttl(requestedTtlMs);
  lease->expires = self.options.now() + std::chrono::milliseconds(lease->ttl);
  return self.leaseResult(*lease);
}

LeaseResult LocalFileSource::release(const std::string &epoch, const std::string &id)
{
  auto &self = *m_impl;
  std::lock_guard lock(self.mutex);
  self.collect();
  auto status = self.session(epoch);
  if (status != Status::Ok)
    return leaseFailure(status);
  const auto found = self.leases.find(id);
  if (found == self.leases.end())
    return leaseFailure(Status::NotFound);
  auto &lease = found->second;
  if (lease.state == LeaseState::Active) {
    if (lease.handles == 0)
      self.finishLease(lease, LeaseState::Closed);
    else
      lease.state = LeaseState::Draining;
  }
  return self.leaseResult(lease);
}

LeaseResult LocalFileSource::revoke(const std::string &epoch, const std::string &id)
{
  auto &self = *m_impl;
  std::lock_guard lock(self.mutex);
  self.collect();
  auto status = self.session(epoch);
  if (status != Status::Ok)
    return leaseFailure(status);
  const auto found = self.leases.find(id);
  if (found == self.leases.end())
    return leaseFailure(Status::NotFound);
  auto &lease = found->second;
  if (lease.state == LeaseState::Active || lease.state == LeaseState::Draining) {
    self.finishLease(lease, LeaseState::Revoked);
    self.closeLeaseHandles(lease);
  }
  return self.leaseResult(lease);
}

StatResult LocalFileSource::stat(
    const std::string &epoch, const std::string &leaseId, const std::string &id, const std::string &version
)
{
  auto &self = *m_impl;
  std::lock_guard lock(self.mutex);
  self.collect();
  auto status = self.session(epoch);
  if (status != Status::Ok)
    return {status, {}};
  Impl::Lease *lease = nullptr;
  std::shared_ptr<Impl::Offer> offer;
  Impl::Node *node = nullptr;
  status = self.boundNode(leaseId, id, version, lease, offer, node);
  if (status != Status::Ok)
    return {status, {}};
  Fd fd;
  status = self.openNode(*offer, *node, fd);
  return status == Status::Ok ? StatResult{status, node->info} : StatResult{status, {}};
}

ListResult LocalFileSource::list(
    const std::string &epoch, const std::string &leaseId, const std::string &id, const std::string &version
)
{
  auto &self = *m_impl;
  std::lock_guard lock(self.mutex);
  self.collect();
  auto status = self.session(epoch);
  if (status != Status::Ok)
    return {status, {}};
  Impl::Lease *lease = nullptr;
  std::shared_ptr<Impl::Offer> offer;
  Impl::Node *node = nullptr;
  status = self.boundNode(leaseId, id, version, lease, offer, node);
  if (status != Status::Ok)
    return {status, {}};
  if (node->info.kind != FileKind::Directory)
    return {Status::InvalidArgument, {}};
  Fd fd;
  status = self.openNode(*offer, *node, fd);
  if (status != Status::Ok)
    return {status, {}};
  ListResult result{Status::Ok, {}};
  for (const auto &child : node->children)
    result.entries.push_back(offer->nodes.at(child).info);
  return result;
}

OpenResult LocalFileSource::open(
    const std::string &epoch, const std::string &leaseId, const std::string &id, const std::string &version,
    ConsistencyMode consistency
)
{
  auto &self = *m_impl;
  std::lock_guard lock(self.mutex);
  self.collect();
  auto status = self.session(epoch);
  if (status != Status::Ok)
    return {status, {}, {}, 0};
  Impl::Lease *lease = nullptr;
  std::shared_ptr<Impl::Offer> offer;
  Impl::Node *node = nullptr;
  status = self.boundNode(leaseId, id, version, lease, offer, node);
  if (status != Status::Ok)
    return {status, {}, {}, 0};
  if (node->info.kind != FileKind::File)
    return {Status::InvalidArgument, {}, {}, 0};
  if (consistency != ConsistencyMode::Immutable && consistency != ConsistencyMode::Detectable)
    return {Status::InvalidArgument, {}, {}, 0};
  if (consistency != ConsistencyMode::Detectable)
    return {Status::SourceNotStable, {}, {}, 0};
  if (self.handles.size() >= self.options.maxOpenHandles)
    return {Status::ResourceLimit, {}, {}, 0};
  Fd fd;
  status = self.openNode(*offer, *node, fd);
  if (status != Status::Ok)
    return {status, {}, {}, 0};
  const auto handleId = self.token + ":h:" + std::to_string(++self.handleSerial);
  self.handles.emplace(handleId, Impl::Handle{leaseId, offer, id, std::move(fd)});
  ++lease->handles;
  return {Status::Ok, handleId, node->info.version, node->info.size};
}

ReadResult LocalFileSource::read(const ReadRequest &request)
{
  auto &self = *m_impl;
  std::lock_guard lock(self.mutex);
  self.collect();
  ReadResult result;
  result.sessionEpoch = request.sessionEpoch;
  result.leaseId = request.leaseId;
  result.handleId = request.handleId;
  result.requestId = request.requestId;
  result.offset = request.offset;
  result.length = request.length;
  result.status = self.session(request.sessionEpoch);
  if (result.status != Status::Ok)
    return result;
  Impl::Lease *lease = nullptr;
  result.status = self.validLease(request.leaseId, true, lease);
  if (lease)
    result.offerId = lease->offerId;
  if (result.status != Status::Ok)
    return result;
  const auto found = self.handles.find(request.handleId);
  if (found == self.handles.end()) {
    result.status =
        issuedId(request.handleId, self.token + ":h:", self.handleSerial) ? Status::HandleClosed : Status::NotFound;
    return result;
  }
  auto &handle = found->second;
  if (handle.leaseId != request.leaseId) {
    result.status = Status::NotFound;
    return result;
  }
  const auto &node = handle.offer->nodes.at(handle.fileId);
  result.fileId = node.info.fileId;
  result.version = node.info.version;
  if (request.requestId.empty() || request.length == 0 || request.length > self.options.maxReadBytes) {
    result.status = Status::InvalidArgument;
    return result;
  }
  Fd pathCheck;
  result.status = self.openNode(*handle.offer, node, pathCheck);
  if (result.status != Status::Ok)
    return result;
  struct stat before{};
  if (::fstat(handle.fd.get(), &before) != 0) {
    result.status = systemStatus(errno);
    return result;
  }
  result.status = checkReadable(handle.fd.get(), before);
  if (result.status != Status::Ok)
    return result;
  if (!sameFile(node.original, before)) {
    result.status = Status::SourceChanged;
    return result;
  }
  if (request.offset >= node.info.size) {
    result.status = Status::Ok;
    result.eof = true;
    return result;
  }
  const auto wanted = static_cast<std::size_t>(
      std::min<std::uint64_t>({request.length, self.options.maxResponseBytes, node.info.size - request.offset})
  );
  result.bytes.resize(wanted);
  ssize_t count;
  do {
    count = ::pread(handle.fd.get(), result.bytes.data(), wanted, static_cast<off_t>(request.offset));
  } while (count < 0 && errno == EINTR);
  if (count < 0) {
    result.status = systemStatus(errno);
    result.bytes.clear();
    return result;
  }
  struct stat after{};
  if (::fstat(handle.fd.get(), &after) != 0)
    result.status = systemStatus(errno);
  else if (checkReadable(handle.fd.get(), after) != Status::Ok)
    result.status = Status::PermissionDenied;
  else if (!sameFile(node.original, after))
    result.status = Status::SourceChanged;
  else
    result.status = self.openNode(*handle.offer, node, pathCheck);
  if (result.status != Status::Ok) {
    result.bytes.clear();
    return result;
  }
  if (count == 0) {
    result.status = Status::InvalidSourceResponse;
    result.bytes.clear();
    return result;
  }
  result.bytes.resize(static_cast<std::size_t>(count));
  result.eof = result.bytes.size() == node.info.size - request.offset;
  return result;
}

Status LocalFileSource::close(const std::string &epoch, const std::string &id)
{
  auto &self = *m_impl;
  std::lock_guard lock(self.mutex);
  self.collect();
  auto status = self.session(epoch);
  if (status != Status::Ok)
    return status;
  const auto found = self.handles.find(id);
  if (found == self.handles.end())
    return issuedId(id, self.token + ":h:", self.handleSerial) ? Status::Ok : Status::NotFound;
  auto &lease = self.leases.at(found->second.leaseId);
  if (lease.handles != 0)
    --lease.handles;
  self.handles.erase(found);
  if (lease.handles == 0 && lease.state == LeaseState::Draining)
    self.finishLease(lease, LeaseState::Closed);
  return Status::Ok;
}

void LocalFileSource::disconnect()
{
  auto &self = *m_impl;
  std::lock_guard lock(self.mutex);
  self.connected = false;
  self.handles.clear();
  for (auto &[id, lease] : self.leases) {
    (void)id;
    if (lease.state == LeaseState::Active || lease.state == LeaseState::Draining)
      lease.state = LeaseState::Revoked;
    lease.handles = 0;
  }
  self.offers.clear();
  self.current.clear();
}

void LocalFileSource::collectExpired()
{
  std::lock_guard lock(m_impl->mutex);
  m_impl->collect();
}

std::string LocalFileSource::currentOfferId() const
{
  std::lock_guard lock(m_impl->mutex);
  return m_impl->current;
}

} // namespace deskflow::clipboard
