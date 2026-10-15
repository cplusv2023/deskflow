# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(deskflow-clipboard-prototype LANGUAGES CXX)
  include(CTest)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The clipboard prototype currently supports Linux only")
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(PROTOTYPE_FUSE3 REQUIRED IMPORTED_TARGET fuse3)
pkg_check_modules(PROTOTYPE_X11 REQUIRED IMPORTED_TARGET x11)

add_library(clipboard_prototype_files STATIC FakeRemoteSource.cpp FuseMount.cpp)
target_compile_features(clipboard_prototype_files PUBLIC cxx_std_20)
target_compile_definitions(clipboard_prototype_files PUBLIC _FILE_OFFSET_BITS=64)
target_link_libraries(clipboard_prototype_files PUBLIC PkgConfig::PROTOTYPE_FUSE3 Threads::Threads)

add_executable(deskflow-file-prototype file_main.cpp)
target_link_libraries(deskflow-file-prototype PRIVATE clipboard_prototype_files)

add_executable(deskflow-clipboard-probe X11Clipboard.cpp clipboard_main.cpp)
target_compile_features(deskflow-clipboard-probe PRIVATE cxx_std_20)
target_link_libraries(deskflow-clipboard-probe PRIVATE PkgConfig::PROTOTYPE_X11)

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
  option(CLIPBOARD_PROTOTYPE_DESKTOP_TESTS "Run FUSE mount and X11 clipboard integration tests" OFF)
  if(CLIPBOARD_PROTOTYPE_DESKTOP_TESTS)
    add_test(
      NAME clipboard_prototype_mount
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_prototype.py
        --file-tool $<TARGET_FILE:deskflow-file-prototype> --mount
    )
    add_test(
      NAME clipboard_prototype_x11
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_prototype.py
        --clipboard-tool $<TARGET_FILE:deskflow-clipboard-probe> --clipboard
    )
    set_tests_properties(clipboard_prototype_mount clipboard_prototype_x11 PROPERTIES TIMEOUT 90)
  endif()
endif()
