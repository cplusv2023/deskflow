/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "X11Clipboard.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace deskflow::clipboard::prototype {
namespace {

constexpr std::size_t kMaxPublishBytes = 64 * 1024;
constexpr std::size_t kMaxReadBytes = 16 * 1024 * 1024;
volatile std::sig_atomic_t g_stop = 0;
int g_xError = 0;

void stopHandler(int)
{
  g_stop = 1;
}

int xErrorHandler(Display *, XErrorEvent *event)
{
  // A requester can disappear between SelectionRequest and XChangeProperty.
  // Do not let Xlib's default handler terminate the clipboard owner in that case.
  g_xError = event->error_code;
  return 0;
}

std::string quote(std::string_view value)
{
  constexpr char hex[] = "0123456789abcdef";
  std::string result = "\"";
  for (const char byte : value) {
    const auto character = static_cast<unsigned char>(byte);
    switch (character) {
    case '\"':
      result += "\\\"";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      if (character < 0x20) {
        result += "\\u00";
        result += hex[character >> 4];
        result += hex[character & 15];
      } else {
        result += static_cast<char>(character);
      }
    }
  }
  return result + '"';
}

class Logger
{
public:
  Logger(const std::string &path, std::ostream &fallback) : m_stream(&fallback)
  {
    if (!path.empty()) {
      m_file.open(path, std::ios::app);
      if (!m_file) {
        throw std::runtime_error("cannot open JSONL log: " + path);
      }
      m_stream = &m_file;
    }
  }

  void write(std::string_view event, const std::string &fields = {})
  {
    const auto now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    *m_stream << "{\"event\":" << quote(event) << ",\"timestamp_ms\":" << now;
    if (!fields.empty()) {
      *m_stream << ',' << fields;
    }
    *m_stream << "}\n";
    m_stream->flush();
    if (!*m_stream) {
      throw std::runtime_error("cannot write JSONL log");
    }
  }

private:
  std::ofstream m_file;
  std::ostream *m_stream;
};

class SignalGuard
{
public:
  SignalGuard()
  {
    g_stop = 0;
    struct sigaction action{};
    action.sa_handler = stopHandler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &m_oldInterrupt) != 0) {
      throw std::runtime_error("cannot install SIGINT handler");
    }
    if (sigaction(SIGTERM, &action, &m_oldTerminate) != 0) {
      sigaction(SIGINT, &m_oldInterrupt, nullptr);
      throw std::runtime_error("cannot install SIGTERM handler");
    }
  }

  ~SignalGuard()
  {
    sigaction(SIGINT, &m_oldInterrupt, nullptr);
    sigaction(SIGTERM, &m_oldTerminate, nullptr);
  }

private:
  struct sigaction m_oldInterrupt{};
  struct sigaction m_oldTerminate{};
};

class XSession
{
public:
  XSession()
  {
    m_display = XOpenDisplay(nullptr);
    if (!m_display) {
      throw std::runtime_error("cannot open X11 DISPLAY (an X11/XWayland connection is required)");
    }
    m_oldErrorHandler = XSetErrorHandler(xErrorHandler);
    m_window = XCreateSimpleWindow(m_display, DefaultRootWindow(m_display), 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(m_display, m_window, PropertyChangeMask);
  }

  ~XSession()
  {
    XDestroyWindow(m_display, m_window);
    XCloseDisplay(m_display);
    XSetErrorHandler(m_oldErrorHandler);
  }

  XSession(const XSession &) = delete;
  XSession &operator=(const XSession &) = delete;

  Display *display() const
  {
    return m_display;
  }

  Window window() const
  {
    return m_window;
  }

  Atom atom(const char *name) const
  {
    return XInternAtom(m_display, name, False);
  }

  std::string name(Atom atom) const
  {
    if (atom == None) {
      return "None";
    }
    char *value = XGetAtomName(m_display, atom);
    if (!value) {
      return "unknown";
    }
    const std::string result(value);
    XFree(value);
    return result;
  }

  bool nextEvent(XEvent &event, int timeoutMs) const
  {
    if (XPending(m_display)) {
      XNextEvent(m_display, &event);
      return true;
    }
    pollfd descriptor{ConnectionNumber(m_display), POLLIN, 0};
    const int result = poll(&descriptor, 1, timeoutMs);
    if (result < 0 && errno != EINTR) {
      throw std::runtime_error("poll failed: " + std::string(std::strerror(errno)));
    }
    if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      throw std::runtime_error("X11 connection closed");
    }
    if (result > 0 && XPending(m_display)) {
      XNextEvent(m_display, &event);
      return true;
    }
    return false;
  }

