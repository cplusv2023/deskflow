/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MacClipboard.h"

#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace deskflow::clipboard::prototype {
namespace {

constexpr std::size_t kMaxRepresentationBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxFileListBytes = 64 * 1024;
constexpr std::size_t kMaxItems = 1024;
constexpr std::size_t kMaxRtfBytes = 1024 * 1024;
constexpr std::size_t kMaxImagePixels = 4 * 1024 * 1024;
constexpr std::size_t kMaxImageDimension = 8192;
constexpr std::size_t kMaxDecodedImageBytes = 64 * 1024 * 1024;
NSString *const kOriginType = @"org.deskflow.clipboard.prototype.origin";
volatile std::sig_atomic_t g_stop = 0;

void stopHandler(int)
{
  g_stop = 1;
}

class SignalGuard
{
public:
  SignalGuard() : m_int(std::signal(SIGINT, stopHandler)), m_term(std::signal(SIGTERM, stopHandler))
  {
    g_stop = 0;
  }
  ~SignalGuard()
  {
    std::signal(SIGINT, m_int);
    std::signal(SIGTERM, m_term);
  }

private:
  using Handler = void (*)(int);
  Handler m_int;
  Handler m_term;
};

NSString *nativeString(const std::string &value)
{
  NSString *result = [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
  if (!result || value.find('\0') != std::string::npos) {
    throw std::invalid_argument("arguments must be UTF-8 without embedded NUL bytes");
  }
  return result;
}

std::string json(NSDictionary *value)
{
  NSError *error = nil;
  NSData *data = [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingSortedKeys error:&error];
  if (!data) {
    throw std::runtime_error("cannot encode JSON metadata");
  }
  return std::string(static_cast<const char *>(data.bytes), data.length);
}

class Logger
{
public:
  Logger(const std::string &path, std::ostream &fallback) : m_stream(&fallback)
  {
    if (!path.empty()) {
      m_file.open(path, std::ios::app);
      if (!m_file) {
        throw std::runtime_error("cannot open JSONL log");
      }
      m_stream = &m_file;
    }
  }

  void write(NSString *event, NSDictionary *fields)
  {
    NSMutableDictionary *record = [fields mutableCopy];
    record[@"event"] = event;
    record[@"side"] = @"macos-prototype";
    record[@"schemaVersion"] = @1;
    record[@"elapsedUs"] =
        @(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_start).count());
    *m_stream << json(record) << '\n';
    m_stream->flush();
    if (!*m_stream) {
      throw std::runtime_error("cannot write JSONL log");
    }
  }

private:
  std::ofstream m_file;
  std::ostream *m_stream;
  std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

NSPasteboard *pasteboard(const MacClipboardOptions &options)
{
  return options.pasteboard.empty() ? NSPasteboard.generalPasteboard
                                    : [NSPasteboard pasteboardWithName:nativeString(options.pasteboard)];
}

NSPasteboardType typeForTarget(const std::string &target)
{
  if (target == "text/plain" || target == "text/plain;charset=utf-8" || target == "UTF8_STRING") {
    return NSPasteboardTypeString;
  }
  if (target == "text/html") {
    return NSPasteboardTypeHTML;
  }
  if (target == "image/png") {
    return NSPasteboardTypePNG;
  }
  throw std::invalid_argument("unsupported representation; use text/plain, text/html, or image/png");
}

// Native clipboard adapters deliberately support bounded text/formatting RTF,
// not RTFD, attachments, OLE objects, external fields, or embedded image payloads.
void validateTextRtf(NSData *data)
{
  if (data.length > kMaxRtfBytes) {
    throw std::runtime_error("RTF fallback exceeds 1 MiB input limit");
  }
  const std::string_view input(static_cast<const char *>(data.bytes), data.length);
  if (!input.starts_with("{\\rtf")) {
    throw std::runtime_error("RTF fallback has an invalid header");
  }
  int depth = 0;
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '{') {
      if (++depth > 128) {
        throw std::runtime_error("RTF fallback exceeds nesting limit");
      }
    } else if (input[i] == '}') {
      if (--depth < 0) {
        throw std::runtime_error("RTF fallback has unbalanced groups");
      }
    } else if (input[i] == '\\') {
      if (++i == input.size()) {
        throw std::runtime_error("RTF fallback has an incomplete escape");
      }
      const auto start = i;
      while (i < input.size() && std::isalpha(static_cast<unsigned char>(input[i]))) {
        ++i;
      }
      if (i == start) {
        continue; // Escaped braces/backslashes and control symbols are not groups.
      }
      std::string word(input.substr(start, i - start));
      for (char &byte : word) {
        byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
      }
      if (word == "bin" || word == "field" || word == "fldinst" || word == "file" || word == "filetbl" ||
          word == "fontemb" || word == "fontfile" || word == "nextgraphic" || word == "listpicture" ||
          word == "datastore" || word == "themedata" || word == "htmltag" || word == "mhtmltag" ||
          word.starts_with("obj") || word.starts_with("pic") || word.starts_with("shp") || word == "nonshppict") {
        throw std::runtime_error("RTF fallback supports text and formatting only; embedded content is unsupported");
      }
      --i; // Revisit the first byte following this control word.
    }
  }
  if (depth != 0) {
    throw std::runtime_error("RTF fallback has unbalanced groups");
  }
}

