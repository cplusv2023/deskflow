/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace deskflow::clipboard::prototype {

inline constexpr std::size_t kX11MaxFormatBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kX11MaxOfferBytes = 32 * 1024 * 1024;

struct X11Format
{
  std::string target;
  // Called on the first data request, never for TARGETS. Binary bytes are safe.
  // The result is bounded and retained while this offer owns the selection.
  std::function<std::string()> provideBytes;
};

struct X11ClipboardOffer
{
  std::vector<X11Format> formats;
  // Local identity metadata; never forwarded as clipboard content.
  std::string origin;
};

struct X11Owner
{
  std::uint64_t window = 0;
  std::uint32_t selectionTimestamp = 0; // Zero means not yet known.
};

struct X11ReadResult
{
  std::string bytes;
  X11Owner owner;
  bool incremental = false;
  // Populated for TARGETS; bytes retains its legacy newline-separated rendering.
  std::vector<std::string> targets;
};

struct X11Announcement
{
  X11Owner owner;
  std::vector<std::string> targets;
};

class X11ClipboardError : public std::runtime_error
{
public:
  X11ClipboardError(std::string status, const std::string &message);
  const std::string &status() const;

private:
  std::string m_status;
};

// Synchronous helpers, each using an independent X11 connection. Foreground
// publisher/watch loops own process signal handlers; run them as separate probes.
int publishX11Offer(const X11ClipboardOffer &offer, const std::string &logPath, int timeoutMs = 5000);
X11ReadResult
readX11Format(const std::string &target, int timeoutMs = 5000, std::optional<X11Owner> expectedOwner = std::nullopt);
// Capture only local file references, never file contents. This pins TARGETS and
// the chosen representation to the same owner and selection timestamp.
std::vector<std::string> readClipboardFiles(int timeoutMs = 5000);
int captureClipboardFiles(int timeoutMs, const std::string &logPath);
int watchX11Clipboard(
    const std::function<void(const X11Announcement &)> &onAnnouncement, bool once, const std::string &logPath,
    int timeoutMs = 5000, const std::string &origin = {}
);
int publishFormatFiles(
    const std::vector<std::pair<std::string, std::string>> &formats, const std::string &logPath, int timeoutMs,
    const std::string &origin
);

// These helpers access the X11 CLIPBOARD explicitly, including under XWayland.
// Publishing never opens, stats, or reads the files represented by these paths.
int publishFiles(
    const std::vector<std::string> &paths, const std::string &logPath, int timeoutMs = 5000,
    const std::string &origin = {}
);
int readClipboard(
    const std::string &target, const std::optional<std::string> &outputPath, int timeoutMs, const std::string &logPath
);

} // namespace deskflow::clipboard::prototype
