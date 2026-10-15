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

// These helpers access the X11 CLIPBOARD explicitly, including under XWayland.
// Publishing never opens, stats, or reads the files represented by these paths.
int publishFiles(const std::vector<std::string> &paths, const std::string &logPath);
int readClipboard(
    const std::string &target, const std::optional<std::string> &outputPath, int timeoutMs, const std::string &logPath
);

} // namespace deskflow::clipboard::prototype
