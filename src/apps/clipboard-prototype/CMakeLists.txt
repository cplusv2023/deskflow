# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(deskflow-clipboard-prototype LANGUAGES CXX)
  include(CTest)
endif()

if(NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The clipboard prototype supports Linux and macOS")
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
if(APPLE AND EXISTS "/usr/local/lib/pkgconfig/fuse3.pc")
  # macFUSE's SDK is installed here even on Apple Silicon.
  set(ENV{PKG_CONFIG_PATH} "/usr/local/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()
pkg_check_modules(PROTOTYPE_FUSE3 REQUIRED IMPORTED_TARGET fuse3)

add_library(clipboard_prototype_files STATIC FakeRemoteSource.cpp FuseMount.cpp RpcFileSource.cpp)
target_compile_features(clipboard_prototype_files PUBLIC cxx_std_20)
target_compile_definitions(clipboard_prototype_files PUBLIC _FILE_OFFSET_BITS=64)
target_link_libraries(clipboard_prototype_files PUBLIC deskflow_clipboard PkgConfig::PROTOTYPE_FUSE3 Threads::Threads)

add_executable(deskflow-file-prototype file_main.cpp)
target_link_libraries(deskflow-file-prototype PRIVATE clipboard_prototype_files)

if(NOT TARGET deskflow_clipboard)
  add_subdirectory(../../lib/clipboard ${CMAKE_CURRENT_BINARY_DIR}/clipboard-service)
endif()

if(APPLE)
  enable_language(OBJCXX)
  # Keep the FUSE3 include before /usr/local/include/fuse.h (FUSE2). CMake
  # otherwise marks pkg-config includes as system paths, changing that order.
  set_target_properties(clipboard_prototype_files PROPERTIES NO_SYSTEM_FROM_IMPORTED ON)
  target_compile_definitions(clipboard_prototype_files PRIVATE FUSE_DARWIN_ENABLE_EXTENSIONS=0)
  add_executable(deskflow-clipboard-probe MacClipboard.mm mac_clipboard_main.mm)
  target_compile_options(deskflow-clipboard-probe PRIVATE -fobjc-arc)
  target_link_libraries(deskflow-clipboard-probe PRIVATE "-framework AppKit" "-framework ImageIO" "-framework CoreGraphics")
  set_target_properties(deskflow-clipboard-probe PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
  add_executable(deskflow-mac-file-service MacFileServiceMain.mm)
  target_compile_options(deskflow-mac-file-service PRIVATE -fobjc-arc -Wall -Wextra -Wpedantic)
  target_link_libraries(deskflow-mac-file-service PRIVATE deskflow_clipboard_service "-framework AppKit")
  set_target_properties(deskflow-mac-file-service PROPERTIES
    OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
else()
  pkg_check_modules(PROTOTYPE_X11 REQUIRED IMPORTED_TARGET x11)
  pkg_check_modules(PROTOTYPE_XFIXES REQUIRED IMPORTED_TARGET xfixes)
  add_library(clipboard_prototype_x11 STATIC X11Clipboard.cpp)
  target_compile_features(clipboard_prototype_x11 PUBLIC cxx_std_20)
  target_link_libraries(clipboard_prototype_x11 PUBLIC PkgConfig::PROTOTYPE_X11 PkgConfig::PROTOTYPE_XFIXES)
  add_executable(deskflow-clipboard-probe clipboard_main.cpp)
  target_link_libraries(deskflow-clipboard-probe PRIVATE clipboard_prototype_x11)
  add_executable(deskflow-file-service service_main.cpp)
  target_link_libraries(deskflow-file-service PRIVATE clipboard_prototype_files clipboard_prototype_x11)
  target_link_libraries(deskflow-clipboard-probe PRIVATE PkgConfig::PROTOTYPE_X11)
endif()
target_compile_features(deskflow-clipboard-probe PRIVATE cxx_std_20)
target_link_libraries(deskflow-clipboard-probe PRIVATE Threads::Threads)

foreach(prototype_target clipboard_prototype_files deskflow-file-prototype deskflow-clipboard-probe)
  set_target_properties(${prototype_target} PROPERTIES CXX_EXTENSIONS OFF AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
  target_compile_options(${prototype_target} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(
    NAME clipboard_prototype_source
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_prototype.py
      --file-tool $<TARGET_FILE:deskflow-file-prototype> --source
  )
  if(APPLE)
    add_executable(clipboard-native-format-fixture MacNativeFormatFixture.mm)
    target_compile_options(clipboard-native-format-fixture PRIVATE -fobjc-arc -Wall -Wextra -Wpedantic)
    target_link_libraries(clipboard-native-format-fixture PRIVATE "-framework AppKit")
    set_target_properties(clipboard-native-format-fixture PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    add_test(
      NAME clipboard_prototype_appkit
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_mac_clipboard.py
        --probe $<TARGET_FILE:deskflow-clipboard-probe>
        --native-format-fixture $<TARGET_FILE:clipboard-native-format-fixture>
    )
    set_tests_properties(clipboard_prototype_appkit PROPERTIES TIMEOUT 60)
    add_test(
      NAME clipboard_prototype_mac_file_service
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_mac_file_service.py
        --service $<TARGET_FILE:deskflow-mac-file-service> --probe $<TARGET_FILE:deskflow-clipboard-probe>
    )
    set_tests_properties(clipboard_prototype_mac_file_service PROPERTIES TIMEOUT 60)
  endif()
  option(CLIPBOARD_PROTOTYPE_DESKTOP_TESTS "Run filesystem mount and desktop clipboard integration tests" OFF)
  if(CLIPBOARD_PROTOTYPE_DESKTOP_TESTS)
    add_test(
      NAME clipboard_prototype_mount
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_prototype.py
        --file-tool $<TARGET_FILE:deskflow-file-prototype> --mount
    )
    set_tests_properties(clipboard_prototype_mount PROPERTIES TIMEOUT 90)
    if(NOT APPLE)
      add_test(
        NAME clipboard_prototype_x11
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_prototype.py
          --clipboard-tool $<TARGET_FILE:deskflow-clipboard-probe> --clipboard
      )
      set_tests_properties(clipboard_prototype_x11 PROPERTIES TIMEOUT 90)
    endif()
  endif()
endif()

if(BUILD_CLIPBOARD_FILES)
  add_dependencies(deskflow-core deskflow-clipboard-probe deskflow-file-prototype)
  install(TARGETS deskflow-clipboard-probe deskflow-file-prototype RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  if(BUILD_OSX_BUNDLE)
    foreach(helper_target deskflow-clipboard-probe deskflow-file-prototype)
      set_target_properties(${helper_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY $<TARGET_BUNDLE_CONTENT_DIR:${CMAKE_PROJECT_PROPER_NAME}>/MacOS
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@loader_path/../Libraries;@loader_path/../Frameworks"
        XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME YES)
    endforeach()
  endif()
endif()