NSData *htmlFromRtf(NSData *data)
{
  validateTextRtf(data);
  NSAttributedString *document = [[NSAttributedString alloc] initWithRTF:data documentAttributes:nil];
  if (!document || document.length > kMaxRtfBytes) {
    throw std::runtime_error("RTF fallback could not decode bounded text");
  }
  __block bool attachment = false;
  [document enumerateAttribute:NSAttachmentAttributeName
                       inRange:NSMakeRange(0, document.length)
                       options:0
                    usingBlock:^(id value, NSRange, BOOL *stop) {
                      if (value) {
                        attachment = true;
                        *stop = YES;
                      }
                    }];
  if (attachment) {
    throw std::runtime_error("RTF fallback contains unsupported attachments");
  }
  NSError *error = nil;
  NSData *result = [document dataFromRange:NSMakeRange(0, document.length)
                        documentAttributes:@{
                          NSDocumentTypeDocumentAttribute : NSHTMLTextDocumentType,
                          NSCharacterEncodingDocumentAttribute : @(NSUTF8StringEncoding)
                        }
                                     error:&error];
  if (!result || result.length > kMaxRepresentationBytes) {
    throw std::runtime_error("RTF fallback could not produce bounded HTML");
  }
  return result;
}

template <typename T> struct OwnedCF
{
  T value;
  explicit OwnedCF(T value) : value(value)
  {
  }
  ~OwnedCF()
  {
    if (value) {
      CFRelease(value);
    }
  }
  OwnedCF(const OwnedCF &) = delete;
  OwnedCF &operator=(const OwnedCF &) = delete;
};

std::size_t imageInteger(NSDictionary *properties, CFStringRef key)
{
  id value = properties[(__bridge NSString *)key];
  const double number = [value isKindOfClass:NSNumber.class] ? [value doubleValue] : 0;
  if (!std::isfinite(number) || number <= 0 || std::floor(number) != number || number > kMaxImageDimension) {
    throw std::runtime_error("TIFF fallback has invalid or excessive image dimensions/depth");
  }
  return static_cast<std::size_t>(number);
}

