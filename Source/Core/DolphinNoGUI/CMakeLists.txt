add_executable(dolphin-nogui
  Platform.cpp
  Platform.h
  PlatformHeadless.cpp
  MainNoGUI.cpp
)

if(X11_FOUND)
  target_sources(dolphin-nogui PRIVATE PlatformX11.cpp)
  target_link_libraries(dolphin-nogui PRIVATE PkgConfig::XRANDR PkgConfig::X11)
endif()

if(WIN32)
  target_sources(dolphin-nogui PRIVATE PlatformWin32.cpp)
endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  target_sources(dolphin-nogui PRIVATE PlatformFBDev.cpp)
endif()

if (WIN32)
  target_sources(dolphin-nogui PRIVATE DolphinNoGUI.exe.manifest DolphinNoGUI.rc)

  set_target_properties(dolphin-nogui PROPERTIES
    DEBUG_POSTFIX    D
    OUTPUT_NAME      DolphinNoGUI
  )
else()
  set_target_properties(dolphin-nogui PROPERTIES OUTPUT_NAME dolphin-nogui)
endif()

if(UNIX AND LINUX_LOCAL_DEV)
  # Relocatable layout: Sys/ and assets/ next to the binary (links; a release copies them).
  add_custom_command(TARGET dolphin-nogui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_SOURCE_DIR}/Data/Sys $<TARGET_FILE_DIR:dolphin-nogui>/Sys
    COMMAND ${CMAKE_COMMAND} -E create_symlink ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:dolphin-nogui>/assets
  )
endif()

target_link_libraries(dolphin-nogui
PRIVATE
  core
  uicommon
  cpp-optparse
)

if(WIN32)
  # needed for adjusting window decorations with DwmSetWindowAttribute
  target_link_libraries(dolphin-nogui PRIVATE dwmapi.lib)
endif()

if(MSVC)
  # Add precompiled header
  target_link_libraries(dolphin-nogui PRIVATE use_pch)
endif()

if(USE_DISCORD_PRESENCE)
  target_compile_definitions(dolphin-nogui PRIVATE -DUSE_DISCORD_PRESENCE)
endif()

set(CPACK_PACKAGE_EXECUTABLES ${CPACK_PACKAGE_EXECUTABLES} dolphin-nogui)
install(TARGETS dolphin-nogui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

