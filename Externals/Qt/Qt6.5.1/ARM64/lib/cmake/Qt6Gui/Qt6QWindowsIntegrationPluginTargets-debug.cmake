#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Qt6::QWindowsIntegrationPlugin" for configuration "Debug"
set_property(TARGET Qt6::QWindowsIntegrationPlugin APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(Qt6::QWindowsIntegrationPlugin PROPERTIES
  IMPORTED_COMMON_LANGUAGE_RUNTIME_DEBUG ""
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/./plugins/platforms/qwindowsd.dll"
  )

list(APPEND _cmake_import_check_targets Qt6::QWindowsIntegrationPlugin )
list(APPEND _cmake_import_check_files_for_Qt6::QWindowsIntegrationPlugin "${_IMPORT_PREFIX}/./plugins/platforms/qwindowsd.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
