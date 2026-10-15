# SPDX-FileCopyrightText: (C) 2024 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-License-Identifier: MIT

# HACK This is set when the files is included so its the real path
# calling CMAKE_CURRENT_LIST_DIR after include would return the wrong scope var
set(MY_DIR ${CMAKE_CURRENT_LIST_DIR})
set(OSX_BUNDLE ${BUILD_OSX_BUNDLE})

set(OS_STRING "macos-${BUILD_ARCHITECTURE}")

if (OSX_BUNDLE)
  if(BUILD_CLIPBOARD_FILES)
    install(FILES ${MY_DIR}/clipboard-readme.txt
      DESTINATION ${CMAKE_INSTALL_LICENSE_DIR} RENAME CLIPBOARD_README.txt)
    install(FILES ${PROJECT_SOURCE_DIR}/docs/dev/clipboard-core-v1.md
      DESTINATION ${CMAKE_INSTALL_DOCDIR})
  endif()
  configure_file(${MY_DIR}/DeployBundle.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/DeployBundle.cmake @ONLY)
  install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/DeployBundle.cmake)
  set(CPACK_PACKAGE_ICON "${MY_DIR}/dmg-volume.icns")
  set(CPACK_DMG_BACKGROUND_IMAGE "${MY_DIR}/dmg-background.tiff")
  set(CPACK_DMG_DS_STORE_SETUP_SCRIPT "${MY_DIR}/generate_ds_store.applescript")
  set(CPACK_DMG_VOLUME_NAME "${CMAKE_PROJECT_PROPER_NAME}")
  set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE ON)
  set(CPACK_GENERATOR "DragNDrop")
endif()
