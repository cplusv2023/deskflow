/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#define FUSE_USE_VERSION 35
#include <fuse.h>

#include "FuseMount.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <sys/statvfs.h>
#include <unistd.h>

namespace clipboard_proto {
namespace {

FuseMount &mount()
{
  return *static_cast<FuseMount *>(fuse_get_context()->private_data);
}

void fillStat(const FileEntry &entry, struct stat *stat)
{
  std::memset(stat, 0, sizeof(*stat));
  const auto &entries = mount().source.entries();
  stat->st_ino = static_cast<ino_t>(&entry - entries.data() + 1);
  stat->st_mode = (entry.directory ? S_IFDIR | 0755 : S_IFREG | 0644);
  stat->st_nlink = entry.directory ? 2 : 1;
  stat->st_uid = getuid();
  stat->st_gid = getgid();
  stat->st_size = static_cast<off_t>(entry.size);
  stat->st_blksize = 4096;
  stat->st_blocks = static_cast<blkcnt_t>(entry.size / 512 + (entry.size % 512 != 0));
  stat->st_atime = stat->st_mtime = stat->st_ctime = 1767225600; // 2026-01-01 UTC.
}

int getAttributes(const char *path, struct stat *stat, struct fuse_file_info *)
{
  const auto *entry = mount().source.findPath(path);
  mount().source.metadata("getattr", path, entry);
  if (!entry)
    return -ENOENT;
  fillStat(*entry, stat);
  return 0;
}

int readDirectory(
    const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *,
    enum fuse_readdir_flags flags
)
{
  const auto *entry = mount().source.findPath(path);
  mount().source.metadata("readdir", path, entry);
  if (!entry)
    return -ENOENT;
  if (!entry->directory)
    return -ENOTDIR;
  if (offset < 0)
    return -EINVAL;
  const auto children = mount().source.children(path);
  const auto total = children.size() + 2;
  for (auto index = static_cast<std::size_t>(offset); index < total; ++index) {
    std::string name;
    const FileEntry *child;
    if (index == 0) {
      name = ".";
      child = entry;
    } else if (index == 1) {
      name = "..";
      const auto split = entry->path.find_last_of('/');
      child = mount().source.findPath(split == 0 ? "/" : entry->path.substr(0, split));
    } else {
      child = children[index - 2];
      name = child->path.substr(child->path.find_last_of('/') + 1);
    }
    struct stat attributes{};
    fillStat(*child, &attributes);
    const auto fillFlags =
        (flags & FUSE_READDIR_PLUS) != 0 ? FUSE_FILL_DIR_PLUS : static_cast<enum fuse_fill_dir_flags>(0);
    if (filler(buffer, name.c_str(), &attributes, static_cast<off_t>(index + 1), fillFlags) != 0)
      break;
  }
  return 0;
}

int openFile(const char *path, struct fuse_file_info *info)
{
  if ((info->flags & O_ACCMODE) != O_RDONLY || (info->flags & (O_TRUNC | O_APPEND | O_CREAT)) != 0)
    return -EROFS;
  const auto *entry = mount().source.findPath(path);
  mount().source.metadata("open", path, entry);
  if (!entry)
    return -ENOENT;
  if (entry->directory)
    return -EISDIR;
  info->fh = mount().open(*entry);
  // Avoid opaque page-cache prefetch during this observability-first prototype.
  info->direct_io = 1;
  info->keep_cache = 0;
  return 0;
}

int readFile(const char *, char *buffer, std::size_t size, off_t offset, struct fuse_file_info *info)
{
  if (offset < 0 || size > static_cast<std::size_t>(INT_MAX))
    return -EINVAL;
  const auto *entry = mount().handle(info->fh);
  if (!entry)
    return -EBADF;
  const auto start = static_cast<std::uint64_t>(offset);
  const auto wanted = start >= entry->size
                          ? std::size_t{0}
                          : static_cast<std::size_t>(std::min<std::uint64_t>(size, entry->size - start));
  std::size_t completed = 0;
  try {
    while (completed < wanted) {
      const auto chunk = std::min(wanted - completed, FakeRemoteSource::maxRequestBytes);
      const auto read = mount().source.readRange(*entry, start + completed, {buffer + completed, chunk}, info->fh);
      // A zero response before known EOF is a protocol failure, never success.
      if (read == 0)
        return -EIO;
      completed += read;
    }
  } catch (const std::exception &error) {
    mount().log.event("read_error", "\"message\":" + jsonQuote(error.what()));
    return -EIO;
  }
  mount().log.event(
      "fuse_read", "\"fileId\":" + jsonQuote(entry->id) + ",\"handleId\":" + std::to_string(info->fh) +
                       ",\"offset\":" + std::to_string(start) + ",\"length\":" + std::to_string(size) +
                       ",\"returnedBytes\":" + std::to_string(completed)
  );
  return static_cast<int>(completed);
}

int releaseFile(const char *, struct fuse_file_info *info)
{
  return mount().release(info->fh) ? 0 : -EBADF;
}

off_t seekFile(const char *, off_t offset, int whence, struct fuse_file_info *info)
{
  const auto *entry = mount().handle(info->fh);
  if (!entry)
    return -EBADF;
  if (offset < 0 || static_cast<std::uint64_t>(offset) >= entry->size)
    return -ENXIO;
  if (whence == SEEK_DATA)
    return offset;
  if (whence == SEEK_HOLE)
    return static_cast<off_t>(entry->size);
  return -EINVAL; // SEEK_SET/CUR/END are handled by the kernel.
}

int checkAccess(const char *path, int mode)
{
  const auto *entry = mount().source.findPath(path);
  if (!entry)
    return -ENOENT;
  if ((mode & W_OK) != 0)
    return -EROFS;
  if ((mode & X_OK) != 0 && !entry->directory)
    return -EACCES;
  return 0;
}

int getFilesystemStats(const char *, struct statvfs *stats)
{
  std::memset(stats, 0, sizeof(*stats));
  stats->f_bsize = stats->f_frsize = 4096;
  for (const auto &entry : mount().source.entries())
    stats->f_blocks += entry.size / 4096 + (entry.size % 4096 != 0);
  stats->f_files = mount().source.entries().size();
  stats->f_namemax = 255;
  stats->f_flag = ST_RDONLY | ST_NOSUID;
  return 0;
}

void *initialize(struct fuse_conn_info *connection, struct fuse_config *config)
{
  config->kernel_cache = 0;
  config->auto_cache = 0;
  config->entry_timeout = 0;
  config->attr_timeout = 0;
  config->negative_timeout = 0;
  config->use_ino = 1;
  connection->max_read = FakeRemoteSource::maxRequestBytes;
  connection->max_readahead = 0;
  mount().log.event("ready", "\"maxRequestBytes\":262144,\"maxWorkerThreads\":8,\"readOnly\":true");
  return &mount();
}

struct fuse_operations operations()
{
  struct fuse_operations ops{};
  ops.getattr = getAttributes;
  ops.readdir = readDirectory;
  ops.open = openFile;
  ops.read = readFile;
  ops.release = releaseFile;
  ops.lseek = seekFile;
  ops.access = checkAccess;
  ops.statfs = getFilesystemStats;
  ops.init = initialize;
  ops.destroy = [](void *data) { static_cast<FuseMount *>(data)->log.event("unmounted"); };
  ops.getxattr = [](const char *, const char *, char *, std::size_t) { return -ENODATA; };
  ops.listxattr = [](const char *, char *, std::size_t) { return 0; };
  ops.setxattr = [](const char *, const char *, const char *, std::size_t, int) { return -EROFS; };
  ops.removexattr = [](const char *, const char *) { return -EROFS; };
  ops.write = [](const char *, const char *, std::size_t, off_t, struct fuse_file_info *) { return -EROFS; };
  ops.create = [](const char *, mode_t, struct fuse_file_info *) { return -EROFS; };
  ops.mkdir = [](const char *, mode_t) { return -EROFS; };
  ops.mknod = [](const char *, mode_t, dev_t) { return -EROFS; };
  ops.unlink = [](const char *) { return -EROFS; };
  ops.rmdir = [](const char *) { return -EROFS; };
  ops.rename = [](const char *, const char *, unsigned int) { return -EROFS; };
  ops.symlink = [](const char *, const char *) { return -EROFS; };
  ops.link = [](const char *, const char *) { return -EROFS; };
  ops.chmod = [](const char *, mode_t, struct fuse_file_info *) { return -EROFS; };
  ops.chown = [](const char *, uid_t, gid_t, struct fuse_file_info *) { return -EROFS; };
  ops.truncate = [](const char *, off_t, struct fuse_file_info *) { return -EROFS; };
  ops.utimens = [](const char *, const struct timespec[2], struct fuse_file_info *) { return -EROFS; };
  ops.fallocate = [](const char *, int, off_t, off_t, struct fuse_file_info *) { return -EROFS; };
  // Tell callers to use ordinary reads; never claim an in-kernel copy succeeded.
  ops.copy_file_range = [](const char *, struct fuse_file_info *, off_t, const char *, struct fuse_file_info *, off_t,
                           std::size_t, int) -> ssize_t { return -EOPNOTSUPP; };
  return ops;
}

} // namespace

FuseMount::FuseMount(FakeRemoteSource &source, TraceLog &log) : source(source), log(log)
{
}

std::uint64_t FuseMount::open(const FileEntry &entry)
{
  std::lock_guard lock(m_handlesMutex);
  const auto id = m_nextHandle++;
  m_handles.emplace(id, &entry);
  log.event(
      "open", "\"fileId\":" + jsonQuote(entry.id) + ",\"handleId\":" + std::to_string(id) +
                  ",\"activeHandles\":" + std::to_string(m_handles.size())
  );
  return id;
}

const FileEntry *FuseMount::handle(std::uint64_t handleId)
{
  std::lock_guard lock(m_handlesMutex);
  const auto found = m_handles.find(handleId);
  return found == m_handles.end() ? nullptr : found->second;
}

bool FuseMount::release(std::uint64_t handleId)
{
  std::lock_guard lock(m_handlesMutex);
  const auto removed = m_handles.erase(handleId);
  log.event(
      "release", "\"handleId\":" + std::to_string(handleId) + ",\"activeHandles\":" + std::to_string(m_handles.size())
  );
  return removed != 0;
}

int FuseMount::run(const std::string &mountpoint)
{
  log.event("mount_start", "\"mountpoint\":" + jsonQuote(mountpoint));
  std::vector<std::string> arguments{
      "deskflow-file-prototype",
      "-f",
      "-o",
      "ro,default_permissions,fsname=deskflow-clipboard-prototype,max_read=262144,max_threads=8",
      mountpoint,
  };
  std::vector<char *> argv;
  for (auto &argument : arguments)
    argv.push_back(argument.data());
  const auto callbacks = operations();
  const auto result = fuse_main(static_cast<int>(argv.size()), argv.data(), &callbacks, this);
  log.event("exit", "\"status\":" + std::to_string(result));
  return result;
}

} // namespace clipboard_proto