  Time serverTime() const
  {
    const Atom property = atom("_DESKFLOW_CLIPBOARD_PROBE_TIMESTAMP");
    const unsigned char byte = 0;
    XChangeProperty(m_display, m_window, property, XA_INTEGER, 8, PropModeReplace, &byte, 1);
    XFlush(m_display);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    XEvent event{};
    while (!g_stop && std::chrono::steady_clock::now() < deadline) {
      if (nextEvent(event, 100) && event.type == PropertyNotify && event.xproperty.atom == property &&
          event.xproperty.state == PropertyNewValue) {
        XDeleteProperty(m_display, m_window, property);
        return event.xproperty.time;
      }
    }
    throw std::runtime_error("timed out obtaining X11 server timestamp");
  }

private:
  Display *m_display;
  Window m_window;
  XErrorHandler m_oldErrorHandler;
};

std::string fileUri(const std::string &path)
{
  if (!std::filesystem::path(path).is_absolute()) {
    throw std::invalid_argument("clipboard file paths must be absolute: " + path);
  }
  constexpr char hex[] = "0123456789ABCDEF";
  std::string result = "file://";
  for (const char byte : path) {
    const auto character = static_cast<unsigned char>(byte);
    if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '/' || character == '-' || character == '.' ||
        character == '_' || character == '~') {
      result += static_cast<char>(character);
    } else {
      result += '%';
      result += hex[character >> 4];
      result += hex[character & 15];
    }
  }
  return result;
}

struct Property
{
  Atom type = None;
  int format = 0;
  std::string bytes;
  std::vector<unsigned long> words;
};

Property
readProperty(const XSession &session, Window window, Atom property, bool remove, std::size_t limit = kMaxReadBytes)
{
  Property result;
  unsigned long count = 0;
  unsigned long remaining = 0;
  unsigned char *data = nullptr;
  const int status = XGetWindowProperty(
      session.display(), window, property, 0, static_cast<long>(limit / 4 + 1), remove, AnyPropertyType, &result.type,
      &result.format, &count, &remaining, &data
  );
  const std::unique_ptr<unsigned char, decltype(&XFree)> guard(data, XFree);
  if (status != Success) {
    throw std::runtime_error("cannot read X11 selection property");
  }
  const auto bytes = count * static_cast<unsigned long>(result.format / 8);
  if (remaining != 0 || bytes > limit) {
    throw std::runtime_error("X11 property exceeds the configured byte limit: " + std::to_string(limit));
  }
  if (result.format == 8 && count) {
    result.bytes.assign(reinterpret_cast<const char *>(data), count);
  } else if (result.format == 32 && count) {
    const auto *words = reinterpret_cast<const unsigned long *>(data);
    result.words.assign(words, words + count);
  } else if (result.format != 0 && result.format != 8 && result.format != 32) {
    throw std::runtime_error("unsupported X11 property format: " + std::to_string(result.format));
  }
  return result;
}

void writeOutput(const std::string &data, const std::optional<std::string> &path)
{
  if (!path) {
    std::cout.write(data.data(), static_cast<std::streamsize>(data.size()));
    std::cout.flush();
    if (!std::cout) {
      throw std::runtime_error("cannot write clipboard data to stdout");
    }
    return;
  }

  // Commit output only after the complete selection transfer succeeded.
  std::string temporary = *path + ".XXXXXX";
  std::vector<char> name(temporary.begin(), temporary.end());
  name.push_back('\0');
  int descriptor = mkstemp(name.data());
  if (descriptor < 0) {
    throw std::runtime_error("cannot create output beside " + *path + ": " + std::strerror(errno));
  }
  try {
    std::size_t offset = 0;
    while (offset < data.size()) {
      const auto written = write(descriptor, data.data() + offset, data.size() - offset);
      if (written <= 0) {
        if (written < 0 && errno == EINTR) {
          continue;
        }
        throw std::runtime_error("cannot write clipboard output: " + std::string(std::strerror(errno)));
      }
      offset += static_cast<std::size_t>(written);
    }
    const int closeResult = close(descriptor);
    descriptor = -1;
    if (closeResult != 0 || rename(name.data(), path->c_str()) != 0) {
      throw std::runtime_error("cannot commit clipboard output: " + std::string(std::strerror(errno)));
    }
  } catch (...) {
    if (descriptor >= 0) {
      close(descriptor);
    }
    unlink(name.data());
    throw;
  }
}

} // namespace

