// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <charconv>
#include <chrono>
#include <csignal>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace deskflow::clipboard::prototype {
// Internal core helper option. Parent death also shuts down providers/mounts when
// normal QProcess teardown cannot run (crash or SIGKILL). No unrelated PID is signalled.
class ParentWatch
{
public:
  ParentWatch(int &argc, char **argv)
  {
    for (int i = 1; i < argc; ++i) {
      if (std::string_view(argv[i]) != "--parent-pid")
        continue;
      if (i + 1 >= argc)
        throw std::invalid_argument("missing parent pid");
      std::string_view value(argv[i + 1]);
      long long parsed = 0;
      const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
      if (ec != std::errc{} || end != value.data() + value.size() || parsed <= 1 ||
          parsed > std::numeric_limits<pid_t>::max() || m_thread.joinable())
        throw std::invalid_argument("invalid parent pid");
      const auto parent = static_cast<pid_t>(parsed);
      if (::getppid() != parent)
        throw std::runtime_error("core parent has already exited");
      for (int j = i; j + 2 <= argc; ++j)
        argv[j] = argv[j + 2];
      argc -= 2;
      --i;
      m_thread = std::jthread([parent](std::stop_token stop) {
        while (!stop.stop_requested()) {
          if (::getppid() != parent) {
            ::kill(::getpid(), SIGTERM);
            return;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      });
    }
  }

private:
  std::jthread m_thread;
};
} // namespace deskflow::clipboard::prototype
