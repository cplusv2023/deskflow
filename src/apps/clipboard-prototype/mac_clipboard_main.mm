/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MacClipboard.h"

#import <Foundation/Foundation.h>

#include <charconv>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace {

void help()
{
  std::cout << "Usage:\n"
               "  deskflow-clipboard-probe publish [options] [--] ABSOLUTE_PATH...\n"
               "  deskflow-clipboard-probe publish-data [options] --target MIME --input PATH\n"
               "      [--target MIME --input PATH ...]\n"
               "  deskflow-clipboard-probe read [options] --target MIME [--output PATH]\n"
               "  deskflow-clipboard-probe inspect [options]\n"
               "  deskflow-clipboard-probe watch [options]\n"
               "  deskflow-clipboard-probe release --pasteboard NAME\n\n"
               "Options: --pasteboard NAME, --log JSONL, --origin TOKEN,\n"
               "         --duration-ms N (publish/watch), --poll-ms N (default 100).\n"
               "Without --pasteboard, commands use the system clipboard. Tests should\n"
               "always use a unique name. release is only for named test pasteboards.\n"
               "publish writes native file URLs without opening or stat-ing the files.\n"
               "Keep the virtual filesystem mounted while the URLs can be pasted.\n"
               "publish-data provides text/plain, text/html, and image/png lazily, with\n"
               "a combined 16 MiB input limit. Inputs must be regular files. Keep the\n"
               "publisher running for promised data. Input changes cause read failure.\n"
               "Both publishers run until replaced, interrupted, or duration expires.\n"
               "read additionally supports text/uri-list and TARGETS. Binary output\n"
               "goes to stdout or --output; read logs go to stderr or --log. AppKit\n"
               "reads are synchronous and have no cancellable timeout. The 16 MiB\n"
               "read limit bounds accepted output, not AppKit's internal allocations.\n"
               "inspect/watch report item types and origin without requesting bodies.\n"
               "watch labels matching --origin as selfChange to support loop filtering.\n"
               "This is a local platform probe, not the Deskflow network backend.\n";
}

std::string value(int &i, int argc, char **argv)
{
  if (++i >= argc) {
    throw std::invalid_argument(std::string("missing value after ") + argv[i - 1]);
  }
  return argv[i];
}

int positiveInteger(const std::string &argument)
{
  int result = 0;
  const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || result <= 0 || result > 600000) {
    throw std::invalid_argument("duration and poll interval must be integers from 1 to 600000 ms");
  }
  return result;
}

int run(int argc, char **argv)
{
  using namespace deskflow::clipboard::prototype;
  if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h") {
    help();
    return argc < 2 ? 2 : 0;
  }
  const std::string command = argv[1];
  MacClipboardOptions options;
  std::vector<std::string> paths;
  std::vector<MacClipboardRepresentation> representations;
  std::string target;
  std::optional<std::string> output;
  bool positional = false;
  for (int i = 2; i < argc; ++i) {
    const std::string argument = argv[i];
    if (!positional && (argument == "--help" || argument == "-h")) {
      help();
      return 0;
    }
    if (!positional && argument == "--") {
      positional = true;
    } else if (!positional && argument == "--pasteboard") {
      options.pasteboard = value(i, argc, argv);
      if (options.pasteboard.empty()) {
        throw std::invalid_argument("--pasteboard must not be empty");
      }
    } else if (!positional && argument == "--log") {
      options.logPath = value(i, argc, argv);
    } else if (!positional && argument == "--origin") {
      options.origin = value(i, argc, argv);
      if (options.origin.empty() || options.origin.size() > 256) {
        throw std::invalid_argument("--origin must have 1 to 256 UTF-8 bytes");
      }
    } else if (!positional && argument == "--duration-ms" && (command == "watch" || command.starts_with("publish"))) {
      options.durationMs = positiveInteger(value(i, argc, argv));
    } else if (!positional && argument == "--poll-ms") {
      options.pollMs = positiveInteger(value(i, argc, argv));
      if (options.pollMs > 1000) {
        throw std::invalid_argument("--poll-ms must not exceed 1000");
      }
    } else if (!positional && argument == "--target" && (command == "read" || command == "publish-data")) {
      if (!target.empty()) {
        throw std::invalid_argument("previous --target has no --input, or read has multiple targets");
      }
      target = value(i, argc, argv);
    } else if (!positional && argument == "--input" && command == "publish-data") {
      if (target.empty()) {
        throw std::invalid_argument("--input must follow --target");
      }
      representations.push_back({target, value(i, argc, argv)});
      target.clear();
    } else if (!positional && argument == "--output" && command == "read") {
      output = value(i, argc, argv);
    } else if (command == "publish" && (positional || !argument.starts_with("-"))) {
      paths.push_back(argument);
    } else {
      throw std::invalid_argument("unexpected argument: " + argument);
    }
  }
  if (command == "publish") {
    return publishMacFiles(paths, options);
  }
  if (command == "publish-data") {
    if (!target.empty()) {
      throw std::invalid_argument("--target requires --input");
    }
    return publishMacData(representations, options);
  }
  if (command == "read") {
    if (target.empty()) {
      throw std::invalid_argument("read requires --target");
    }
    return readMacClipboard(target, output, options);
  }
  if (command == "inspect") {
    return inspectMacClipboard(options);
  }
  if (command == "watch") {
    return watchMacClipboard(options);
  }
  if (command == "release") {
    return releaseMacPasteboard(options);
  }
  throw std::invalid_argument("unknown command: " + command);
}

} // namespace

int main(int argc, char **argv)
{
  @autoreleasepool {
    @try {
      try {
        return run(argc, argv);
      } catch (const std::exception &error) {
        std::cerr << "deskflow-clipboard-probe: " << error.what() << '\n';
        return 2;
      }
    } @catch (NSException *exception) {
      std::cerr << "deskflow-clipboard-probe: AppKit exception: " << exception.name.UTF8String << '\n';
      return 2;
    }
  }
}
