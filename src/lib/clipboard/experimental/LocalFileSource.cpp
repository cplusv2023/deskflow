/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "LocalFileSource.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace deskflow::clipboard::experimental {
namespace {

Status fromErrno(int error)
{
  switch (error) {
  case ENOENT:
    return Status::SourceMissing;
  case EACCES:
  case EPERM:
    return Status::PermissionDenied;
  case EMFILE:
  case ENFILE:
  case ENOMEM:
    return Status::ResourceLimit;
  case ELOOP:
  case ENOTDIR:
    return Status::InvalidArgument;
  default:
    return Status::IoError;
  }
}

bool unchanged(const struct stat &before, const struct stat &after)
{
#if defined(__APPLE__)
  const auto beforeMtime = before.st_mtimespec;
  const auto afterMtime = after.st_mtimespec;
  const auto beforeCtime = before.st_ctimespec;
  const auto afterCtime = after.st_ctimespec;
#else
  const auto beforeMtime = before.st_mtim;
  const auto afterMtime = after.st_mtim;
  const auto beforeCtime = before.st_ctim;
  const auto afterCtime = after.st_ctim;
#endif
  return before.st_dev == after.st_dev && before.st_ino == after.st_ino && before.st_size == after.st_size &&
         before.st_mode == after.st_mode && beforeMtime.tv_sec == afterMtime.tv_sec &&
         beforeMtime.tv_nsec == afterMtime.tv_nsec && beforeCtime.tv_sec == afterCtime.tv_sec &&
         beforeCtime.tv_nsec == afterCtime.tv_nsec;
}

class OwnedFd
{
public:
  explicit OwnedFd(int fd) : fd(fd)
  {
  }
  ~OwnedFd()
  {
    if (fd >= 0)
      ::close(fd);
  }
  int fd;
};

Status contextStatus(const RequestContext &context)
{
  if (context.stop.stop_requested())
    return Status::Cancelled;
  if (Clock::now() >= context.deadline)
    return Status::Timeout;
  return Status::Ok;
}

} // namespace

struct LocalFileSource::File
{
  OwnedFd fd;
  struct stat snapshot;
  std::string version;
  File(int fd, const struct stat &snapshot, std::string version)
      : fd(fd),
        snapshot(snapshot),
        version(std::move(version))
  {
  }
};

LocalFileSource::~LocalFileSource() = default;