int publishFiles(const std::vector<std::string> &paths, const std::string &logPath)
{
  Logger log(logPath, std::cout);
  try {
    std::string uriList;
    std::string copiedFiles = "copy";
    for (const auto &path : paths) {
      const auto uri = fileUri(path);
      uriList += uri + "\r\n";
      copiedFiles += '\n' + uri;
      if (uriList.size() > kMaxPublishBytes || copiedFiles.size() > kMaxPublishBytes) {
        throw std::invalid_argument("file-list payload exceeds the explicit 64 KiB publisher limit");
      }
    }

    SignalGuard signals;
    XSession session;
    Display *display = session.display();
    const Atom clipboard = session.atom("CLIPBOARD");
    const Atom targetsAtom = session.atom("TARGETS");
    const Atom timestampAtom = session.atom("TIMESTAMP");
    const Atom multipleAtom = session.atom("MULTIPLE");
    const Atom atomPair = session.atom("ATOM_PAIR");
    const std::map<Atom, std::string> payloads{
        {session.atom("text/uri-list"), uriList},
        {session.atom("x-special/gnome-copied-files"), copiedFiles},
        {session.atom("application/x-gnome-copied-files"), copiedFiles},
        {session.atom("UTF8_STRING"), uriList},
        {session.atom("text/plain;charset=utf-8"), uriList},
        {session.atom("text/plain"), uriList}
    };
    std::vector<Atom> targets{targetsAtom, timestampAtom, multipleAtom};
    for (const auto &[target, unused] : payloads) {
      targets.push_back(target);
    }

    // Stay below the ordinary X11 request limit instead of silently truncating
    // a large file list. INCR publishing can be added if larger lists are needed.
    const auto requestUnits = XMaxRequestSize(display);
    if (requestUnits <= 64) {
      throw std::runtime_error("X11 server request limit is too small");
    }
    const auto maxProperty = std::min(kMaxPublishBytes, static_cast<std::size_t>(requestUnits - 64) * 4);
    for (const auto &[target, payload] : payloads) {
      if (payload.size() > maxProperty) {
        throw std::runtime_error("file-list payload exceeds this X11 server's property limit");
      }
    }

    const Time acquired = session.serverTime();
    XSetSelectionOwner(display, clipboard, session.window(), acquired);
    XSync(display, False);
    if (XGetSelectionOwner(display, clipboard) != session.window()) {
      throw std::runtime_error("cannot acquire X11 CLIPBOARD ownership");
    }

    log.write(
        "ready", "\"files\":" + std::to_string(paths.size()) + ",\"max_property_bytes\":" +
                     std::to_string(maxProperty) + ",\"display\":" + quote(DisplayString(display))
    );

    const auto convert = [&](Window requestor, Atom target, Atom property) {
      if (property == None) {
        return false;
      }
      if (target == targetsAtom) {
        XChangeProperty(
            display, requestor, property, XA_ATOM, 32, PropModeReplace,
            reinterpret_cast<const unsigned char *>(targets.data()), static_cast<int>(targets.size())
        );
        return true;
      }
      if (target == timestampAtom) {
        const unsigned long timestamp = acquired;
        XChangeProperty(
            display, requestor, property, XA_INTEGER, 32, PropModeReplace,
            reinterpret_cast<const unsigned char *>(&timestamp), 1
        );
        return true;
      }
      if (const auto payload = payloads.find(target); payload != payloads.end()) {
        XChangeProperty(
            display, requestor, property, target, 8, PropModeReplace,
            reinterpret_cast<const unsigned char *>(payload->second.data()), static_cast<int>(payload->second.size())
        );
        return true;
      }
      return false;
    };

    while (!g_stop) {
      XEvent event{};
      if (!session.nextEvent(event, 100)) {
        continue;
      }
      if (event.type == SelectionClear && event.xselectionclear.selection == clipboard) {
        log.write("clipboard_lost");
        return 0;
      }
      if (event.type != SelectionRequest) {
        continue;
      }
      const auto &request = event.xselectionrequest;
      const Atom property = request.property == None ? request.target : request.property;
      bool accepted = false;
      g_xError = 0;
      const bool validTime = request.time == CurrentTime || static_cast<std::int32_t>(request.time - acquired) >= 0;
      try {
        if (request.owner == session.window() && request.selection == clipboard && validTime) {
          if (request.target == multipleAtom && request.property != None) {
            auto pairs = readProperty(session, request.requestor, property, false, maxProperty);
            if ((pairs.type == atomPair || pairs.type == XA_ATOM) && pairs.format == 32 &&
                pairs.words.size() % 2 == 0) {
              for (std::size_t i = 0; i < pairs.words.size(); i += 2) {
                if (pairs.words[i] == multipleAtom || pairs.words[i + 1] == property ||
                    !convert(request.requestor, pairs.words[i], pairs.words[i + 1])) {
                  pairs.words[i + 1] = None;
                }
              }
              XChangeProperty(
                  display, request.requestor, property, atomPair, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char *>(pairs.words.data()), static_cast<int>(pairs.words.size())
              );
              accepted = true;
            }
          } else {
            accepted = convert(request.requestor, request.target, property);
          }
        }
      } catch (const std::exception &error) {
        // A malformed MULTIPLE request or a vanished requester must not take
        // ownership away from the next application that wants to paste.
        log.write("selection_error", "\"message\":" + quote(error.what()));
      }
      XSync(display, False);
      if (g_xError) {
        accepted = false;
      }

      XEvent response{};
      response.xselection.type = SelectionNotify;
      response.xselection.display = display;
      response.xselection.requestor = request.requestor;
      response.xselection.selection = request.selection;
      response.xselection.target = request.target;
      response.xselection.property = accepted ? property : None;
      response.xselection.time = request.time;
      XSendEvent(display, request.requestor, False, NoEventMask, &response);
      XSync(display, False);
      log.write(
          "selection_request", "\"target\":" + quote(session.name(request.target)) + ",\"accepted\":" +
                                   (accepted ? "true" : "false") + ",\"x_error\":" + std::to_string(g_xError)
      );
    }
    log.write("stopped", "\"reason\":\"signal\"");
    return 0;
  } catch (const std::exception &error) {
    log.write("error", "\"message\":" + quote(error.what()));
    return 1;
  }
}

