/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ParentWatch.h"

#include "FakeRemoteSource.h"
#include "FuseMount.h"
#include "RpcFileSource.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

std::uint64_t number(std::string_view text, std::string_view option)
{
  std::uint64_t result = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    throw std::invalid_argument(std::string(option) + " requires an unsigned decimal integer");
  return result;
}

void help()
{
  std::cout << "Usage:\n"
               "  deskflow-file-prototype --mount PATH --log JSONL [options]\n"
               "  deskflow-file-prototype --socket PATH --mount PATH --log JSONL [--detectable]\n"
               "  deskflow-file-prototype --describe [options]\n"
               "  deskflow-file-prototype --read FILEID --offset N --length N --output PATH [options]\n\n"
               "Options:\n"
               "  --socket PATH           connect to same-user experimental file service\n"
               "  --detectable            explicitly allow detectable local-file consistency\n"
               "  --timeout-ms N          RPC timeout, 1..600000 (default 5000)\n"
               "  --lease-ttl-ms N        requested RPC lease TTL, 1..60000 (default 30000)\n"
               "  --large-size BYTES       large.bin size (default 4294967419)\n"
               "  --delay-ms N             simulated delay per source response (default 0)\n"
               "  --source-chunk-size N    maximum bytes returned per source call, 1..262144\n"
               "  --log JSONL             flushed request trace (required for mounts)\n"
               "  --foreground            accepted for clarity; all mounts are foreground\n"
               "  --help                  show this help\n\n"
               "Mountpoint must be an existing empty directory owned by the current user.\n"
               "Mounts are read-only and never use allow_other. SIGINT/SIGTERM exits and unmounts.\n"
               "No source files or complete file caches are created. Generated file byte =\n"
               "(absoluteOffset % 251 + seed) % 251; --describe gives sizes, seeds and literals.\n";
}

} // namespace