NSData *pngFromTiff(NSData *data)
{
  if (data.length > kMaxRepresentationBytes) {
    throw std::runtime_error("TIFF fallback exceeds 16 MiB input limit");
  }
  NSDictionary *noDecode = @{(__bridge NSString *)kCGImageSourceShouldCache : @NO};
  OwnedCF source(CGImageSourceCreateWithData((__bridge CFDataRef)data, (__bridge CFDictionaryRef)noDecode));
  if (!source.value || !CGImageSourceGetType(source.value) ||
      !CFEqual(CGImageSourceGetType(source.value), CFSTR("public.tiff")) || CGImageSourceGetCount(source.value) != 1) {
    throw std::runtime_error("TIFF fallback requires one valid TIFF image");
  }
  NSDictionary *properties =
      CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source.value, 0, (__bridge CFDictionaryRef)noDecode));
  const auto width = imageInteger(properties, kCGImagePropertyPixelWidth);
  const auto height = imageInteger(properties, kCGImagePropertyPixelHeight);
  const auto depth = imageInteger(properties, kCGImagePropertyDepth);
  if (width > kMaxImagePixels / height || depth > 16) {
    throw std::runtime_error("TIFF fallback exceeds 4 Mi-pixel or 16-bit component budget");
  }
  // Inspect dimensions before requesting any bitmap. Native ImageIO may allocate
  // its own working buffers; the limits bound admitted images, not its allocator.
  NSDictionary *decode = @{
    (__bridge NSString *)kCGImageSourceShouldCache : @NO,
    (__bridge NSString *)kCGImageSourceShouldAllowFloat : @NO,
    (__bridge NSString *)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
    (__bridge NSString *)kCGImageSourceCreateThumbnailWithTransform : @YES,
    (__bridge NSString *)kCGImageSourceThumbnailMaxPixelSize : @(std::max(width, height)),
  };
  OwnedCF bitmap(CGImageSourceCreateThumbnailAtIndex(source.value, 0, (__bridge CFDictionaryRef)decode));
  if (!bitmap.value) {
    throw std::runtime_error("TIFF fallback could not decode image");
  }
  const auto actualWidth = CGImageGetWidth(bitmap.value);
  const auto actualHeight = CGImageGetHeight(bitmap.value);
  if (!actualWidth || !actualHeight || actualWidth > kMaxImageDimension || actualHeight > kMaxImageDimension ||
      actualWidth > kMaxImagePixels / actualHeight || CGImageGetBitsPerPixel(bitmap.value) > 128 ||
      CGImageGetBytesPerRow(bitmap.value) > kMaxDecodedImageBytes / actualHeight ||
      !((actualWidth == width && actualHeight == height) || (actualWidth == height && actualHeight == width))) {
    throw std::runtime_error("TIFF fallback decoded dimensions exceed admitted image budget");
  }
  NSMutableData *result = [NSMutableData data];
  OwnedCF destination(
      CGImageDestinationCreateWithData((__bridge CFMutableDataRef)result, CFSTR("public.png"), 1, nullptr)
  );
  if (!destination.value) {
    throw std::runtime_error("TIFF fallback could not create PNG encoder");
  }
  CGImageDestinationAddImage(destination.value, bitmap.value, nullptr);
  if (!CGImageDestinationFinalize(destination.value) || result.length > kMaxRepresentationBytes) {
    throw std::runtime_error("TIFF fallback could not produce bounded PNG");
  }
  return result;
}

NSString *publisherOrigin(const MacClipboardOptions &options)
{
  return options.origin.empty() ? NSUUID.UUID.UUIDString : nativeString(options.origin);
}

NSDictionary *metadata(NSPasteboard *board, const MacClipboardOptions &options)
{
  const NSInteger before = board.changeCount;
  NSArray<NSPasteboardItem *> *items = board.pasteboardItems;
  if (items.count > kMaxItems) {
    throw std::runtime_error("pasteboard exceeds metadata item limit");
  }
  NSMutableArray *result = [NSMutableArray array];
  for (NSPasteboardItem *item in items) {
    // Type inspection does not request promised body data or resolve file URLs.
    [result addObject:@{@"types" : item.types ? item.types : @[]}];
  }
  NSString *origin = nil;
  if ([items.firstObject.types containsObject:kOriginType]) {
    origin = [items.firstObject stringForType:kOriginType];
  }
  const NSInteger after = board.changeCount;
  return @{
    @"changeCount" : @(after),
    @"stable" : @(before == after),
    @"itemCount" : @(items.count),
    @"items" : result,
    @"origin" : origin ? origin : (id)NSNull.null,
    @"selfOrigin" : @(!options.origin.empty() && [origin isEqualToString:nativeString(options.origin)]),
  };
}