int readClipboard(
    const std::string &targetName, const std::optional<std::string> &outputPath, int timeoutMs,
    const std::string &logPath
)
{
  Logger log(logPath, std::cerr);
  try {
    SignalGuard signals;
    XSession session;
    Display *display = session.display();
    const Atom clipboard = session.atom("CLIPBOARD");
    const Atom target = session.atom(targetName.c_str());
    const Atom property = session.atom("_DESKFLOW_CLIPBOARD_PROBE_DATA");
    const Atom increment = session.atom("INCR");
    if (XGetSelectionOwner(display, clipboard) == None) {
      throw std::runtime_error("X11 CLIPBOARD has no owner");
    }
    XConvertSelection(display, clipboard, target, property, session.window(), session.serverTime());
    XFlush(display);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool incremental = false;
    bool notified = false;
    std::string data;
    while (!g_stop && std::chrono::steady_clock::now() < deadline) {
      XEvent event{};
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (!session.nextEvent(event, static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 0, 100)))) {
        continue;
      }
      if (event.type == SelectionNotify && event.xselection.selection == clipboard &&
          event.xselection.target == target) {
        notified = true;
        if (event.xselection.property == None) {
          throw std::runtime_error("clipboard owner rejected target: " + targetName);
        }
        auto value = readProperty(session, session.window(), property, false);
        if (value.type == increment) {
          if (value.format != 32 || value.words.size() != 1 || value.words.front() > kMaxReadBytes) {
            throw std::runtime_error("unsupported or oversized X11 INCR transfer");
          }
          incremental = true;
          XDeleteProperty(display, session.window(), property);
          XFlush(display);
          continue;
        }
        if (value.format == 8) {
          data = std::move(value.bytes);
        } else if (targetName == "TARGETS" && value.format == 32 && value.type == XA_ATOM) {
          for (const auto atom : value.words) {
            data += session.name(atom) + '\n';
          }
        } else if (targetName == "TIMESTAMP" && value.format == 32 && value.words.size() == 1) {
          data = std::to_string(value.words.front()) + '\n';
        } else {
          throw std::runtime_error("clipboard returned an unsupported property type or format");
        }
        XDeleteProperty(display, session.window(), property);
        writeOutput(data, outputPath);
        log.write("read_complete", "\"target\":" + quote(targetName) + ",\"bytes\":" + std::to_string(data.size()));
        return 0;
      }
      if (notified && incremental && event.type == PropertyNotify && event.xproperty.atom == property &&
          event.xproperty.state == PropertyNewValue) {
        const auto chunk = readProperty(session, session.window(), property, true);
        XFlush(display);
        if (chunk.format != 8) {
          throw std::runtime_error("only 8-bit clipboard INCR data is supported");
        }
        if (chunk.bytes.empty()) {
          writeOutput(data, outputPath);
          log.write(
              "read_complete",
              "\"target\":" + quote(targetName) + ",\"bytes\":" + std::to_string(data.size()) + ",\"incremental\":true"
          );
          return 0;
        }
        if (chunk.bytes.size() > kMaxReadBytes - data.size()) {
          throw std::runtime_error("clipboard data exceeds the 16 MiB reader limit");
        }
        data += chunk.bytes;
      }
    }
    throw std::runtime_error(g_stop ? "clipboard read cancelled by signal" : "clipboard read timed out");
  } catch (const std::exception &error) {
    log.write("error", "\"message\":" + quote(error.what()));
    return 1;
  }
}

} // namespace deskflow::clipboard::prototype
