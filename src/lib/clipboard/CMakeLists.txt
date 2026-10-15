# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(DeskflowClipboardService LANGUAGES CXX)
  include(CTest)
endif()

find_package(Threads REQUIRED)
add_library(deskflow_clipboard_service STATIC FileService.cpp LocalFileSource.cpp ClipboardBroker.cpp)
target_compile_features(deskflow_clipboard_service PUBLIC cxx_std_20)
target_include_directories(deskflow_clipboard_service PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(deskflow_clipboard_service PUBLIC Threads::Threads)
set_target_properties(deskflow_clipboard_service PROPERTIES
  CXX_EXTENSIONS OFF AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

if(DEFINED BUILD_TESTING)
  set(_clipboard_tests_default ${BUILD_TESTING})
elseif(DEFINED BUILD_TESTS)
  set(_clipboard_tests_default ${BUILD_TESTS})
else()
  set(_clipboard_tests_default OFF)
endif()
option(CLIPBOARD_SERVICE_TESTS "Build standalone clipboard service contract tests" ${_clipboard_tests_default})
unset(_clipboard_tests_default)
if(CLIPBOARD_SERVICE_TESTS)
  add_executable(clipboard-service-tests FileServiceTests.cpp)
  target_link_libraries(clipboard-service-tests PRIVATE deskflow_clipboard_service)
  add_test(NAME clipboard-service-contract COMMAND clipboard-service-tests)
  add_executable(clipboard-broker-tests ClipboardBrokerTests.cpp)
  target_link_libraries(clipboard-broker-tests PRIVATE deskflow_clipboard_service)
  set_target_properties(clipboard-service-tests clipboard-broker-tests PROPERTIES
    CXX_EXTENSIONS OFF AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
  add_test(NAME clipboard-broker-lifecycle COMMAND clipboard-broker-tests)
endif()
