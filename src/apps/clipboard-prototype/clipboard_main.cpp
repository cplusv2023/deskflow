/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

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
               "  deskflow-clipboard-probe publish [--log JSONL] [--] ABSOLUTE_PATH...\n"
               "  deskflow-clipboard-probe read --target MIME [--output PATH]\n"
               "                                [--timeout-ms N] [--log JSONL]\n\n"
               "publish owns the X11 CLIPBOARD in the foreground until SIGINT, SIGTERM,\n"
               "or another application replaces it. It publishes file URIs without\n"
               "opening or reading the named files. Keep the virtual filesystem mounted.\n"
               "Supported targets include text/uri-list, x-special/gnome-copied-files,\n"
               "application/x-gnome-copied-files, UTF8_STRING, TARGETS, and TIMESTAMP.\n"
               "File-list payloads are limited to 64 KiB (or the X server request limit).\n\n"
               "read requests one target from the current clipboard; its payload goes\n"
               "to stdout unless --output is supplied. TARGETS and TIMESTAMP are printed\n"
               "as text. Reads are limited to 16 MiB, including INCR transfers.\n"
               "The default read timeout is 5000 ms. No automatic paste is performed.\n"
               "JSONL logs go to --log, otherwise publish uses stdout and read stderr.\n";
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

} // namespace

int main(int argc, char **argv)
{
  try {
    if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h") {
      printHelp();
      return argc < 2 ? 2 : 0;
    }

    const std::string command = argv[1];
    std::string logPath;
    std::string target;
    std::optional<std::string> outputPath;
    std::vector<std::string> paths;
    int timeoutMs = 5000;
    bool positionalOnly = false;

    for (int i = 2; i < argc; ++i) {
      const std::string argument = argv[i];
      if (!positionalOnly && (argument == "--help" || argument == "-h")) {
        printHelp();
        return 0;
      }
      if (!positionalOnly && argument == "--") {
        positionalOnly = true;
      } else if (!positionalOnly && argument == "--log") {
        logPath = optionValue(i, argc, argv);
      } else if (!positionalOnly && command == "read" && argument == "--target") {
        target = optionValue(i, argc, argv);
      } else if (!positionalOnly && command == "read" && argument == "--output") {
        outputPath = optionValue(i, argc, argv);
      } else if (!positionalOnly && command == "read" && argument == "--timeout-ms") {
        timeoutMs = parseTimeout(optionValue(i, argc, argv));
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
      return deskflow::clipboard::prototype::publishFiles(paths, logPath);
    }
    if (command == "read") {
      if (target.empty()) {
        throw std::invalid_argument("read requires --target");
      }
      return deskflow::clipboard::prototype::readClipboard(target, outputPath, timeoutMs, logPath);
    }
    throw std::invalid_argument("unknown command: " + command);
  } catch (const std::exception &error) {
    std::cerr << "deskflow-clipboard-probe: " << error.what() << '\n';
    return 2;
  }
}
