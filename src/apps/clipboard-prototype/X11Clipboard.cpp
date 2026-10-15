/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "X11Clipboard.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <poll.h>
#include <set>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace deskflow::clipboard::prototype {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kDirectBytes = 64 * 1024;
constexpr std::size_t kMaxTransfers = 8;
constexpr std::size_t kMaxFileListBytes = 1024 * 1024;
constexpr std::size_t kMaxFileListEntries = 4096;
constexpr const char *kOriginProperty = "_DESKFLOW_CLIPBOARD_PROTOTYPE_ORIGIN";
volatile std::sig_atomic_t g_stop = 0;
thread_local int g_xError = 0;
std::once_flag g_xInitialized;

void stopHandler(int)
{
  g_stop = 1;
}

int xErrorHandler(Display *, XErrorEvent *event)
{
  // A requestor can disappear before a property operation. Surface that failure
  // to this connection's caller instead of terminating the entire owner.
  g_xError = event->error_code;
  return 0;
}

[[noreturn]] void fail(const std::string &status, const std::string &message)
{
  throw X11ClipboardError(status, message);
}

std::string quote(std::string_view value)
{
  constexpr char hex[] = "0123456789abcdef";
  std::string result = "\"";
  for (const char byte : value) {
    const auto character = static_cast<unsigned char>(byte);
    switch (character) {
    case '"':
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

std::string stringArray(const std::vector<std::string> &values)
{
  std::string result = "[";
  for (const auto &value : values) {
    if (result.size() > 1)
      result += ',';
    result += quote(value);
  }
  return result + ']';
}

std::string defaultOrigin(const std::string &requested)
{
  const auto origin = requested.empty() ? "deskflow-clipboard-prototype:" + std::to_string(getuid()) : requested;
  if (origin.size() > 256)
    throw std::invalid_argument("origin identity exceeds 256 bytes");
  return origin;
}

class Logger
{
public:
  Logger(const std::string &path, std::ostream &fallback) : m_stream(&fallback)
  {
    if (!path.empty()) {
      m_file.open(path, std::ios::app);
      if (!m_file)
        throw std::runtime_error("cannot open JSONL log: " + path);
      m_stream = &m_file;
    }
  }

  void write(std::string_view event, const std::string &fields = {})
  {
    const auto now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    *m_stream << "{\"event\":" << quote(event) << ",\"timestamp_ms\":" << now;
    if (!fields.empty())
      *m_stream << ',' << fields;
    *m_stream << "}\n";
    m_stream->flush();
    if (!*m_stream)
      throw std::runtime_error("cannot write JSONL log");
  }

  int error(const std::exception &error)
  {
    const auto *typed = dynamic_cast<const X11ClipboardError *>(&error);
    write("error", "\"status\":" + quote(typed ? typed->status() : "IO_ERROR") + ",\"message\":" + quote(error.what()));
    return 1;
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
    if (sigaction(SIGINT, &action, &m_oldInterrupt) != 0)
      throw std::runtime_error("cannot install SIGINT handler");
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
    std::call_once(g_xInitialized, [] {
      if (!XInitThreads())
        throw std::runtime_error("cannot initialize Xlib threading");
      XSetErrorHandler(xErrorHandler);
    });
    m_display = XOpenDisplay(nullptr);
    if (!m_display)
      fail("DISCONNECTED", "cannot open X11 DISPLAY (an X11/XWayland connection is required)");
    m_window = XCreateSimpleWindow(m_display, DefaultRootWindow(m_display), 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(m_display, m_window, PropertyChangeMask);
  }

  ~XSession()
  {
    XDestroyWindow(m_display, m_window);
    XCloseDisplay(m_display);
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
  std::uint64_t revision() const
  {
    return m_revision;
  }
  Time selectionTimestamp() const
  {
    return m_lastTimestamp;
  }

  Atom atom(const char *name) const
  {
    return XInternAtom(m_display, name, False);
  }

  std::string name(Atom value) const
  {
    if (value == None)
      return "None";
    char *text = XGetAtomName(m_display, value);
    if (!text)
      return "unknown";
    const std::string result(text);
    XFree(text);
    return result;
  }

  void watchSelection(Atom selection)
  {
    int errorBase = 0;
    if (!XFixesQueryExtension(m_display, &m_fixesBase, &errorBase))
      fail("UNSUPPORTED", "XFixes selection notifications are required");
    int major = 5;
    int minor = 0;
    XFixesQueryVersion(m_display, &major, &minor);
    if (major < 1)
      fail("UNSUPPORTED", "XFixes version 1 or later is required");
    m_watched = selection;
    XFixesSelectSelectionInput(
        m_display, m_window, selection,
        XFixesSetSelectionOwnerNotifyMask | XFixesSelectionWindowDestroyNotifyMask |
            XFixesSelectionClientCloseNotifyMask
    );
    XSync(m_display, False);
  }

  bool selectionEvent(const XEvent &event) const
  {
    return m_watched != None && event.type == m_fixesBase + XFixesSelectionNotify &&
           reinterpret_cast<const XFixesSelectionNotifyEvent *>(&event)->selection == m_watched;
  }

  bool nextEvent(XEvent &event, int timeoutMs)
  {
    if (!XPending(m_display)) {
      pollfd descriptor{ConnectionNumber(m_display), POLLIN, 0};
      const int result = poll(&descriptor, 1, timeoutMs);
      if (result < 0 && errno != EINTR)
        fail("IO_ERROR", "poll failed: " + std::string(std::strerror(errno)));
      if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
        fail("DISCONNECTED", "X11 connection closed");
      if (result <= 0 || !XPending(m_display))
        return false;
    }
    XNextEvent(m_display, &event);
    if (selectionEvent(event)) {
      const auto *selection = reinterpret_cast<const XFixesSelectionNotifyEvent *>(&event);
      ++m_revision;
      m_lastTimestamp = selection->selection_timestamp;
    }
    return true;
  }

  void drain()
  {
    XEvent event{};
    while (XPending(m_display))
      nextEvent(event, 0);
  }

  Time serverTime()
  {
    const Atom property = atom("_DESKFLOW_CLIPBOARD_PROBE_TIMESTAMP");
    const unsigned char byte = 0;
    XChangeProperty(m_display, m_window, property, XA_INTEGER, 8, PropModeReplace, &byte, 1);
    XFlush(m_display);
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    XEvent event{};
    while (!g_stop && Clock::now() < deadline) {
      if (nextEvent(event, 100) && event.type == PropertyNotify && event.xproperty.atom == property &&
          event.xproperty.state == PropertyNewValue) {
        XDeleteProperty(m_display, m_window, property);
        return event.xproperty.time;
      }
    }
    fail(g_stop ? "CANCELLED" : "TIMEOUT", "cannot obtain X11 server timestamp");
  }

private:
  Display *m_display;
  Window m_window;
  Atom m_watched = None;
  int m_fixesBase = 0;
  std::uint64_t m_revision = 0;
  Time m_lastTimestamp = 0;
};

std::string fileUri(const std::string &path)
{
  if (!std::filesystem::path(path).is_absolute())
    throw std::invalid_argument("clipboard file paths must be absolute: " + path);
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
readProperty(const XSession &session, Window window, Atom property, bool remove, std::size_t limit = kX11MaxFormatBytes)
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
  if (status != Success)
    fail("IO_ERROR", "cannot read X11 selection property");
  const auto bytes = count * static_cast<unsigned long>(result.format / 8);
  if (remaining != 0 || bytes > limit)
    fail("TOO_LARGE", "X11 property exceeds the configured byte limit: " + std::to_string(limit));
  if (result.format == 8 && count) {
    result.bytes.assign(reinterpret_cast<const char *>(data), count);
  } else if (result.format == 32 && count) {
    const auto *words = reinterpret_cast<const unsigned long *>(data);
    result.words.assign(words, words + count);
  } else if (result.format != 0 && result.format != 8 && result.format != 32) {
    fail("INVALID_SOURCE_RESPONSE", "unsupported X11 property width");
  }
  return result;
}

void writeOutput(const std::string &data, const std::optional<std::string> &path)
{
  if (!path) {
    std::cout.write(data.data(), static_cast<std::streamsize>(data.size()));
    std::cout.flush();
    if (!std::cout)
      throw std::runtime_error("cannot write clipboard data to stdout");
    return;
  }
  std::string temporary = *path + ".XXXXXX";
  std::vector<char> name(temporary.begin(), temporary.end());
  name.push_back('\0');
  int descriptor = mkstemp(name.data());
  if (descriptor < 0)
    throw std::runtime_error("cannot create output beside " + *path + ": " + std::strerror(errno));
  try {
    std::size_t offset = 0;
    while (offset < data.size()) {
      const auto written = write(descriptor, data.data() + offset, data.size() - offset);
      if (written <= 0) {
        if (written < 0 && errno == EINTR)
          continue;
        throw std::runtime_error("cannot write clipboard output");
      }
      offset += static_cast<std::size_t>(written);
    }
    const int closeResult = close(descriptor);
    descriptor = -1;
    if (closeResult != 0 || rename(name.data(), path->c_str()) != 0)
      throw std::runtime_error("cannot commit clipboard output: " + std::string(std::strerror(errno)));
  } catch (...) {
    if (descriptor >= 0)
      close(descriptor);
    unlink(name.data());
    throw;
  }
}

void checkOwner(XSession &session, Atom selection, Window owner, std::uint64_t revision)
{
  XSync(session.display(), False);
  session.drain();
  const auto current = XGetSelectionOwner(session.display(), selection);
  if (current != owner || session.revision() != revision)
    fail("OWNER_CHANGED", "clipboard ownership changed during the request");
}

Property receive(
    XSession &session, Atom selection, Atom target, Atom property, Window owner, std::uint64_t revision,
    Clock::time_point deadline, bool &incremental
)
{
  XConvertSelection(session.display(), selection, target, property, session.window(), CurrentTime);
  XFlush(session.display());
  const Atom incr = session.atom("INCR");
  bool notified = false;
  bool receiving = false;
  Atom dataType = None;
  Property result;
  while (!g_stop && Clock::now() < deadline) {
    XEvent event{};
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    if (!session.nextEvent(event, static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 0, 100))))
      continue;
    if (session.revision() != revision)
      fail("OWNER_CHANGED", "clipboard ownership changed during the request");
    if (event.type == SelectionNotify && event.xselection.requestor == session.window() &&
        event.xselection.selection == selection && event.xselection.target == target) {
      if (notified)
        fail("INVALID_SOURCE_RESPONSE", "duplicate SelectionNotify");
      notified = true;
      if (event.xselection.property == None)
        fail("UNSUPPORTED_TARGET", "clipboard owner rejected target: " + session.name(target));
      if (event.xselection.property != property)
        fail("INVALID_SOURCE_RESPONSE", "clipboard replied on the wrong property");
      auto value = readProperty(session, session.window(), property, false);
      if (value.type != incr) {
        XDeleteProperty(session.display(), session.window(), property);
        checkOwner(session, selection, owner, revision);
        return value;
      }
      if (value.format != 32 || value.words.size() != 1 || value.words.front() > kX11MaxFormatBytes)
        fail("TOO_LARGE", "unsupported or oversized INCR transfer");
      incremental = true;
      receiving = true;
      XDeleteProperty(session.display(), session.window(), property);
      XFlush(session.display());
      continue;
    }
    if (receiving && event.type == PropertyNotify && event.xproperty.window == session.window() &&
        event.xproperty.atom == property && event.xproperty.state == PropertyNewValue) {
      const auto chunk = readProperty(session, session.window(), property, true);
      XFlush(session.display());
      if (chunk.format != 8 || chunk.type == None || (dataType != None && chunk.type != dataType))
        fail("INVALID_SOURCE_RESPONSE", "INCR chunks have inconsistent type or width");
      dataType = chunk.type;
      if (chunk.bytes.empty()) {
        result.type = dataType;
        result.format = 8;
        checkOwner(session, selection, owner, revision);
        return result;
      }
      if (chunk.bytes.size() > kX11MaxFormatBytes - result.bytes.size())
        fail("TOO_LARGE", "clipboard data exceeds 16 MiB");
      result.bytes += chunk.bytes;
    }
  }
  fail(g_stop ? "CANCELLED" : "TIMEOUT", g_stop ? "clipboard read cancelled" : "clipboard read timed out");
}

struct Payload
{
  std::function<std::string()> provide;
  std::shared_ptr<const std::string> data;
};

struct Transfer
{
  Atom target;
  std::shared_ptr<const std::string> data;
  std::size_t offset = 0;
  Clock::time_point deadline;
};

using TransferKey = std::pair<Window, Atom>;

bool supportedMime(const std::string &target)
{
  static const std::set<std::string> supported{
      "text/plain;charset=utf-8",
      "text/plain",
      "UTF8_STRING",
      "text/html",
      "image/png",
      "text/uri-list",
      "x-special/gnome-copied-files",
      "application/x-gnome-copied-files"
  };
  return supported.contains(target);
}

int hexValue(char character)
{
  if (character >= '0' && character <= '9')
    return character - '0';
  if (character >= 'a' && character <= 'f')
    return character - 'a' + 10;
  if (character >= 'A' && character <= 'F')
    return character - 'A' + 10;
  return -1;
}

std::string lowerAscii(std::string value)
{
  for (auto &character : value) {
    if (character >= 'A' && character <= 'Z')
      character = static_cast<char>(character + ('a' - 'A'));
  }
  return value;
}

std::string localUriPath(const std::string &uri)
{
  if (uri.size() < 6 || lowerAscii(uri.substr(0, 5)) != "file:")
    fail("INVALID_SOURCE_RESPONSE", "clipboard contains a non-file URI");
  std::string encoded = uri.substr(5);
  if (encoded.starts_with("//")) {
    const auto slash = encoded.find('/', 2);
    if (slash == std::string::npos)
      fail("INVALID_SOURCE_RESPONSE", "file URI has no absolute path");
    const auto host = lowerAscii(encoded.substr(2, slash - 2));
    if (!host.empty() && host != "localhost")
      fail("INVALID_SOURCE_RESPONSE", "remote file URI hosts are not supported");
    encoded.erase(0, slash);
  }
  if (encoded.empty() || encoded.front() != '/' || encoded.find_first_of("?#") != std::string::npos)
    fail("INVALID_SOURCE_RESPONSE", "file URI must contain an absolute path without query or fragment");
  std::string path;
  path.reserve(encoded.size());
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    char character = encoded[index];
    if (character == '%') {
      if (index + 2 >= encoded.size())
        fail("INVALID_SOURCE_RESPONSE", "truncated percent escape in file URI");
      const int high = hexValue(encoded[index + 1]);
      const int low = hexValue(encoded[index + 2]);
      if (high < 0 || low < 0)
        fail("INVALID_SOURCE_RESPONSE", "malformed percent escape in file URI");
      character = static_cast<char>((high << 4) | low);
      index += 2;
    }
    if (character == '\0')
      fail("INVALID_SOURCE_RESPONSE", "NUL is not valid in a local file path");
    path += character;
  }
  if (path.empty() || path.front() != '/')
    fail("INVALID_SOURCE_RESPONSE", "decoded file path is not absolute");
  return path;
}

std::vector<std::string> decodeFileList(const std::string &bytes, bool gnome)
{
  if (bytes.size() > kMaxFileListBytes)
    fail("TOO_LARGE", "clipboard file list exceeds 1 MiB");
  if (bytes.find('\0') != std::string::npos)
    fail("INVALID_SOURCE_RESPONSE", "clipboard file list contains NUL");
  std::vector<std::string> paths;
  bool headerSeen = !gnome;
  for (std::size_t start = 0; start < bytes.size();) {
    const auto end = bytes.find('\n', start);
    auto line = bytes.substr(start, end == std::string::npos ? bytes.size() - start : end - start);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.find('\r') != std::string::npos)
      fail("INVALID_SOURCE_RESPONSE", "file-list lines contain an unescaped carriage return");
    if (!headerSeen) {
      if (line == "cut")
        fail("UNSUPPORTED_ACTION", "cut clipboard operations are not supported");
      if (line != "copy")
        fail("INVALID_SOURCE_RESPONSE", "GNOME file list must begin with copy");
      headerSeen = true;
    } else if (!line.empty() && (gnome || !line.starts_with('#'))) {
      paths.push_back(localUriPath(line));
      if (paths.size() > kMaxFileListEntries)
        fail("TOO_LARGE", "clipboard file list exceeds 4096 entries");
    }
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  if (paths.empty())
    fail("INVALID_SOURCE_RESPONSE", "clipboard file list is empty");
  return paths;
}

} // namespace

X11ClipboardError::X11ClipboardError(std::string status, const std::string &message)
    : std::runtime_error(message),
      m_status(std::move(status))
{
}

const std::string &X11ClipboardError::status() const
{
  return m_status;
}

X11ReadResult readX11Format(const std::string &targetName, int timeoutMs, std::optional<X11Owner> expectedOwner)
{
  if (timeoutMs <= 0)
    throw std::invalid_argument("timeout must be positive");
  XSession session;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  const Atom selection = session.atom("CLIPBOARD");
  session.watchSelection(selection);
  session.drain();
  const auto revision = session.revision();
  const Window owner = XGetSelectionOwner(session.display(), selection);
  if (owner == None)
    fail("NO_OWNER", "X11 CLIPBOARD has no owner");
  if (expectedOwner && expectedOwner->window != owner)
    fail("OWNER_CHANGED", "clipboard owner differs from the requested offer");
  const Atom property = session.atom("_DESKFLOW_CLIPBOARD_PROBE_DATA");
  bool incremental = false;
  std::uint32_t selectionTimestamp = expectedOwner ? expectedOwner->selectionTimestamp : 0;
  if (targetName == "TARGETS" || selectionTimestamp) {
    const auto timestamp =
        receive(session, selection, session.atom("TIMESTAMP"), property, owner, revision, deadline, incremental);
    if (timestamp.type != XA_INTEGER || timestamp.format != 32 || timestamp.words.size() != 1)
      fail("INVALID_SOURCE_RESPONSE", "clipboard did not supply a valid ownership timestamp");
    if (selectionTimestamp && static_cast<std::uint32_t>(timestamp.words.front()) != selectionTimestamp)
      fail("OWNER_CHANGED", "clipboard selection timestamp differs from the requested offer");
    selectionTimestamp = static_cast<std::uint32_t>(timestamp.words.front());
  }
  const auto value =
      receive(session, selection, session.atom(targetName.c_str()), property, owner, revision, deadline, incremental);
  X11ReadResult result;
  result.owner = {owner, selectionTimestamp};
  result.incremental = incremental;
  if (value.format == 8 && value.type != None) {
    result.bytes = value.bytes;
  } else if (targetName == "TARGETS" && value.format == 32 && value.type == XA_ATOM) {
    for (const auto atom : value.words) {
      const auto name = session.name(atom);
      result.targets.push_back(name);
      result.bytes += name + '\n';
    }
  } else if (targetName == "TIMESTAMP" && value.format == 32 && value.type == XA_INTEGER && value.words.size() == 1) {
    result.bytes = std::to_string(static_cast<std::uint32_t>(value.words.front())) + '\n';
  } else {
    fail("INVALID_SOURCE_RESPONSE", "clipboard returned an unsupported property type or width");
  }
  return result;
}

std::vector<std::string> readClipboardFiles(int timeoutMs)
{
  if (timeoutMs <= 0)
    throw std::invalid_argument("timeout must be positive");
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  const auto available = readX11Format("TARGETS", timeoutMs);
  for (const auto *target : {"x-special/gnome-copied-files", "application/x-gnome-copied-files", "text/uri-list"}) {
    if (std::find(available.targets.begin(), available.targets.end(), target) == available.targets.end())
      continue;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (remaining <= 0)
      fail("TIMEOUT", "clipboard file-list capture timed out");
    const auto result = readX11Format(target, static_cast<int>(remaining), available.owner);
    return decodeFileList(result.bytes, std::string_view(target) != "text/uri-list");
  }
  fail("UNSUPPORTED_TARGET", "clipboard does not contain a supported file list");
}

int captureClipboardFiles(int timeoutMs, const std::string &logPath)
{
  Logger log(logPath, std::cerr);
  try {
    SignalGuard signals;
    const auto paths = readClipboardFiles(timeoutMs);
    writeOutput(stringArray(paths) + '\n', std::nullopt);
    log.write("files_captured", "\"count\":" + std::to_string(paths.size()));
    return 0;
  } catch (const std::exception &error) {
    return log.error(error);
  }
}

int publishX11Offer(const X11ClipboardOffer &offer, const std::string &logPath, int timeoutMs)
{
  Logger log(logPath, std::cout);
  try {
    if (offer.formats.empty() || offer.formats.size() > 32 || timeoutMs <= 0)
      throw std::invalid_argument("offer requires 1..32 formats and a positive timeout");
    SignalGuard signals;
    XSession session;
    Display *display = session.display();
    const Atom clipboard = session.atom("CLIPBOARD");
    const Atom targetsAtom = session.atom("TARGETS");
    const Atom timestampAtom = session.atom("TIMESTAMP");
    const Atom multipleAtom = session.atom("MULTIPLE");
    const Atom atomPair = session.atom("ATOM_PAIR");
    const Atom incrAtom = session.atom("INCR");
    const auto origin = defaultOrigin(offer.origin);
    XChangeProperty(
        display, session.window(), session.atom(kOriginProperty), session.atom("UTF8_STRING"), 8, PropModeReplace,
        reinterpret_cast<const unsigned char *>(origin.data()), static_cast<int>(origin.size())
    );
    std::map<Atom, std::shared_ptr<Payload>> payloads;
    for (const auto &format : offer.formats) {
      if (!supportedMime(format.target) || !format.provideBytes)
        throw std::invalid_argument("unsupported or empty data provider: " + format.target);
      const Atom atom = session.atom(format.target.c_str());
      if (!payloads.emplace(atom, std::make_shared<Payload>(Payload{format.provideBytes, {}})).second)
        throw std::invalid_argument("duplicate clipboard format: " + format.target);
    }
    // Text aliases share one lazy provider and one allocation.
    if (const auto text = payloads.find(session.atom("text/plain;charset=utf-8")); text != payloads.end()) {
      payloads.try_emplace(session.atom("UTF8_STRING"), text->second);
      payloads.try_emplace(session.atom("text/plain"), text->second);
    }
    std::vector<Atom> targets{targetsAtom, timestampAtom, multipleAtom};
    std::vector<std::string> targetNames;
    for (const auto &[target, payload] : payloads) {
      targets.push_back(target);
      targetNames.push_back(session.name(target));
    }
    const auto units = XMaxRequestSize(display);
    if (units <= 64)
      fail("UNSUPPORTED", "X11 server request limit is too small");
    const auto directBytes = std::min(kDirectBytes, static_cast<std::size_t>(units - 64) * 4);
    const Time acquired = session.serverTime();
    XSetSelectionOwner(display, clipboard, session.window(), acquired);
    XSync(display, False);
    if (XGetSelectionOwner(display, clipboard) != session.window())
      fail("OWNER_CHANGED", "cannot acquire X11 CLIPBOARD ownership");
    log.write(
        "ready", "\"owner\":" + std::to_string(session.window()) + ",\"targets\":" + stringArray(targetNames) +
                     ",\"max_property_bytes\":" + std::to_string(directBytes) +
                     ",\"display\":" + quote(DisplayString(display))
    );

    std::map<TransferKey, Transfer> transfers;
    struct Subscription
    {
      long previousMask;
      std::size_t references;
    };
    std::map<Window, Subscription> subscriptions;
    std::size_t loadedBytes = 0;
    const auto eraseTransfer = [&](std::map<TransferKey, Transfer>::iterator iterator, bool restoreMask = true) {
      const Window requestor = iterator->first.first;
      const auto subscription = subscriptions.find(requestor);
      if (subscription != subscriptions.end() && --subscription->second.references == 0) {
        if (restoreMask)
          XSelectInput(display, requestor, subscription->second.previousMask);
        subscriptions.erase(subscription);
      }
      return transfers.erase(iterator);
    };
    const auto convert = [&](Window requestor, Atom target, Atom property) {
      if (property == None || transfers.contains({requestor, property}))
        return false;
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
      const auto found = payloads.find(target);
      if (found == payloads.end())
        return false;
      auto &payload = *found->second;
      if (!payload.data) {
        auto data = payload.provide();
        if (data.size() > kX11MaxFormatBytes || data.size() > kX11MaxOfferBytes - loadedBytes)
          fail("TOO_LARGE", "format or aggregate offer exceeds the bounded data budget");
        payload.data = std::make_shared<const std::string>(std::move(data));
        loadedBytes += payload.data->size();
        log.write(
            "data_provided",
            "\"target\":" + quote(session.name(target)) + ",\"bytes\":" + std::to_string(payload.data->size())
        );
      }
      if (payload.data->size() <= directBytes) {
        XChangeProperty(
            display, requestor, property, target, 8, PropModeReplace,
            reinterpret_cast<const unsigned char *>(payload.data->data()), static_cast<int>(payload.data->size())
        );
        return true;
      }
      if (transfers.size() >= kMaxTransfers)
        fail("RESOURCE_LIMIT", "too many concurrent INCR requests");
      auto subscription = subscriptions.find(requestor);
      if (subscription == subscriptions.end()) {
        XWindowAttributes attributes{};
        if (!XGetWindowAttributes(display, requestor, &attributes))
          return false;
        subscription = subscriptions.emplace(requestor, Subscription{attributes.your_event_mask, 0}).first;
        XSelectInput(display, requestor, attributes.your_event_mask | PropertyChangeMask | StructureNotifyMask);
      }
      ++subscription->second.references;
      transfers.emplace(
          TransferKey{requestor, property},
          Transfer{target, payload.data, 0, Clock::now() + std::chrono::milliseconds(timeoutMs)}
      );
      const unsigned long length = payload.data->size();
      XChangeProperty(
          display, requestor, property, incrAtom, 32, PropModeReplace, reinterpret_cast<const unsigned char *>(&length),
          1
      );
      log.write(
          "incremental_start", "\"target\":" + quote(session.name(target)) + ",\"bytes\":" + std::to_string(length)
      );
      return true;
    };

    while (!g_stop) {
      for (auto it = transfers.begin(); it != transfers.end();) {
        if (Clock::now() >= it->second.deadline) {
          log.write("incremental_cancelled", "\"status\":\"TIMEOUT\",\"owner\":" + std::to_string(it->first.first));
          it = eraseTransfer(it);
        } else {
          ++it;
        }
      }
      XEvent event{};
      if (!session.nextEvent(event, 100))
        continue;
      if (event.type == SelectionClear && event.xselectionclear.selection == clipboard) {
        log.write("clipboard_lost", "\"cancelledTransfers\":" + std::to_string(transfers.size()));
        return 0;
      }
      if (event.type == DestroyNotify) {
        for (auto it = transfers.begin(); it != transfers.end();) {
          if (it->first.first == event.xdestroywindow.window)
            it = eraseTransfer(it, false);
          else
            ++it;
        }
        continue;
      }
      if (event.type == PropertyNotify && event.xproperty.state == PropertyDelete) {
        const auto found = transfers.find({event.xproperty.window, event.xproperty.atom});
        if (found == transfers.end())
          continue;
        auto &transfer = found->second;
        const auto count = std::min(directBytes, transfer.data->size() - transfer.offset);
        g_xError = 0;
        XChangeProperty(
            display, found->first.first, found->first.second, transfer.target, 8, PropModeReplace,
            reinterpret_cast<const unsigned char *>(transfer.data->data() + transfer.offset), static_cast<int>(count)
        );
        transfer.offset += count;
        transfer.deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        XSync(display, False);
        if (count == 0 || g_xError) {
          log.write(
              g_xError ? "incremental_cancelled" : "incremental_complete",
              "\"bytes\":" + std::to_string(transfer.offset) + ",\"x_error\":" + std::to_string(g_xError)
          );
          eraseTransfer(found, g_xError != BadWindow);
        }
        continue;
      }
      if (event.type != SelectionRequest)
        continue;
      const auto &request = event.xselectionrequest;
      const Atom property = request.property == None ? request.target : request.property;
      bool accepted = false;
      g_xError = 0;
      const bool validTime = request.time == CurrentTime || static_cast<std::int32_t>(request.time - acquired) >= 0;
      try {
        if (request.owner == session.window() && request.selection == clipboard && validTime) {
          if (request.target == multipleAtom && request.property != None) {
            auto pairs = readProperty(session, request.requestor, property, false, directBytes);
            if ((pairs.type == atomPair || pairs.type == XA_ATOM) && pairs.format == 32 &&
                pairs.words.size() % 2 == 0) {
              for (std::size_t i = 0; i < pairs.words.size(); i += 2) {
                try {
                  if (pairs.words[i] == multipleAtom || pairs.words[i + 1] == property ||
                      !convert(request.requestor, pairs.words[i], pairs.words[i + 1]))
                    pairs.words[i + 1] = None;
                } catch (const X11ClipboardError &error) {
                  pairs.words[i + 1] = None;
                  log.write(
                      "selection_error", "\"status\":" + quote(error.status()) + ",\"message\":" + quote(error.what())
                  );
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
        const auto *typed = dynamic_cast<const X11ClipboardError *>(&error);
        log.write(
            "selection_error",
            "\"status\":" + quote(typed ? typed->status() : "IO_ERROR") + ",\"message\":" + quote(error.what())
        );
      }
      XSync(display, False);
      if (g_xError)
        accepted = false;
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
    log.write("stopped", "\"reason\":\"signal\",\"cancelledTransfers\":" + std::to_string(transfers.size()));
    return 0;
  } catch (const std::exception &error) {
    return log.error(error);
  }
}

int publishFiles(
    const std::vector<std::string> &paths, const std::string &logPath, int timeoutMs, const std::string &origin
)
{
  std::string uriList;
  std::string copiedFiles = "copy";
  for (const auto &path : paths) {
    const auto uri = fileUri(path);
    uriList += uri + "\r\n";
    copiedFiles += '\n' + uri;
    if (uriList.size() > kDirectBytes || copiedFiles.size() > kDirectBytes)
      throw std::invalid_argument("legacy file-list publisher limit is 64 KiB");
  }
  X11ClipboardOffer offer;
  offer.origin = origin;
  for (const auto *target : {"text/uri-list", "text/plain;charset=utf-8"})
    offer.formats.push_back({target, [uriList] { return uriList; }});
  for (const auto *target : {"x-special/gnome-copied-files", "application/x-gnome-copied-files"})
    offer.formats.push_back({target, [copiedFiles] { return copiedFiles; }});
  return publishX11Offer(offer, logPath, timeoutMs);
}

int publishFormatFiles(
    const std::vector<std::pair<std::string, std::string>> &formats, const std::string &logPath, int timeoutMs,
    const std::string &origin
)
{
  X11ClipboardOffer offer;
  offer.origin = origin;
  std::uintmax_t total = 0;
  for (const auto &[mime, path] : formats) {
    if (!supportedMime(mime))
      throw std::invalid_argument("unsupported MIME: " + mime);
    if (!std::filesystem::is_regular_file(path))
      throw std::invalid_argument("format source must be a regular file: " + path);
    const auto size = std::filesystem::file_size(path);
    if (size > kX11MaxFormatBytes || size > kX11MaxOfferBytes - total)
      fail("TOO_LARGE", "format files exceed the 16 MiB/32 MiB budget");
    total += size;
    offer.formats.push_back({mime, [path, size] {
                               std::ifstream input(path, std::ios::binary);
                               if (!input)
                                 fail("IO_ERROR", "cannot open format source");
                               std::string data(static_cast<std::size_t>(size), '\0');
                               input.read(data.data(), static_cast<std::streamsize>(data.size()));
                               if (input.gcount() != static_cast<std::streamsize>(size) ||
                                   input.peek() != std::char_traits<char>::eof())
                                 fail("SOURCE_CHANGED", "format source size changed before the data request");
                               return data;
                             }});
  }
  return publishX11Offer(offer, logPath, timeoutMs);
}

int readClipboard(
    const std::string &target, const std::optional<std::string> &outputPath, int timeoutMs, const std::string &logPath
)
{
  Logger log(logPath, std::cerr);
  try {
    SignalGuard signals;
    const auto result = readX11Format(target, timeoutMs);
    writeOutput(result.bytes, outputPath);
    log.write(
        "read_complete", "\"target\":" + quote(target) + ",\"owner\":" + std::to_string(result.owner.window) +
                             ",\"bytes\":" + std::to_string(result.bytes.size()) +
                             ",\"incremental\":" + (result.incremental ? "true" : "false")
    );
    return 0;
  } catch (const std::exception &error) {
    return log.error(error);
  }
}

int watchX11Clipboard(
    const std::function<void(const X11Announcement &)> &onAnnouncement, bool once, const std::string &logPath,
    int timeoutMs, const std::string &requestedOrigin
)
{
  Logger log(logPath, std::cout);
  try {
    if (timeoutMs <= 0)
      throw std::invalid_argument("timeout must be positive");
    SignalGuard signals;
    XSession session;
    const Atom clipboard = session.atom("CLIPBOARD");
    session.watchSelection(clipboard);
    const auto origin = defaultOrigin(requestedOrigin);
    log.write("ready", "\"role\":\"watch\",\"display\":" + quote(DisplayString(session.display())));
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    bool inspect = true;
    while (!g_stop) {
      if (inspect) {
        inspect = false;
        XSync(session.display(), False);
        session.drain();
        const auto revision = session.revision();
        const Window owner = XGetSelectionOwner(session.display(), clipboard);
        const X11Owner identity{owner, static_cast<std::uint32_t>(session.selectionTimestamp())};
        log.write(
            "owner_changed", "\"owner\":" + std::to_string(owner) +
                                 ",\"selectionTimestamp\":" + std::to_string(identity.selectionTimestamp)
        );
        if (owner != None) {
          try {
            const auto marker = readProperty(session, owner, session.atom(kOriginProperty), false, 256);
            checkOwner(session, clipboard, owner, revision);
            if (marker.format == 8 && marker.bytes == origin) {
              log.write("self_offer_ignored", "\"owner\":" + std::to_string(owner));
            } else {
              const auto targetList = readX11Format("TARGETS", timeoutMs, identity);
              checkOwner(session, clipboard, owner, revision);
              X11Announcement announcement{targetList.owner, targetList.targets};
              log.write(
                  "clipboard_offer", "\"owner\":" + std::to_string(owner) + ",\"selectionTimestamp\":" +
                                         std::to_string(announcement.owner.selectionTimestamp) +
                                         ",\"targets\":" + stringArray(announcement.targets)
              );
              if (onAnnouncement)
                onAnnouncement(announcement);
              if (once)
                return 0;
            }
          } catch (const X11ClipboardError &error) {
            log.write(
                "announcement_skipped", "\"status\":" + quote(error.status()) + ",\"message\":" + quote(error.what())
            );
            if (error.status() == "OWNER_CHANGED")
              inspect = true;
          }
        }
      }
      if (once && Clock::now() >= deadline)
        fail("TIMEOUT", "no external clipboard offer before the watch deadline");
      XEvent event{};
      if (session.nextEvent(event, 100) && session.selectionEvent(event))
        inspect = true;
    }
    log.write("stopped", "\"reason\":\"signal\"");
    return 0;
  } catch (const std::exception &error) {
    return log.error(error);
  }
}

} // namespace deskflow::clipboard::prototype
