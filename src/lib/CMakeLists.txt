# SPDX-FileCopyrightText: (C) 2024 Deskflow Developers
# SPDX-FileCopyrightText: (C) 2012 - 2024 Symless Ltd
# SPDX-FileCopyrightText: (C) 2009 - 2012 Nick Bolton
# SPDX-License-Identifier: MIT

add_subdirectory(arch)
add_subdirectory(base)
add_subdirectory(client)
add_subdirectory(common)
if(BUILD_CLIPBOARD_FILES)
  add_subdirectory(clipboard)
endif()
add_library(clipboard_runtime STATIC clipboard/CoreClipboard.cpp)
target_link_libraries(clipboard_runtime PUBLIC common Qt6::Network)
if(BUILD_CLIPBOARD_FILES)
  find_package(OpenSSL REQUIRED)
  target_link_libraries(clipboard_runtime PUBLIC deskflow_clipboard OpenSSL::Crypto)
endif()
add_subdirectory(deskflow)
add_subdirectory(io)
add_subdirectory(mt)
add_subdirectory(net)
add_subdirectory(platform)
add_subdirectory(server)
add_subdirectory(gui)
