/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "FakeRemoteSource.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace clipboard_proto {

std::string jsonQuote(std::string_view value)
{
  std::ostringstream out;
  out << '"';
  for (const unsigned char ch : value) {
    switch (ch) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (ch < 0x20)
        out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(ch) << std::dec;
      else
        out << static_cast<char>(ch);
    }
  }
  out << '"';
  return out.str();
}

TraceLog::TraceLog(const std::string &path)
{
  if (!path.empty()) {
    m_stream.open(path, std::ios::out | std::ios::trunc);
    if (!m_stream)
      throw std::runtime_error("Cannot create trace log: " + path);
  }
}

void TraceLog::event(std::string_view operation, std::string_view fields)
{
  std::lock_guard lock(m_mutex);
  if (!m_stream.is_open())
    return;
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_start).count();
  const auto unixMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  m_stream << "{\"operation\":" << jsonQuote(operation) << ",\"offerId\":" << jsonQuote(FakeRemoteSource::offerId)
           << ",\"elapsedUs\":" << elapsed << ",\"unixMs\":" << unixMs;
  if (!fields.empty())
    m_stream << ',' << fields;
  m_stream << "}\n";
  m_stream.flush();
}

FakeRemoteSource::FakeRemoteSource(
    TraceLog &log, std::uint64_t largeSize, unsigned int delayMs, std::size_t sourceChunkSize
)
    : m_log(log),
      m_delayMs(delayMs),
      m_sourceChunkSize(sourceChunkSize)
{
  if (largeSize > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    throw std::invalid_argument("large-size exceeds a signed 64-bit file offset");
  if (sourceChunkSize == 0 || sourceChunkSize > maxRequestBytes)
    throw std::invalid_argument("source-chunk-size must be between 1 and 262144");
  const std::string hello = "Deskflow on-demand clipboard prototype.\nRead requests fetch only the requested range.\n";
  const std::string preview =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"64\" viewBox=\"0 0 64 64\">"
      "<rect width=\"64\" height=\"64\" rx=\"8\" fill=\"#2358a8\"/>"
      "<circle cx=\"32\" cy=\"32\" r=\"18\" fill=\"#f6d548\"/></svg>\n";
  m_entries = {
      {"root", "/", true, 0, 0, {}},
      {"hello", "/hello.txt", false, hello.size(), 0, hello},
      {"empty", "/empty.txt", false, 0, 0, {}},
      {"preview", "/preview.svg", false, preview.size(), 0, preview},
      {"nested", "/nested folder", true, 0, 0, {}},
      {"empty-dir", "/nested folder/empty dir", true, 0, 0, {}},
      {"unicode", "/nested folder/含 空格.txt", false, 12289, 17, {}},
      {"payload", "/nested folder/payload.bin", false, (std::uint64_t{4} << 20) + 37, 41, {}},
      {"large", "/large.bin", false, largeSize, 97, {}},
  };
}

const FileEntry *FakeRemoteSource::findPath(std::string_view path) const
{
  const auto found =
      std::find_if(m_entries.begin(), m_entries.end(), [&](const auto &entry) { return entry.path == path; });
  return found == m_entries.end() ? nullptr : &*found;
}

const FileEntry *FakeRemoteSource::findId(std::string_view id) const
{
  const auto found =
      std::find_if(m_entries.begin(), m_entries.end(), [&](const auto &entry) { return entry.id == id; });
  return found == m_entries.end() ? nullptr : &*found;
}

std::vector<const FileEntry *> FakeRemoteSource::children(std::string_view directory) const
{
  std::vector<const FileEntry *> result;
  const std::string prefix = directory == "/" ? "/" : std::string(directory) + '/';
  for (const auto &entry : m_entries) {
    if (entry.path.size() > prefix.size() && entry.path.starts_with(prefix) &&
        entry.path.find('/', prefix.size()) == std::string::npos)
      result.push_back(&entry);
  }
  return result;
}

unsigned char FakeRemoteSource::byteAt(const FileEntry &entry, std::uint64_t offset)
{
  if (!entry.literal.empty())
    return static_cast<unsigned char>(entry.literal.at(static_cast<std::size_t>(offset)));
  return static_cast<unsigned char>((offset % 251 + entry.seed) % 251);
}

std::size_t FakeRemoteSource::readRange(
    const FileEntry &entry, std::uint64_t offset, std::span<char> buffer, std::uint64_t handleId
)
{
  if (entry.directory)
    throw std::invalid_argument("Cannot read a directory");
  if (buffer.size() > maxRequestBytes)
    throw std::invalid_argument("Remote request exceeds 256 KiB budget");
  const auto requestId = m_nextRequest.fetch_add(1);
  const auto inFlight = m_inFlightBytes.fetch_add(buffer.size()) + buffer.size();
  const auto active = m_activeRequests.fetch_add(1) + 1;
  const auto start = std::chrono::steady_clock::now();
  const std::string fields = "\"fileId\":" + jsonQuote(entry.id) + ",\"handleId\":" + std::to_string(handleId) +
                             ",\"requestId\":" + std::to_string(requestId) + ",\"offset\":" + std::to_string(offset) +
                             ",\"length\":" + std::to_string(buffer.size()) +
                             ",\"inFlightBytes\":" + std::to_string(inFlight) +
                             ",\"activeRequests\":" + std::to_string(active);
  m_log.event("read_range_start", fields);
  if (m_delayMs != 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(m_delayMs));
  const auto count =
      offset >= entry.size
          ? std::size_t{0}
          : static_cast<std::size_t>(std::min<std::uint64_t>({buffer.size(), m_sourceChunkSize, entry.size - offset}));
  for (std::size_t index = 0; index < count; ++index)
    buffer[index] = static_cast<char>(byteAt(entry, offset + index));
  m_inFlightBytes.fetch_sub(buffer.size());
  m_activeRequests.fetch_sub(1);
  const auto duration =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
  m_log.event(
      "read_range", fields + ",\"returnedBytes\":" + std::to_string(count) +
                        ",\"durationUs\":" + std::to_string(duration) + ",\"status\":\"ok\""
  );
  return count;
}

void FakeRemoteSource::metadata(std::string_view action, std::string_view path, const FileEntry *entry)
{
  m_log.event(
      "metadata", "\"action\":" + jsonQuote(action) + ",\"path\":" + jsonQuote(path) + ",\"fileId\":" +
                      (entry ? jsonQuote(entry->id) : "null") + ",\"status\":" + jsonQuote(entry ? "ok" : "not_found")
  );
}

std::string FakeRemoteSource::describe() const
{
  std::ostringstream out;
  out << "{\"offerId\":" << jsonQuote(offerId) << ",\"maxRequestBytes\":" << maxRequestBytes
      << ",\"pattern\":\"(offset % 251 + seed) % 251\",\"files\":[";
  bool first = true;
  for (const auto &entry : m_entries) {
    if (!first)
      out << ',';
    first = false;
    out << "{\"id\":" << jsonQuote(entry.id) << ",\"path\":" << jsonQuote(entry.path.substr(1))
        << ",\"kind\":" << jsonQuote(entry.directory ? "directory" : "file") << ",\"size\":" << entry.size
        << ",\"seed\":" << entry.seed << ",\"literal\":" << jsonQuote(entry.literal) << '}';
  }
  out << "]}";
  return out.str();
}

} // namespace clipboard_proto
