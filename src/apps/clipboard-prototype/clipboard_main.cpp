/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ParentWatch.h"

#include "X11Clipboard.h"

#include <charconv>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void printHelp()
{
  std::cout << "Usage:\n"
               "  deskflow-clipboard-probe publish [options] [--] ABSOLUTE_PATH...\n"
               "  deskflow-clipboard-probe publish-formats --format MIME=PATH\n"
               "                                           [--format MIME=PATH ...] [options]\n"
               "  deskflow-clipboard-probe read --target MIME [--output PATH]\n"
               "                                [--timeout-ms N] [--log JSONL]\n"
               "  deskflow-clipboard-probe watch [--once] [options]\n\n"
               "  deskflow-clipboard-probe capture-files [--timeout-ms N] [--log JSONL]\n\n"
               "Options: --log JSONL, --timeout-ms N (1..600000, default 5000),\n"
               "         --origin TOKEN (publisher/watch local identity).\n\n"
               "Publishers own the X11 CLIPBOARD in the foreground until SIGINT, SIGTERM,\n"
               "or another application replaces it. It publishes file URIs without\n"
               "opening or reading the named files. Keep the virtual filesystem mounted.\n"
               "Supported targets include text/uri-list, x-special/gnome-copied-files,\n"
               "application/x-gnome-copied-files, UTF8_STRING, TARGETS, and TIMESTAMP.\n"
               "The legacy publish command limits file-list data to 64 KiB.\n\n"
               "publish-formats reads each source lazily on its first data request, never\n"
               "for TARGETS. Supported MIME types: text/plain;charset=utf-8, text/plain,\n"
               "UTF8_STRING, text/html, image/png, text/uri-list and the two GNOME targets.\n"
               "Use MIME=PATH (the first '=' after the MIME); charset=utf-8 is supported.\n"
               "Limits: 16 MiB per format, 32 MiB per offer, 8 concurrent INCR requests.\n"
               "Larger payloads use INCR; --timeout-ms bounds idle INCR requestors.\n\n"
               "read emits exact binary MIME bytes to stdout or --output; logs never\n"
               "share stdout. TARGETS/TIMESTAMP retain their textual diagnostic rendering.\n"
               "INCR reads are limited to 16 MiB and the entire read deadline. Reads fail\n"
               "if clipboard ownership changes, including a same-window re-acquisition.\n\n"
               "watch uses XFixes to announce owner/TARGETS changes without reading user\n"
               "formats or file contents. Matching local --origin is ignored to stop loops.\n"
               "--once waits at most --timeout-ms for one external offer; without --once\n"
               "watch continues until a signal and applies timeout to metadata requests.\n"
               "The default origin identifies this probe for the current local user.\n"
               "capture-files emits a JSON array of absolute local file paths; it pins\n"
               "owner and timestamp, prefers GNOME copy, and rejects cut/remote URIs.\n"
               "It parses references only (1 MiB, 4096 entries); file contents stay unread.\n"
               "JSONL logs go to --log, otherwise publish/watch use stdout and read stderr.\n"
               "No automatic paste or clipboard restoration is performed.\n";
}

std::string optionValue(int &index, int argc, char **argv)
{
  if (++index >= argc) {
    throw std::invalid_argument(std::string("missing value after ") + argv[index - 1]);
  }
  return argv[index];
}

int parseTimeout(std::string_view value)
{
  int result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result <= 0 || result > 600000) {
    throw std::invalid_argument("--timeout-ms must be an integer from 1 to 600000");
  }
  return result;
}

std::pair<std::string, std::string> parseFormat(const std::string &value)
{
  // The sole accepted MIME parameter is part of the canonical UTF-8 target.
  const std::string charsetPrefix = "text/plain;charset=utf-8=";
  const auto separator = value.starts_with(charsetPrefix) ? charsetPrefix.size() - 1 : value.find('=');
  if (separator == std::string::npos || separator == 0 || separator + 1 == value.size())
    throw std::invalid_argument("--format requires MIME=PATH");
  return {value.substr(0, separator), value.substr(separator + 1)};
}

} // namespace