void pump(int milliseconds)
{
  [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:milliseconds / 1000.0]];
}

int holdPublication(NSPasteboard *board, NSString *origin, const MacClipboardOptions &options, Logger &logger)
{
  SignalGuard signals;
  const NSInteger ownedCount = board.changeCount;
  const auto start = std::chrono::steady_clock::now();
  logger.write(
      @"published",
      @{@"changeCount" : @(ownedCount),
        @"origin" : origin,
        @"itemCount" : @(board.pasteboardItems.count)}
  );
  while (!g_stop) {
    @autoreleasepool {
      if (board.changeCount != ownedCount) {
        logger.write(@"ownershipLost", @{@"changeCount" : @(board.changeCount)});
        return 0;
      }
      if (options.durationMs > 0 &&
          std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(options.durationMs)) {
        break;
      }
      pump(options.pollMs);
    }
  }
  logger.write(@"stopped", @{@"changeCount" : @(board.changeCount)});
  // URL metadata remains valid; unresolved representation promises require this process.
  return 0;
}

struct RepresentationSource
{
  int fd = -1;
  struct stat version{};
  std::string target;
  __strong NSString *type = nil;

  RepresentationSource(const MacClipboardRepresentation &input)
      : target(input.target),
        type(typeForTarget(input.target))
  {
    fd = open(input.inputPath.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
      throw std::runtime_error("cannot open representation input: " + std::string(std::strerror(errno)));
    }
    if (fstat(fd, &version) != 0 || !S_ISREG(version.st_mode) || version.st_size < 0 ||
        static_cast<std::uint64_t>(version.st_size) > kMaxRepresentationBytes) {
      close(fd);
      fd = -1;
      throw std::invalid_argument("representation input must be a regular file of at most 16 MiB");
    }
  }
  RepresentationSource(const RepresentationSource &) = delete;
  RepresentationSource &operator=(const RepresentationSource &) = delete;
  RepresentationSource(RepresentationSource &&other) noexcept
      : fd(std::exchange(other.fd, -1)),
        version(other.version),
        target(std::move(other.target)),
        type(other.type)
  {
  }
  ~RepresentationSource()
  {
    if (fd >= 0) {
      close(fd);
    }
  }
  bool unchanged() const
  {
    struct stat current{};
    return fstat(fd, &current) == 0 && current.st_size == version.st_size &&
           current.st_mtimespec.tv_sec == version.st_mtimespec.tv_sec &&
           current.st_mtimespec.tv_nsec == version.st_mtimespec.tv_nsec &&
           current.st_ctimespec.tv_sec == version.st_ctimespec.tv_sec &&
           current.st_ctimespec.tv_nsec == version.st_ctimespec.tv_nsec;
  }
};

} // namespace
} // namespace deskflow::clipboard::prototype

using deskflow::clipboard::prototype::Logger;
using deskflow::clipboard::prototype::RepresentationSource;

@interface DeskflowPrototypeDataProvider : NSObject <NSPasteboardItemDataProvider> {
  std::vector<RepresentationSource> _sources;
  std::shared_ptr<Logger> _logger;
}
- (instancetype)initWithSources:(std::vector<RepresentationSource>)sources logger:(std::shared_ptr<Logger>)logger;
@end

@implementation DeskflowPrototypeDataProvider
- (instancetype)initWithSources:(std::vector<RepresentationSource>)sources logger:(std::shared_ptr<Logger>)logger
{
  self = [super init];
  if (self) {
    _sources = std::move(sources);
    _logger = std::move(logger);
  }
  return self;
}

