/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "clipboard/FileService.h"
#include "clipboard/LocalFileSource.h"

#import <AppKit/AppKit.h>

#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace {

using namespace deskflow::clipboard;
constexpr std::size_t kMaxLineBytes = 1024 * 1024;
constexpr std::size_t kMaxInputPaths = 512;
constexpr std::size_t kMaxSelectionBytes = 64 * 1024;
constexpr std::size_t kMaxRequestCount = 65536;

struct Options
{
  std::string pasteboard;
  std::vector<std::filesystem::path> paths;
  bool detectable = false;
};

struct RequestError : std::runtime_error
{
  Status status;
  RequestError(Status status, const char *message) : std::runtime_error(message), status(status)
  {
  }
};

NSString *text(const std::string &value)
{
  NSString *result = [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
  if (!result || value.find('\0') != std::string::npos) {
    throw RequestError(Status::InvalidArgument, "invalid UTF-8 string");
  }
  return result;
}

std::string stringField(NSDictionary *request, NSString *key, bool required = true)
{
  id value = request[key];
  if (!value && !required) {
    return {};
  }
  if (![value isKindOfClass:NSString.class] || [value length] == 0 ||
      [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 256) {
    throw RequestError(Status::InvalidArgument, "missing or invalid string field");
  }
  NSData *bytes = [value dataUsingEncoding:NSUTF8StringEncoding];
  if (!bytes) {
    throw RequestError(Status::InvalidArgument, "invalid UTF-8 field");
  }
  std::string result(static_cast<const char *>(bytes.bytes), bytes.length);
  if (result.find('\0') != std::string::npos) {
    throw RequestError(Status::InvalidArgument, "embedded NUL in string field");
  }
  return result;
}

std::uint32_t integerField(NSDictionary *request, NSString *key, std::uint32_t fallback = 0, bool required = true)
{
  id value = request[key];
  if (!value && !required) {
    return fallback;
  }
  if (![value isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID()) {
    throw RequestError(Status::InvalidArgument, "missing or invalid integer field");
  }
  const double number = [value doubleValue];
  if (!std::isfinite(number) || number < 0 || number > std::numeric_limits<std::uint32_t>::max() ||
      std::floor(number) != number) {
    throw RequestError(Status::InvalidArgument, "integer field outside uint32 range");
  }
  return static_cast<std::uint32_t>(number);
}

std::uint64_t offsetField(NSDictionary *request)
{
  const auto string = stringField(request, @"offset");
  std::uint64_t result = 0;
  const auto converted = std::from_chars(string.data(), string.data() + string.size(), result);
  if (converted.ec != std::errc{} || converted.ptr != string.data() + string.size()) {
    throw RequestError(Status::InvalidArgument, "offset must be a uint64 decimal string");
  }
  return result;
}

void writeJson(NSDictionary *record)
{
  NSError *error = nil;
  NSData *data = [NSJSONSerialization dataWithJSONObject:record options:NSJSONWritingSortedKeys error:&error];
  if (!data) {
    throw std::runtime_error("cannot encode response");
  }
  std::cout.write(static_cast<const char *>(data.bytes), data.length);
  std::cout << '\n';
  std::cout.flush();
  if (!std::cout) {
    throw std::runtime_error("response pipe closed");
  }
}

NSArray *entryRecords(const std::vector<Entry> &entries)
{
  NSMutableArray *result = [NSMutableArray arrayWithCapacity:entries.size()];
  for (const auto &entry : entries) {
    [result addObject:@{
      @"fileId" : text(entry.id),
      @"parentId" : text(entry.parentId),
      @"name" : text(entry.name),
      @"version" : text(entry.version),
      @"directory" : @(entry.directory),
      @"size" : text(std::to_string(entry.size)),
    }];
  }
  return result;
}

NSString *leaseState(LeaseState state)
{
  switch (state) {
  case LeaseState::Active:
    return @"ACTIVE";
  case LeaseState::Draining:
    return @"DRAINING";
  case LeaseState::Closed:
    return @"CLOSED";
  case LeaseState::Expired:
    return @"EXPIRED";
  case LeaseState::Revoked:
    return @"REVOKED";
  }
  return @"REVOKED";
}

struct Selection
{
  std::vector<std::filesystem::path> paths;
  __strong NSPasteboard *board = nil;
  NSInteger generation = 0;

  void validateGeneration() const
  {
    if (board && board.changeCount != generation) {
      throw RequestError(Status::SourceChanged, "pasteboard changed during capture");
    }
  }
};

Selection selectedPaths(const Options &options)
{
  if (!options.paths.empty()) {
    return {.paths = options.paths};
  }
  NSPasteboard *board = options.pasteboard == "general" ? NSPasteboard.generalPasteboard
                                                        : [NSPasteboard pasteboardWithName:text(options.pasteboard)];
  Selection selection{.board = board, .generation = board.changeCount};
  NSArray<NSPasteboardItem *> *items = board.pasteboardItems;
  if (items.count == 0 || items.count > kMaxInputPaths) {
    throw RequestError(Status::InvalidArgument, "pasteboard must contain 1 to 512 file items");
  }
  std::size_t metadataBytes = 0;
  for (NSPasteboardItem *item in items) {
    NSString *value = [item stringForType:NSPasteboardTypeFileURL];
    NSURL *url = value ? [NSURL URLWithString:value] : nil;
    if (!url.isFileURL || url.user || url.password || url.port || url.query || url.fragment ||
        (url.host.length != 0 && ![url.host isEqualToString:@"localhost"])) {
      throw RequestError(Status::InvalidArgument, "selection includes a nonlocal or invalid file URL");
    }
    metadataBytes += [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    if (metadataBytes > kMaxSelectionBytes) {
      throw RequestError(Status::ResourceLimit, "selection metadata exceeds 64 KiB");
    }
    NSData *pathBytes = [url.path dataUsingEncoding:NSUTF8StringEncoding];
    if (!pathBytes || std::memchr(pathBytes.bytes, '\0', pathBytes.length)) {
      throw RequestError(Status::InvalidArgument, "file URL contains an invalid path");
    }
    const char *path = url.fileSystemRepresentation;
    if (!path || path[0] != '/') {
      throw RequestError(Status::InvalidArgument, "file URL has no absolute local path");
    }
    selection.paths.emplace_back(path);
  }
  selection.validateGeneration();
  return selection;
}

void validateCaptureContext(const RequestContext *context)
{
  if (context && context->stop.stop_requested()) {
    throw RequestError(Status::Cancelled, "capture cancelled");
  }
  if (context && Clock::now() >= context->deadline) {
    throw RequestError(Status::Timeout, "capture deadline expired");
  }
}

NSDictionary *capture(FileService &service, const Options &options, const RequestContext *context = nullptr)
{
  validateCaptureContext(context);
  const auto selection = selectedPaths(options);
  validateCaptureContext(context);
  auto result =
      LocalFileSource::capture(selection.paths, NSUUID.UUID.UUIDString.UTF8String, NSUUID.UUID.UUIDString.UTF8String);
  // Synchronous filesystem calls are not forcibly interruptible here. Their late
  // result still must not be published or reported as a successful request.
  validateCaptureContext(context);
  // URL reads and recursive filesystem capture are separate steps. An A -> B
  // replacement during either step must not announce the earlier selection.
  selection.validateGeneration();
  if (result.status != Status::Ok) {
    throw RequestError(result.status, "local file selection capture failed");
  }
  // Serialize before publication: unsupported filename encodings must fail before state changes.
  NSDictionary *manifest = @{
    @"offerId" : text(result.offer.id),
    @"offerVersion" : text(result.offer.version),
    @"entries" : entryRecords(result.offer.entries),
    @"consistency" : @"detectable",
    @"immutableVersion" : @NO,
    @"sourceChangeCount" : selection.board ? @(selection.generation) : (id)NSNull.null,
  };
  selection.validateGeneration();
  validateCaptureContext(context);
  const auto status = service.publishOffer(std::move(result.offer));
  if (status != Status::Ok) {
    throw RequestError(status, "offer publication failed");
  }
  // There is no atomic transaction between the system pasteboard and FileService.
  // If publication races a replacement, withhold the manifest. Its unknown offer
  // ID cannot be acquired by the client; normal service offer budgets still apply.
  // A later replacement after this final check is a subsequent clipboard event,
  // not a change to the captured generation or its pinned source references.
  selection.validateGeneration();
  validateCaptureContext(context);
  return manifest;
}

NSMutableDictionary *responseEnvelope(NSDictionary *request)
{
  NSMutableDictionary *response = [@{
    @"schemaVersion" : @1,
    @"status" : @"INVALID_ARGUMENT",
    @"operation" : NSNull.null,
    @"sessionEpoch" : NSNull.null,
    @"requestId" : NSNull.null,
  } mutableCopy];
  for (NSString *key in @[
         @"operation", @"sessionEpoch", @"requestId", @"leaseId", @"handleId", @"offerId", @"fileId", @"version",
         @"offerVersion", @"offset"
       ]) {
    id value = request[key];
    if ([value isKindOfClass:NSString.class] && [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] <= 256) {
      response[key] = value;
    }
  }
  if ([request[@"operation"] isEqual:@"read"]) {
    response[@"bytesBase64"] = @"";
    response[@"returnedBytes"] = @0;
    response[@"eof"] = @NO;
    id length = request[@"length"];
    if ([length isKindOfClass:NSNumber.class]) {
      response[@"length"] = length;
    }
  }
  return response;
}

void leaseFields(NSMutableDictionary *response, const LeaseResult &result)
{
  response[@"status"] = @(statusName(result.status));
  response[@"leaseId"] = text(result.leaseId);
  response[@"offerVersion"] = text(result.offerVersion);
  response[@"grantedTtlMs"] = @(result.grantedTtl.count());
  response[@"state"] = leaseState(result.state);
}

void execute(
    FileService &service, const Options &options, const std::string &sessionEpoch, NSDictionary *request,
    NSMutableDictionary *response, std::unordered_set<std::string> &requests, bool &connected
)
{
  const auto operation = stringField(request, @"operation");
  RequestContext context{
      .sessionEpoch = stringField(request, @"sessionEpoch"), .requestId = stringField(request, @"requestId")
  };
  if (context.sessionEpoch != sessionEpoch) {
    throw RequestError(Status::StaleSession, "stale session");
  }
  if (!connected) {
    throw RequestError(Status::Disconnected, "session disconnected");
  }
  if (requests.size() >= kMaxRequestCount) {
    throw RequestError(Status::ResourceLimit, "request budget exhausted");
  }
  if (!requests.insert(context.requestId).second) {
    throw RequestError(Status::InvalidArgument, "duplicate requestId");
  }
  const auto timeoutMs = integerField(request, @"timeoutMs", 30000, false);
  if (timeoutMs > 600000) {
    throw RequestError(Status::InvalidArgument, "timeoutMs exceeds 600000");
  }
  context.deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  std::stop_source cancellation;
  id cancelled = request[@"cancelled"];
  if (cancelled) {
    if (CFGetTypeID((__bridge CFTypeRef)cancelled) != CFBooleanGetTypeID()) {
      throw RequestError(Status::InvalidArgument, "cancelled must be boolean");
    }
    if ([cancelled boolValue]) {
      cancellation.request_stop();
    }
  }
  context.stop = cancellation.get_token();

  if (operation == "acquire") {
    leaseFields(
        response, service.acquireLease(
                      context, stringField(request, @"offerId"), stringField(request, @"offerVersion"),
                      std::chrono::milliseconds(integerField(request, @"ttlMs", 30000, false))
                  )
    );
  } else if (operation == "renew") {
    leaseFields(
        response, service.renewLease(
                      context, stringField(request, @"leaseId"),
                      std::chrono::milliseconds(integerField(request, @"ttlMs", 30000, false))
                  )
    );
  } else if (operation == "release") {
    leaseFields(response, service.releaseLease(context, stringField(request, @"leaseId")));
  } else if (operation == "list") {
    const auto result = service.listDirectory(
        context, stringField(request, @"leaseId"), stringField(request, @"fileId"), stringField(request, @"version"),
        integerField(request, @"cursor", 0, false), integerField(request, @"pageSize", 256, false)
    );
    response[@"status"] = @(statusName(result.status));
    response[@"entries"] = entryRecords(result.entries);
    response[@"nextCursor"] = @(result.nextCursor);
  } else if (operation == "open") {
    const auto consistency = stringField(request, @"consistency", false);
    if (!consistency.empty() && consistency != "detectable" && consistency != "immutable") {
      throw RequestError(Status::InvalidArgument, "unsupported consistency mode");
    }
    const auto result = service.open(
        context, stringField(request, @"leaseId"), stringField(request, @"fileId"), stringField(request, @"version"),
        consistency == "immutable" ? Consistency::Immutable : Consistency::Detectable
    );
    response[@"status"] = @(statusName(result.status));
    response[@"handleId"] = text(result.handleId);
    response[@"version"] = text(result.version);
    response[@"size"] = text(std::to_string(result.size));
  } else if (operation == "read") {
    const auto result = service.read(
        context, stringField(request, @"leaseId"), stringField(request, @"handleId"), offsetField(request),
        integerField(request, @"length")
    );
    response[@"status"] = @(statusName(result.status));
    response[@"sessionEpoch"] = text(result.sessionEpoch);
    response[@"requestId"] = text(result.requestId);
    response[@"offerId"] = text(result.offerId);
    response[@"fileId"] = text(result.fileId);
    response[@"version"] = text(result.version);
    response[@"handleId"] = text(result.handleId);
    response[@"offset"] = text(std::to_string(result.offset));
    response[@"length"] = @(result.length);
    response[@"returnedBytes"] = @(result.bytes.size());
    response[@"bytesBase64"] = [[NSData dataWithBytes:result.bytes.data()
                                               length:result.bytes.size()] base64EncodedStringWithOptions:0];
    response[@"eof"] = @(result.eof);
  } else if (operation == "close") {
    response[@"status"] = @(statusName(service.close(context, stringField(request, @"handleId"))));
  } else if (operation == "capture" || operation == "disconnect") {
    // These harness operations do not use FileService's RequestContext entry points.
    if (context.stop.stop_requested()) {
      throw RequestError(Status::Cancelled, "request cancelled");
    }
    if (Clock::now() >= context.deadline) {
      throw RequestError(Status::Timeout, "request deadline expired");
    }
    if (operation == "capture") {
      [response addEntriesFromDictionary:capture(service, options, &context)];
    } else {
      service.disconnect();
      connected = false;
    }
    response[@"status"] = @"OK";
  } else {
    throw RequestError(Status::InvalidArgument, "unknown operation");
  }
}

// Avoid getline's unbounded allocation for malformed clients. Drain overlong
// records so the next complete request can still be processed independently.
bool readLine(std::string &line, bool &overlong)
{
  line.clear();
  overlong = false;
  bool any = false;
  char byte;
  while (std::cin.get(byte)) {
    any = true;
    if (byte == '\n') {
      break;
    }
    if (line.size() < kMaxLineBytes) {
      line.push_back(byte);
    } else {
      overlong = true;
    }
  }
  return any;
}

void help()
{
  std::cout << "Usage: deskflow-mac-file-service --consistency detectable\n"
               "         (--pasteboard NAME | --path ABSOLUTE_PATH [--path ...])\n\n"
               "Explicit --pasteboard general reads the system clipboard. Use unique\n"
               "named pasteboards for tests. Captures local file URLs and directory\n"
               "membership, pins file descriptors, and detects ordinary mutations.\n"
               "It does not provide immutable versions or create complete file copies.\n\n"
               "Emits a ready JSON manifest; reads bounded JSONL requests on stdin and\n"
               "writes JSONL responses on stdout. Every request needs operation,\n"
               "sessionEpoch, requestId. Operations: acquire, renew, release, list,\n"
               "open, read, close, capture (refresh configured input), disconnect.\n"
               "64-bit size/offset are decimal strings; read bytes use bytesBase64.\n"
               "A request may set timeoutMs or cancelled for boundary tests.\n"
               "Requests are serialized; cancellation of an in-progress request and\n"
               "network transport/authentication are not implemented by this harness.\n";
}

Options parseOptions(int argc, char **argv)
{
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (++i >= argc) {
      throw std::invalid_argument("option is missing a value");
    }
    const std::string value = argv[i];
    if (argument == "--pasteboard" && options.pasteboard.empty() && !value.empty()) {
      options.pasteboard = value;
    } else if (argument == "--path") {
      const std::filesystem::path path(value);
      if (!path.is_absolute() || options.paths.size() >= kMaxInputPaths) {
        throw std::invalid_argument("--path requires at most 512 absolute paths");
      }
      options.paths.push_back(path);
    } else if (argument == "--consistency" && value == "detectable") {
      options.detectable = true;
    } else {
      throw std::invalid_argument("unknown or invalid option");
    }
  }
  if (!options.detectable || options.pasteboard.empty() == options.paths.empty()) {
    throw std::invalid_argument("require --consistency detectable and exactly one of --pasteboard or --path");
  }
  return options;
}

int run(int argc, char **argv)
{
  if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
    help();
    return 0;
  }
  const auto options = parseOptions(argc, argv);
  const std::string sessionEpoch = NSUUID.UUID.UUIDString.UTF8String;
  Limits limits;
  // A local probe opens descriptors eagerly for a stable directory inventory.
  // Keep its retained announcement/descriptor budgets smaller than production defaults.
  limits.maxOffers = 8;
  limits.maxEntries = 2048;
  limits.maxActiveLeases = 32;
  limits.maxActiveHandles = 128;
  FileService service(sessionEpoch, limits);
  NSMutableDictionary *ready = [capture(service, options) mutableCopy];
  ready[@"event"] = @"ready";
  ready[@"schemaVersion"] = @1;
  ready[@"sessionEpoch"] = text(sessionEpoch);
  ready[@"status"] = @"OK";
  ready[@"maxRequestBytes"] = @(limits.maxRequestBytes);
  writeJson(ready);

  std::unordered_set<std::string> requests;
  bool connected = true;
  std::string line;
  bool overlong = false;
  while (readLine(line, overlong)) {
    @autoreleasepool {
      NSDictionary *request = nil;
      NSMutableDictionary *response = responseEnvelope(nil);
      try {
        if (overlong) {
          throw RequestError(Status::ResourceLimit, "JSONL record exceeds 1 MiB");
        }
        NSData *data = [NSData dataWithBytes:line.data() length:line.size()];
        id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        if (![parsed isKindOfClass:NSDictionary.class]) {
          throw RequestError(Status::InvalidArgument, "request must be a JSON object");
        }
        request = parsed;
        response = responseEnvelope(request);
        execute(service, options, sessionEpoch, request, response, requests, connected);
      } catch (const RequestError &error) {
        response[@"status"] = @(statusName(error.status));
      } catch (const std::bad_alloc &) {
        response[@"status"] = @"RESOURCE_LIMIT";
      } catch (const std::exception &) {
        response[@"status"] = @"IO_ERROR";
      }
      writeJson(response);
    }
  }
  service.disconnect();
  return 0;
}

} // namespace

int main(int argc, char **argv)
{
  @autoreleasepool {
    @try {
      try {
        return run(argc, argv);
      } catch (const RequestError &error) {
        std::cerr << "deskflow-mac-file-service: " << statusName(error.status) << ": " << error.what() << '\n';
      } catch (const std::exception &error) {
        std::cerr << "deskflow-mac-file-service: " << error.what() << '\n';
      }
    } @catch (NSException *exception) {
      std::cerr << "deskflow-mac-file-service: Foundation exception: " << exception.name.UTF8String << '\n';
    }
    return 2;
  }
}
