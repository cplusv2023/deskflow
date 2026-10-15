/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "FakeRemoteSource.h"
#include "X11Clipboard.h"
#include "clipboard/LocalFileRpc.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <csignal>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace deskflow::clipboard;
std::atomic_bool stop{false};
static_assert(std::atomic_bool::is_always_lock_free);
void stopServer(int)
{
  stop.store(true);
}

std::uint64_t number(const std::string &text)
{
  std::uint64_t value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
    throw std::invalid_argument("Expected an unsigned decimal integer");
  return value;
}
void require(Status status)
{
  if (status != Status::Ok)
    throw RpcError(status, std::string(statusName(status)));
}
void printOffer(const OfferInfo &offer)
{
  using clipboard_proto::jsonQuote;
  std::cout << "{\"sessionEpoch\":" << jsonQuote(offer.sessionEpoch) << ",\"offerId\":" << jsonQuote(offer.offerId)
            << ",\"offerVersion\":" << jsonQuote(offer.offerVersion) << ",\"immutableVersion\":false,\"files\":[";
  bool first = true;
  for (const auto &file : offer.files) {
    if (!first)
      std::cout << ',';
    first = false;
    std::cout << "{\"fileId\":" << jsonQuote(file.fileId) << ",\"path\":" << jsonQuote(file.relativePath)
              << ",\"version\":" << jsonQuote(file.version) << ",\"size\":" << jsonQuote(std::to_string(file.size))
              << ",\"kind\":" << jsonQuote(file.kind == FileKind::Directory ? "directory" : "file") << '}';
  }
  std::cout << "]}\n";
}
void help()
{
  std::cout << "Usage:\n"
               "  deskflow-file-service serve --socket PATH --source PATH [--source PATH ...]\n"
               "  deskflow-file-service serve --socket PATH --from-clipboard [--log JSONL]\n"
               "      [--log JSONL] [--source-chunk-size 1..262144] [--delay-ms 0..60000]\n"
               "  deskflow-file-service inspect --socket PATH [--timeout-ms N]\n"
               "  deskflow-file-service read --socket PATH --path OFFER_RELATIVE_PATH\n"
               "      --offset N --length N --output NEW_PATH --detectable [--timeout-ms N]\n\n"
               "Experimental same-user AF_UNIX service; not a remote or Deskflow protocol.\n"
               "Create the socket parent directory with mode 0700. Server paths alone authorize files.\n"
               "Ordinary local files cannot promise immutable versions; read requires explicit --detectable.\n"
               "Publication/listing never reads file bodies. Source chunks are at most 256 KiB.\n"
               "Output must not exist and is atomically published only after all range reads succeed.\n";
}
} // namespace

