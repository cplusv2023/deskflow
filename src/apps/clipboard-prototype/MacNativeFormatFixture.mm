/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#import <AppKit/AppKit.h>

#include <iostream>

// Test-only native-format writer. Never accepts the system pasteboard, never
// reads pasteboard data, and intentionally offers no general-purpose app CLI.
int main(int argc, char **argv)
{
  @autoreleasepool {
    if (argc < 4 || argc % 2 != 0) {
      return 2;
    }
    NSString *name = @(argv[1]);
    if (![name hasPrefix:@"org.deskflow.test."]) {
      return 2;
    }
    NSPasteboardItem *item = [NSPasteboardItem new];
    for (int index = 2; index < argc; index += 2) {
      NSString *type = @(argv[index]);
      if (![@[ NSPasteboardTypeRTF, NSPasteboardTypeTIFF, NSPasteboardTypeHTML, NSPasteboardTypePNG ]
              containsObject:type]) {
        return 2;
      }
      NSString *path = @(argv[index + 1]);
      NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
      if (!attributes || [attributes[NSFileSize] unsignedLongLongValue] > 16 * 1024 * 1024 ||
          ![attributes[NSFileType] isEqual:NSFileTypeRegular]) {
        return 2;
      }
      NSData *data = [NSData dataWithContentsOfFile:path];
      if (!data || data.length > 16 * 1024 * 1024 || ![item setData:data forType:type]) {
        return 2;
      }
    }
    NSPasteboard *board = [NSPasteboard pasteboardWithName:name];
    [board prepareForNewContentsWithOptions:NSPasteboardContentsCurrentHostOnly];
    return [board writeObjects:@[ item ]] ? 0 : 2;
  }
}
