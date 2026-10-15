/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace deskflow::clipboard::prototype {

struct MacClipboardOptions
{
  // Empty selects the system clipboard. Tests must use a unique named pasteboard.
  std::string pasteboard;
  std::string logPath;
  std::string origin;
  std::optional<long long> expectedChangeCount;
  int durationMs = 0;
  int pollMs = 100;
  bool skipInitial = false;
};

struct MacClipboardRepresentation
{
  std::string target;
  std::string inputPath;
};

// File publication transfers URL metadata only and never opens or stats the files.
int publishMacFiles(const std::vector<std::string> &paths, const MacClipboardOptions &options);
// Pins bounded local representation inputs; reads their bytes on an AppKit request.
// This prototype detects ordinary file changes; it does not claim immutable snapshots.
int publishMacData(const std::vector<MacClipboardRepresentation> &representations, const MacClipboardOptions &options);
// Exact HTML/PNG representations take priority. If absent, supports text-only RTF
// -> HTML (1 MiB input, nesting <=128, no embedded objects/images/fields) and
// single-image TIFF -> PNG (<=8192 per edge, <=4 Mi pixels, <=16 bits/component).
// Converted output is limited to 16 MiB. TIFF dimensions are checked before decode;
// the native parsers' internal allocations are not a separately enforceable budget.
int readMacClipboard(
    const std::string &target, const std::optional<std::string> &outputPath, const MacClipboardOptions &options
);
int inspectMacClipboard(const MacClipboardOptions &options);
int watchMacClipboard(const MacClipboardOptions &options);
// Only named pasteboards may be released, to allow isolated test cleanup.
int releaseMacPasteboard(const MacClipboardOptions &options);

} // namespace deskflow::clipboard::prototype