int main(int argc, char **argv)
{
  try {
    deskflow::clipboard::prototype::ParentWatch parent(argc, argv);
    if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h") {
      printHelp();
      return argc < 2 ? 2 : 0;
    }

    const std::string command = argv[1];
    std::string logPath;
    std::string target;
    std::string origin;
    std::optional<std::string> outputPath;
    std::vector<std::string> paths;
    std::vector<std::pair<std::string, std::string>> formats;
    int timeoutMs = 5000;
    bool once = false;
    std::optional<deskflow::clipboard::prototype::X11Owner> expectedOwner;
    bool positionalOnly = false;

    for (int i = 2; i < argc; ++i) {
      const std::string argument = argv[i];
      if (!positionalOnly && (argument == "--help" || argument == "-h")) {
        printHelp();
        return 0;
      }
      if (!positionalOnly && argument == "--") {
        positionalOnly = true;
      } else if (!positionalOnly && (argument == "--expected-owner" || argument == "--expected-timestamp")) {
        if (!expectedOwner)
          expectedOwner.emplace();
        auto number = std::stoull(optionValue(i, argc, argv));
        if (argument == "--expected-owner")
          expectedOwner->window = number;
        else {
          if (number > UINT32_MAX)
            throw std::invalid_argument("timestamp overflow");
          expectedOwner->selectionTimestamp = number;
        }
      } else if (!positionalOnly && argument == "--log") {
        logPath = optionValue(i, argc, argv);
      } else if (!positionalOnly && argument == "--timeout-ms") {
        timeoutMs = parseTimeout(optionValue(i, argc, argv));
      } else if (!positionalOnly && command != "read" && argument == "--origin") {
        origin = optionValue(i, argc, argv);
      } else if (!positionalOnly && command == "publish-formats" && argument == "--format") {
        formats.push_back(parseFormat(optionValue(i, argc, argv)));
      } else if (!positionalOnly && command == "watch" && argument == "--once") {
        once = true;
      } else if (!positionalOnly && command == "read" && argument == "--target") {
        target = optionValue(i, argc, argv);
      } else if (!positionalOnly && command == "read" && argument == "--output") {
        outputPath = optionValue(i, argc, argv);
      } else if (command == "publish" && (positionalOnly || !argument.starts_with("-"))) {
        paths.push_back(argument);
      } else {
        throw std::invalid_argument("unexpected argument: " + argument);
      }
    }

    if (command == "publish") {
      if (paths.empty()) {
        throw std::invalid_argument("publish requires at least one absolute file path");
      }
      return deskflow::clipboard::prototype::publishFiles(paths, logPath, timeoutMs, origin, expectedOwner);
    }
    if (command == "publish-formats") {
      if (formats.empty())
        throw std::invalid_argument("publish-formats requires at least one --format MIME=PATH");
      return deskflow::clipboard::prototype::publishFormatFiles(formats, logPath, timeoutMs, origin, expectedOwner);
    }
    if (command == "watch") {
      return deskflow::clipboard::prototype::watchX11Clipboard({}, once, logPath, timeoutMs, origin);
    }
    if (command == "capture-files") {
      return deskflow::clipboard::prototype::captureClipboardFiles(timeoutMs, logPath, expectedOwner);
    }
    if (command == "read") {
      if (target.empty()) {
        throw std::invalid_argument("read requires --target");
      }
      return deskflow::clipboard::prototype::readClipboard(target, outputPath, timeoutMs, logPath, expectedOwner);
    }
    throw std::invalid_argument("unknown command: " + command);
  } catch (const std::exception &error) {
    std::cerr << "deskflow-clipboard-probe: " << error.what() << '\n';
    return 2;
  }
}
