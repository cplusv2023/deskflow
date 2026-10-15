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
pkg_check_modules(PROTOTYPE_XFIXES REQUIRED IMPORTED_TARGET xfixes)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/clipboard ${CMAKE_CURRENT_BINARY_DIR}/clipboard)

add_library(clipboard_prototype_files STATIC FakeRemoteSource.cpp FuseMount.cpp RpcFileSource.cpp)
target_compile_features(clipboard_prototype_files PUBLIC cxx_std_20)
target_compile_definitions(clipboard_prototype_files PUBLIC _FILE_OFFSET_BITS=64)
target_link_libraries(clipboard_prototype_files PUBLIC deskflow_clipboard PkgConfig::PROTOTYPE_FUSE3 Threads::Threads)

add_executable(deskflow-file-prototype file_main.cpp)
target_link_libraries(deskflow-file-prototype PRIVATE clipboard_prototype_files)

add_library(clipboard_prototype_x11 STATIC X11Clipboard.cpp)
target_compile_features(clipboard_prototype_x11 PUBLIC cxx_std_20)
target_link_libraries(clipboard_prototype_x11 PUBLIC PkgConfig::PROTOTYPE_X11 PkgConfig::PROTOTYPE_XFIXES)
add_executable(deskflow-clipboard-probe clipboard_main.cpp)
target_compile_features(deskflow-clipboard-probe PRIVATE cxx_std_20)
target_link_libraries(deskflow-clipboard-probe PRIVATE clipboard_prototype_x11)

add_executable(deskflow-file-service service_main.cpp)
target_link_libraries(deskflow-file-service PRIVATE deskflow_clipboard clipboard_prototype_files clipboard_prototype_x11)

foreach(prototype_target
    clipboard_prototype_files clipboard_prototype_x11
    deskflow-file-prototype deskflow-clipboard-probe deskflow-file-service)
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
  add_test(
    NAME clipboard_local_rpc
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_local_rpc.py
      --tool $<TARGET_FILE:deskflow-file-service>
  )
  add_test(
    NAME clipboard_x11_formats
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_x11_formats.py
      --tool $<TARGET_FILE:deskflow-clipboard-probe>
      --service-tool $<TARGET_FILE:deskflow-file-service>
  )
  set_tests_properties(clipboard_local_rpc clipboard_x11_formats PROPERTIES TIMEOUT 90)
  set_tests_properties(clipboard_x11_formats PROPERTIES SKIP_RETURN_CODE 77)
  option(CLIPBOARD_PROTOTYPE_DESKTOP_TESTS "Run FUSE mount and X11 clipboard integration tests" OFF)
  option(CLIPBOARD_PROTOTYPE_FUSE_TESTS "Run FUSE integration tests without accessing the desktop clipboard" OFF)
  if(CLIPBOARD_PROTOTYPE_FUSE_TESTS OR CLIPBOARD_PROTOTYPE_DESKTOP_TESTS)
    add_test(
      NAME clipboard_prototype_mount
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_prototype.py
        --file-tool $<TARGET_FILE:deskflow-file-prototype> --mount
    )
    add_test(
      NAME clipboard_rpc_mount
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_rpc_mount.py
        --file-tool $<TARGET_FILE:deskflow-file-prototype>
        --service-tool $<TARGET_FILE:deskflow-file-service>
    )
    set_tests_properties(clipboard_prototype_mount clipboard_rpc_mount PROPERTIES TIMEOUT 90)
  endif()
  if(CLIPBOARD_PROTOTYPE_DESKTOP_TESTS)
    add_test(
      NAME clipboard_prototype_x11
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_prototype.py
        --clipboard-tool $<TARGET_FILE:deskflow-clipboard-probe> --clipboard
    )
    set_tests_properties(clipboard_prototype_x11 PROPERTIES TIMEOUT 90)
  endif()
endif()