- (void)pasteboard:(NSPasteboard *)board item:(NSPasteboardItem *)item provideDataForType:(NSPasteboardType)type
{
  (void)board;
  // C++ exceptions must not unwind through AppKit's data-provider callback.
  try {
    for (const auto &source : _sources) {
      if (![source.type isEqualToString:type]) {
        continue;
      }
      _logger->write(@"provideDataRequested", @{@"target" : @(source.target.c_str())});
      if (!source.unchanged()) {
        _logger->write(@"provideData", @{@"status" : @"SOURCE_CHANGED", @"returnedBytes" : @0});
        return;
      }
      NSMutableData *data = [NSMutableData dataWithLength:static_cast<NSUInteger>(source.version.st_size)];
      if (!data) {
        throw std::runtime_error("representation allocation failed");
      }
      std::size_t done = 0;
      while (done < data.length) {
        const auto count = pread(source.fd, static_cast<char *>(data.mutableBytes) + done, data.length - done, done);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          throw std::runtime_error("representation input read failed");
        }
        done += static_cast<std::size_t>(count);
      }
      if (!source.unchanged()) {
        _logger->write(@"provideData", @{@"status" : @"SOURCE_CHANGED", @"returnedBytes" : @0});
        return;
      }
      if (![item setData:data forType:type]) {
        throw std::runtime_error("AppKit rejected representation data");
      }
      _logger->write(
          @"provideData", @{@"status" : @"OK", @"returnedBytes" : @(done), @"target" : @(source.target.c_str())}
      );
      return;
    }
  } catch (const std::exception &error) {
    std::cerr << "deskflow-clipboard-probe: data provider failed: " << error.what() << '\n';
  }
}
@end