int main(int argc, char **argv)
{
  try {
    deskflow::clipboard::prototype::ParentWatch parent(argc, argv);
    std::string mountpoint;
    std::string logPath;
    std::string readId;
    std::string outputPath;
    std::string socketPath;
    clipboard_proto::RpcFileSource::Options rpcOptions;
    bool rpcOptionSet = false;
    bool fakeOptionSet = false;
    bool describe = false;
    bool hasOffset = false;
    bool hasLength = false;
    std::uint64_t largeSize = clipboard_proto::FakeRemoteSource::defaultLargeSize;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint64_t delayMs = 0;
    std::uint64_t sourceChunkSize = clipboard_proto::FakeRemoteSource::maxRequestBytes;
    for (int index = 1; index < argc; ++index) {
      const std::string option = argv[index];
      if (option == "--help" || option == "-h") {
        help();
        return 0;
      }
      if (option == "--foreground")
        continue;
      if (option == "--detectable") {
        rpcOptions.detectable = true;
        rpcOptionSet = true;
        continue;
      }
      if (option == "--describe") {
        describe = true;
        continue;
      }
      if (index + 1 == argc)
        throw std::invalid_argument("Missing value for " + option);
      const std::string value = argv[++index];
      if (option == "--mount")
        mountpoint = value;
      else if (option == "--log")
        logPath = value;
      else if (option == "--read")
        readId = value;
      else if (option == "--output")
        outputPath = value;
      else if (option == "--socket")
        socketPath = value;
      else if (option == "--timeout-ms" || option == "--lease-ttl-ms") {
        const auto parsed = number(value, option);
        const auto limit = option == "--timeout-ms" ? 600000U : 60000U;
        if (parsed == 0 || parsed > limit)
          throw std::invalid_argument(option + " must be between 1 and " + std::to_string(limit));
        if (option == "--timeout-ms")
          rpcOptions.timeoutMs = static_cast<std::uint32_t>(parsed);
        else
          rpcOptions.leaseTtlMs = static_cast<std::uint32_t>(parsed);
        rpcOptionSet = true;
      } else if (option == "--large-size") {
        largeSize = number(value, option);
        fakeOptionSet = true;
      } else if (option == "--delay-ms") {
        delayMs = number(value, option);
        fakeOptionSet = true;
      } else if (option == "--source-chunk-size") {
        sourceChunkSize = number(value, option);
        fakeOptionSet = true;
      } else if (option == "--offset") {
        offset = number(value, option);
        hasOffset = true;
      } else if (option == "--length") {
        length = number(value, option);
        hasLength = true;
      } else
        throw std::invalid_argument("Unknown option: " + option);
    }
    const auto modes = static_cast<unsigned int>(describe) + static_cast<unsigned int>(!readId.empty()) +
                       static_cast<unsigned int>(!mountpoint.empty());
    if (modes != 1)
      throw std::invalid_argument("Choose exactly one of --mount, --describe and --read");
    if ((!socketPath.empty() && mountpoint.empty()) || (socketPath.empty() && rpcOptionSet))
      throw std::invalid_argument("--socket and its options require --mount; RPC options require --socket");
    if (!socketPath.empty() && fakeOptionSet)
      throw std::invalid_argument("Set generated source options on the service, separately from --socket mounts");
    if (delayMs > std::numeric_limits<unsigned int>::max())
      throw std::invalid_argument("delay-ms is too large");
    if (sourceChunkSize == 0 || sourceChunkSize > clipboard_proto::FakeRemoteSource::maxRequestBytes)
      throw std::invalid_argument("source-chunk-size must be between 1 and 262144");
    if (readId.empty() && (hasOffset || hasLength || !outputPath.empty()))
      throw std::invalid_argument("--offset, --length and --output require --read");
    if (!mountpoint.empty()) {
      if (logPath.empty())
        throw std::invalid_argument("--mount requires --log");
      mountpoint = std::filesystem::canonical(mountpoint).string();
      if (!std::filesystem::is_directory(mountpoint) || !std::filesystem::is_empty(mountpoint))
        throw std::invalid_argument("Mountpoint must be an existing empty directory");
      struct stat mountAttributes{};
      if (::stat(mountpoint.c_str(), &mountAttributes) != 0 || mountAttributes.st_uid != getuid())
        throw std::invalid_argument("Mountpoint must be owned by the current user");
      const auto absoluteLog = std::filesystem::weakly_canonical(logPath).string();
      if (absoluteLog == mountpoint || absoluteLog.starts_with(mountpoint + '/'))
        throw std::invalid_argument("Trace log must be outside the mountpoint");
    }
    clipboard_proto::TraceLog log(logPath);
    if (!socketPath.empty()) {
      clipboard_proto::RpcFileSource source(socketPath, log, rpcOptions);
      clipboard_proto::FuseMount filesystem(source, log);
      return filesystem.run(mountpoint);
    }
    clipboard_proto::FakeRemoteSource source(
        log, largeSize, static_cast<unsigned int>(delayMs), static_cast<std::size_t>(sourceChunkSize)
    );
    if (describe) {
      std::cout << source.describe() << '\n';
      return 0;
    }
    if (!readId.empty()) {
      if (!hasOffset || !hasLength || outputPath.empty())
        throw std::invalid_argument("--read requires --offset, --length and --output");
      const auto *entry = source.findId(readId);
      if (!entry || entry->directory)
        throw std::invalid_argument("--read must name a regular fileId from --describe");
      std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
      if (!output)
        throw std::runtime_error("Cannot create output: " + outputPath);
      const auto wanted = offset >= entry->size ? std::uint64_t{0} : std::min(length, entry->size - offset);
      std::vector<char> buffer(clipboard_proto::FakeRemoteSource::maxRequestBytes);
      std::uint64_t completed = 0;
      while (completed < wanted) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(wanted - completed, buffer.size()));
        const auto count = source.readRange(*entry, offset + completed, {buffer.data(), chunk});
        if (count == 0)
          throw std::runtime_error("Unexpected zero-length source response before EOF");
        output.write(buffer.data(), static_cast<std::streamsize>(count));
        if (!output)
          throw std::runtime_error("Writing output failed");
        completed += count;
      }
      output.close();
      if (!output)
        throw std::runtime_error("Closing output failed");
      return 0;
    }
    clipboard_proto::FuseMount filesystem(source, log);
    return filesystem.run(mountpoint);
  } catch (const std::exception &error) {
    std::cerr << "deskflow-file-prototype: " << error.what() << '\n';
    return 1;
  }
}