LocalCapture LocalFileSource::capture(
    const std::vector<std::filesystem::path> &paths, std::string offerId, std::string offerVersion,
    std::uint32_t maxEntries
)
{
  if (paths.empty() || offerId.empty() || offerVersion.empty() || maxEntries < 2)
    return {.status = Status::InvalidArgument};
  auto source = std::shared_ptr<LocalFileSource>(new LocalFileSource);
  Offer offer{std::move(offerId), std::move(offerVersion), {}, source};
  offer.entries.push_back({"root", "", "", offer.version, true, 0});
  std::unordered_set<std::string> names;
  for (auto path : paths) {
    if (path.filename().empty())
      path = path.parent_path();
    const auto name = path.filename().string();
    if (name.empty() || name == "." || name == ".." || !names.insert(name).second)
      return {.status = Status::InvalidArgument};
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    OwnedFd parentFd(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (parentFd.fd < 0)
      return {.status = fromErrno(errno)};
    if (auto status = source->captureObject(parentFd.fd, name, "root", offer, maxEntries, 0); status != Status::Ok)
      return {.status = status};
  }
  return {Status::Ok, std::move(offer)};
}

Status LocalFileSource::captureObject(
    int parentFd, const std::string &name, const std::string &parentId, Offer &offer, std::uint32_t maxEntries,
    unsigned int depth
)
{
  if (offer.entries.size() >= maxEntries || depth > 128)
    return Status::ResourceLimit;
  // O_NONBLOCK prevents a FIFO/device selected during a race from hanging capture.
  OwnedFd fd(::openat(parentFd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (fd.fd < 0)
    return fromErrno(errno);
  struct stat info{};
  if (::fstat(fd.fd, &info) != 0)
    return fromErrno(errno);
  if (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode))
    return Status::InvalidArgument;
  if (info.st_size < 0 ||
      static_cast<std::uint64_t>(info.st_size) > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return Status::UnsupportedSize;
  const auto id = "file-" + std::to_string(offer.entries.size());
  const auto version = offer.version + ":" + id;
  const bool directory = S_ISDIR(info.st_mode);
  offer.entries.push_back(
      {id, parentId, name, version, directory, directory ? 0 : static_cast<std::uint64_t>(info.st_size)}
  );
  if (!directory) {
    m_files.emplace(id, std::make_unique<File>(fd.fd, info, version));
    fd.fd = -1;
    return Status::Ok;
  }
  const int duplicate = ::dup(fd.fd);
  if (duplicate < 0)
    return fromErrno(errno);
  DIR *raw = ::fdopendir(duplicate);
  if (!raw) {
    const int error = errno;
    ::close(duplicate);
    return fromErrno(error);
  }
  const auto deleter = [](DIR *dir) { ::closedir(dir); };
  std::unique_ptr<DIR, decltype(deleter)> dir(raw, deleter);
  std::vector<std::string> names;
  errno = 0;
  while (auto *entry = ::readdir(dir.get())) {
    const std::string child = entry->d_name;
    if (child != "." && child != "..") {
      if (names.size() >= maxEntries - offer.entries.size())
        return Status::ResourceLimit;
      names.push_back(child);
    }
    errno = 0;
  }
  if (errno)
    return fromErrno(errno);
  std::sort(names.begin(), names.end());
  for (const auto &child : names) {
    if (auto status = captureObject(fd.fd, child, id, offer, maxEntries, depth + 1); status != Status::Ok)
      return status;
  }
  struct stat after{};
  if (::fstat(fd.fd, &after) != 0)
    return fromErrno(errno);
  return unchanged(info, after) ? Status::Ok : Status::SourceChanged;
}

bool LocalFileSource::immutable() const
{
  return false;
}

Status LocalFileSource::validate(const Entry &entry)
{
  const auto found = m_files.find(entry.id);
  if (found == m_files.end())
    return Status::NotFound;
  if (entry.version != found->second->version ||
      entry.size != static_cast<std::uint64_t>(found->second->snapshot.st_size))
    return Status::VersionMismatch;
  struct stat current{};
  if (::fstat(found->second->fd.fd, &current) != 0)
    return fromErrno(errno);
  return unchanged(found->second->snapshot, current) ? Status::Ok : Status::SourceChanged;
}

SourceRead
LocalFileSource::read(const Entry &entry, std::uint64_t offset, std::uint32_t length, const RequestContext &context)
{
  if (!length)
    return {.status = Status::InvalidArgument};
  if (auto status = contextStatus(context); status != Status::Ok)
    return {.status = status};
  if (auto status = validate(entry); status != Status::Ok)
    return {.status = status};
  if (offset >= entry.size)
    return {.eof = true};
  SourceRead result;
  result.bytes.resize(static_cast<std::size_t>(std::min<std::uint64_t>(length, entry.size - offset)));
  ssize_t count;
  do {
    if (auto status = contextStatus(context); status != Status::Ok)
      return {.status = status};
    count = ::pread(m_files.at(entry.id)->fd.fd, result.bytes.data(), result.bytes.size(), static_cast<off_t>(offset));
  } while (count < 0 && errno == EINTR);
  if (count < 0)
    return {.status = fromErrno(errno)};
  if (auto status = contextStatus(context); status != Status::Ok)
    return {.status = status};
  if (auto status = validate(entry); status != Status::Ok)
    return {.status = status};
  if (count == 0)
    return {.status = Status::InvalidSourceResponse};
  result.bytes.resize(static_cast<std::size_t>(count));
  result.eof = result.bytes.size() == entry.size - offset;
  return result;
}

} // namespace deskflow::clipboard::experimental