namespace deskflow::clipboard::prototype {

int publishMacFiles(const std::vector<std::string> &paths, const MacClipboardOptions &options)
{
  @autoreleasepool {
    if (paths.empty() || paths.size() > kMaxItems) {
      throw std::invalid_argument("publish requires 1 to 1024 absolute file paths");
    }
    NSString *origin = publisherOrigin(options);
    NSMutableArray<NSPasteboardItem *> *items = [NSMutableArray arrayWithCapacity:paths.size()];
    std::size_t totalBytes = 0;
    for (const auto &path : paths) {
      NSString *native = nativeString(path);
      if (![native isAbsolutePath]) {
        throw std::invalid_argument("publish requires absolute paths");
      }
      // Explicit isDirectory avoids the filesystem lookup of the convenience initializer.
      NSURL *url = [NSURL fileURLWithPath:native isDirectory:[native hasSuffix:@"/"]];
      totalBytes += [url.absoluteString lengthOfBytesUsingEncoding:NSUTF8StringEncoding] + 2;
      if (totalBytes > kMaxFileListBytes) {
        throw std::invalid_argument("file URL metadata exceeds 64 KiB");
      }
      NSPasteboardItem *item = [[NSPasteboardItem alloc] init];
      if (![item setString:url.absoluteString forType:NSPasteboardTypeFileURL] || ![item setString:origin
                                                                                           forType:kOriginType]) {
        throw std::runtime_error("cannot prepare file URL and origin metadata");
      }
      [items addObject:item];
    }
    Logger logger(options.logPath, std::cout);
    NSPasteboard *board = pasteboard(options);
    if (options.expectedChangeCount && board.changeCount != *options.expectedChangeCount)
      throw std::runtime_error("OWNER_CHANGED: newer local copy superseded publication");
    [board prepareForNewContentsWithOptions:NSPasteboardContentsCurrentHostOnly];
    // Publish complete items in one write. Mixing writeObjects with legacy
    // board-level addTypes/setString can replace the NSURL representations,
    // leaving only the origin marker and disabling Finder's Paste command.
    if (![board writeObjects:items]) {
      throw std::runtime_error("AppKit rejected file URL publication");
    }
    NSArray<NSPasteboardItem *> *published = board.pasteboardItems;
    if (published.count != items.count) {
      throw std::runtime_error("file URL publication lost pasteboard items");
    }
    for (NSPasteboardItem *item in published) {
      if (![item.types containsObject:NSPasteboardTypeFileURL] ||
          ![[item stringForType:kOriginType] isEqualToString:origin]) {
        throw std::runtime_error("file URL publication lost its representation or origin");
      }
    }
    return holdPublication(board, origin, options, logger);
  }
}

int publishMacData(const std::vector<MacClipboardRepresentation> &representations, const MacClipboardOptions &options)
{
  @autoreleasepool {
    if (representations.empty() || representations.size() > 3) {
      throw std::invalid_argument("publish-data requires 1 to 3 distinct representations");
    }
    std::vector<RepresentationSource> sources;
    NSMutableArray<NSPasteboardType> *types = [NSMutableArray array];
    std::size_t totalBytes = 0;
    for (const auto &input : representations) {
      RepresentationSource source(input);
      if ([types containsObject:source.type]) {
        throw std::invalid_argument("duplicate pasteboard representation");
      }
      totalBytes += static_cast<std::size_t>(source.version.st_size);
      if (totalBytes > kMaxRepresentationBytes) {
        throw std::invalid_argument("representation inputs exceed aggregate 16 MiB budget");
      }
      [types addObject:source.type];
      sources.push_back(std::move(source));
    }
    auto logger = std::make_shared<Logger>(options.logPath, std::cout);
    DeskflowPrototypeDataProvider *provider = [[DeskflowPrototypeDataProvider alloc] initWithSources:std::move(sources)
                                                                                              logger:logger];
    NSPasteboardItem *item = [NSPasteboardItem new];
    NSString *origin = publisherOrigin(options);
    [item setString:origin forType:kOriginType];
    if (![item setDataProvider:provider forTypes:types]) {
      throw std::runtime_error("AppKit rejected lazy representation provider");
    }
    NSPasteboard *board = pasteboard(options);
    if (options.expectedChangeCount && board.changeCount != *options.expectedChangeCount)
      throw std::runtime_error("OWNER_CHANGED: newer local copy superseded publication");
    [board prepareForNewContentsWithOptions:NSPasteboardContentsCurrentHostOnly];
    if (![board writeObjects:@[ item ]]) {
      throw std::runtime_error("AppKit rejected representation publication");
    }
    const int result = holdPublication(board, origin, options, *logger);
    // Explicit use retains the provider for the whole owner run loop under ARC.
    (void)provider;
    return result;
  }
}

int readMacClipboard(
    const std::string &target, const std::optional<std::string> &outputPath, const MacClipboardOptions &options
)
{
  @autoreleasepool {
    Logger logger(options.logPath, std::cerr);
    NSPasteboard *board = pasteboard(options);
    if (!options.origin.empty() && [[board stringForType:kOriginType] isEqualToString:nativeString(options.origin)])
      throw std::runtime_error("SELF_ORIGIN: refusing to recapture this core's publication");
    const NSInteger changeCount = board.changeCount;
    if (options.expectedChangeCount && changeCount != *options.expectedChangeCount)
      throw std::runtime_error("OWNER_CHANGED: capture generation changed");
    NSData *data = nil;
    if (target == "text/uri-list") {
      NSArray<NSPasteboardItem *> *items = board.pasteboardItems;
      if (items.count > kMaxItems) {
        throw std::runtime_error("pasteboard exceeds file item limit");
      }
      NSMutableString *list = [NSMutableString string];
      for (NSPasteboardItem *item in items) {
        NSString *value = [item stringForType:NSPasteboardTypeFileURL];
        if (value) {
          NSURL *url = [NSURL URLWithString:value];
          if (!url.isFileURL) {
            throw std::runtime_error("invalid file URL representation");
          }
          // Finder can supply file:///.file/id=... instead of a pathname.
          // Resolve the reference while still in AppKit; passing its pseudo-path
          // to the POSIX source makes O_NOFOLLOW report SOURCE_CHANGED.
          if (url.isFileReferenceURL) {
            url = url.filePathURL;
            if (!url) {
              throw std::runtime_error("SOURCE_MISSING: cannot resolve file reference URL");
            }
          }
          [list appendFormat:@"%@\r\n", url.absoluteString];
          if ([list lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > kMaxFileListBytes) {
            throw std::runtime_error("file URL metadata exceeds 64 KiB");
          }
        }
      }
      if (list.length == 0) {
        throw std::runtime_error("clipboard has no file URL items");
      }
      data = [list dataUsingEncoding:NSUTF8StringEncoding];
    } else if (target == "TARGETS") {
      NSString *types = [[board.types componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"];
      data = [types dataUsingEncoding:NSUTF8StringEncoding];
    } else {
      data = [board dataForType:typeForTarget(target)];
      if (!data && target == "text/html") {
        NSData *rtf = [board dataForType:NSPasteboardTypeRTF];
        if (rtf) {
          data = htmlFromRtf(rtf);
          logger.write(
              @"converted",
              @{@"sourceType" : NSPasteboardTypeRTF,
                @"target" : @"text/html",
                @"returnedBytes" : @(data.length)}
          );
        }
      } else if (!data && target == "image/png") {
        NSData *tiff = [board dataForType:NSPasteboardTypeTIFF];
        if (tiff) {
          data = pngFromTiff(tiff);
          logger.write(
              @"converted",
              @{@"sourceType" : NSPasteboardTypeTIFF,
                @"target" : @"image/png",
                @"returnedBytes" : @(data.length)}
          );
        }
      }
    }
    if (!data) {
      throw std::runtime_error("clipboard representation unavailable (or access denied)");
    }
    // AppKit materializes data before returning it. This bounds accepted output,
    // not allocations made by AppKit or an untrusted external pasteboard owner.
    if (data.length > kMaxRepresentationBytes) {
      throw std::runtime_error("clipboard representation exceeds 16 MiB output limit");
    }
    if (board.changeCount != changeCount) {
      throw std::runtime_error("clipboard changed while reading representation");
    }
    std::ofstream file;
    std::ostream *output = &std::cout;
    if (outputPath) {
      file.open(*outputPath, std::ios::binary | std::ios::trunc);
      if (!file) {
        throw std::runtime_error("cannot open output path");
      }
      output = &file;
    }
    if (data.length != 0) {
      output->write(static_cast<const char *>(data.bytes), data.length);
    }
    output->flush();
    if (!*output) {
      throw std::runtime_error("cannot write clipboard output");
    }
    logger.write(
        @"read", @{
          @"target" : nativeString(target),
          @"returnedBytes" : @(data.length),
          @"changeCount" : @(changeCount),
          @"status" : @"OK"
        }
    );
    return 0;
  }
}

int inspectMacClipboard(const MacClipboardOptions &options)
{
  @autoreleasepool {
    std::cout << json(metadata(pasteboard(options), options)) << '\n';
    return 0;
  }
}

int watchMacClipboard(const MacClipboardOptions &options)
{
  @autoreleasepool {
    Logger logger(options.logPath, std::cout);
    NSPasteboard *board = pasteboard(options);
    SignalGuard signals;
    std::string previous;
    bool initialized = false;
    const auto start = std::chrono::steady_clock::now();
    while (!g_stop) {
      @autoreleasepool {
        // A writer can clear and then populate the pasteboard under one changeCount.
        // Poll the metadata too, so observing its intermediate empty/type-only state
        // does not permanently hide the completed publication or its origin marker.
        NSDictionary *record = metadata(board, options);
        if ([record[@"stable"] boolValue]) {
          auto fingerprint = json(record);
          if (fingerprint != previous) {
            previous = std::move(fingerprint);
            if (!initialized && options.skipInitial)
              logger.write(@"owner_changed", record);
            else
              logger.write([record[@"selfOrigin"] boolValue] ? @"selfChange" : @"clipboardChanged", record);
          }
          if (!initialized) {
            initialized = true;
            logger.write(@"watch_initialized", record);
          }
        }
        if (options.durationMs > 0 &&
            std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(options.durationMs)) {
          break;
        }
        pump(options.pollMs);
      }
    }
    return 0;
  }
}

int releaseMacPasteboard(const MacClipboardOptions &options)
{
  @autoreleasepool {
    if (options.pasteboard.empty() || [nativeString(options.pasteboard) isEqualToString:NSPasteboardNameGeneral]) {
      throw std::invalid_argument("release requires a named test pasteboard");
    }
    [pasteboard(options) releaseGlobally];
    return 0;
  }
}

} // namespace deskflow::clipboard::prototype