int main(int argc, char **argv)
{
  try {
    if (argc < 2 || std::string_view(argv[1]) == "--help") {
      help();
      return argc < 2 ? 2 : 0;
    }
    const std::string command = argv[1];
    FileRpcServerOptions server;
    std::string relativePath, output;
    std::uint64_t offset = 0, length = 0;
    bool hasOffset = false, hasLength = false, detectable = false, fromClipboard = false;
    std::uint32_t timeout = 5000;
    for (int i = 2; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--detectable") {
        detectable = true;
        continue;
      }
      if (option == "--from-clipboard" && command == "serve") {
        fromClipboard = true;
        continue;
      }
      if (option == "--help") {
        help();
        return 0;
      }
      if (++i == argc)
        throw std::invalid_argument("Missing option value");
      const std::string value = argv[i];
      if (option == "--socket")
        server.socketPath = value;
      else if (option == "--source" && command == "serve")
        server.paths.emplace_back(value);
      else if (option == "--log" && command == "serve")
        server.logPath = value;
      else if (option == "--path" && command == "read")
        relativePath = value;
      else if (option == "--output" && command == "read")
        output = value;
      else if (option == "--offset" && command == "read") {
        offset = number(value);
        hasOffset = true;
      } else if (option == "--length" && command == "read") {
        length = number(value);
        hasLength = true;
      } else if (option == "--timeout-ms" ||
                 (command == "serve" && (option == "--source-chunk-size" || option == "--delay-ms"))) {
        const auto parsed = number(value);
        if (parsed > UINT32_MAX)
          throw std::invalid_argument("Option exceeds uint32 range");
        if (option == "--timeout-ms")
          timeout = static_cast<std::uint32_t>(parsed);
        else if (option == "--source-chunk-size")
          server.responseBytes = static_cast<std::uint32_t>(parsed);
        else
          server.delayMs = static_cast<std::uint32_t>(parsed);
      } else
        throw std::invalid_argument("Unexpected option: " + option);
    }
    if (server.socketPath.empty())
      throw std::invalid_argument("--socket is required");
    if (timeout == 0 || timeout > 600000)
      throw std::invalid_argument("--timeout-ms must be between 1 and 600000");
    if (command == "serve") {
      if (fromClipboard) {
        if (!server.paths.empty())
          throw std::invalid_argument("Choose --source or --from-clipboard");
        for (const auto &path : deskflow::clipboard::prototype::readClipboardFiles(static_cast<int>(timeout)))
          server.paths.emplace_back(path);
      }
      std::signal(SIGTERM, stopServer);
      std::signal(SIGINT, stopServer);
      serveLocalFiles(server, stop);
      return 0;
    }
    if (command != "inspect" && command != "read")
      throw std::invalid_argument("Unknown command");
    FileRpcClient client(server.socketPath, timeout);
    const auto description = client.hello();
    require(description.status);
    if (command == "inspect") {
      printOffer(description.offer);
      return 0;
    }
    if (!detectable || relativePath.empty() || output.empty() || !hasOffset || !hasLength)
      throw std::invalid_argument("read requires --path, --offset, --length, --output and --detectable");
    const auto &offer = description.offer;
    const auto selected = std::find_if(offer.files.begin(), offer.files.end(), [&](const auto &file) {
      return file.relativePath == relativePath && file.kind == FileKind::File;
    });
    if (selected == offer.files.end())
      throw RpcError(Status::NotFound, "File not in authorized offer");
    if (std::filesystem::exists(output))
      throw std::invalid_argument("Output already exists");
    const auto lease = client.acquire(offer);
    require(lease.status);
    const auto opened = client.open(lease.leaseId, *selected, ConsistencyMode::Detectable);
    require(opened.status);
    std::string temporary = output + ".deskflow-XXXXXX";
    const int fd = ::mkstemp(temporary.data());
    if (fd < 0)
      throw std::runtime_error("Cannot create output temporary file");
    struct Output
    {
      int fd;
      std::string path;
      ~Output()
      {
        ::close(fd);
        ::unlink(path.c_str());
      }
    } cleanup{fd, temporary};
    const auto wanted = offset >= opened.size ? std::uint64_t{0} : std::min(length, opened.size - offset);
    std::uint64_t completed = 0;
    const auto renewalDelay = [](std::uint32_t ttl) {
      return std::chrono::milliseconds(std::max<std::uint32_t>(1, std::min<std::uint32_t>(10000, ttl / 3)));
    };
    auto renewAt = std::chrono::steady_clock::now() + renewalDelay(lease.grantedTtlMs);
    while (completed < wanted) {
      if (std::chrono::steady_clock::now() >= renewAt) {
        const auto renewed = client.renew(lease.leaseId);
        require(renewed.status);
        renewAt = std::chrono::steady_clock::now() + renewalDelay(renewed.grantedTtlMs);
      }
      const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(262144, wanted - completed));
      const auto result = client.read(lease.leaseId, opened.handleId, offset + completed, count);
      require(result.status);
      if (result.bytes.empty() || result.bytes.size() > count || result.version != opened.version ||
          result.fileId != selected->fileId || result.offerId != offer.offerId ||
          result.eof != (result.bytes.size() == opened.size - (offset + completed)))
        throw RpcError(Status::InvalidSourceResponse, "Invalid short response or EOF");
      std::size_t written = 0;
      while (written < result.bytes.size()) {
        const auto n = ::write(fd, result.bytes.data() + written, result.bytes.size() - written);
        if (n < 0 && errno == EINTR)
          continue;
        if (n <= 0)
          throw std::runtime_error("Output write failed");
        written += static_cast<std::size_t>(n);
      }
      completed += result.bytes.size();
    }
    require(client.close(opened.handleId));
    require(client.release(lease.leaseId).status);
    if (::fsync(fd) != 0 || ::link(temporary.c_str(), output.c_str()) != 0)
      throw std::runtime_error("Cannot atomically publish output (destination may already exist)");
    std::cerr << "{\"status\":\"OK\",\"returnedBytes\":" << completed << "}\n";
    return 0;
  } catch (const RpcError &error) {
    std::cerr << "deskflow-file-service: " << statusName(error.status) << ": " << error.what() << '\n';
    return 1;
  } catch (const std::exception &error) {
    std::cerr << "deskflow-file-service: " << error.what() << '\n';
    return 2;
  }
}
