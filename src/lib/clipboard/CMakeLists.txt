# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

find_package(Threads REQUIRED)
add_library(deskflow_clipboard STATIC
  LocalFileSource.cpp
  LocalFileRpc.cpp
  ClipboardBroker.cpp
)
target_compile_features(deskflow_clipboard PUBLIC cxx_std_20)
target_compile_definitions(deskflow_clipboard PUBLIC _FILE_OFFSET_BITS=64)
target_include_directories(deskflow_clipboard PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(deskflow_clipboard PUBLIC Threads::Threads)
target_compile_options(deskflow_clipboard PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(deskflow_clipboard PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF CXX_EXTENSIONS OFF)

if(BUILD_TESTING)
  foreach(test_name LocalFileSource ClipboardBroker LocalFileRpc)
    add_executable(${test_name}Tests ${test_name}Tests.cpp)
    target_link_libraries(${test_name}Tests PRIVATE deskflow_clipboard)
    set_target_properties(${test_name}Tests PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    add_test(NAME clipboard_${test_name} COMMAND ${test_name}Tests)
  endforeach()
endif()
