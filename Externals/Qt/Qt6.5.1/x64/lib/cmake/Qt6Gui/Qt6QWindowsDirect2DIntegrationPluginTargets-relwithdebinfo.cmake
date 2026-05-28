#----------------------------------------------------------------
# Generated CMake target import file for configuration "RelWithDebInfo".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Qt6::QWindowsDirect2DIntegrationPlugin" for configuration "RelWithDebInfo"
set_property(TARGET Qt6::QWindowsDirect2DIntegrationPlugin APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(Qt6::QWindowsDirect2DIntegrationPlugin PROPERTIES
  IMPORTED_COMMON_LANGUAGE_RUNTIME_RELWITHDEBINFO ""
  IMPORTED_LOCATION_RELWITHDEBINFO "${_IMPORT_PREFIX}/./plugins/platforms/qdirect2d.dll"
  )

list(APPEND _cmake_import_check_targets Qt6::QWindowsDirect2DIntegrationPlugin )
list(APPEND _cmake_import_check_files_for_Qt6::QWindowsDirect2DIntegrationPlugin "${_IMPORT_PREFIX}/./plugins/platforms/qdirect2d.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
