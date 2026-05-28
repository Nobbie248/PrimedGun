#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Qt6::BundledFreetype" for configuration "Debug"
set_property(TARGET Qt6::BundledFreetype APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(Qt6::BundledFreetype PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/Qt6BundledFreetyped.lib"
  )

list(APPEND _cmake_import_check_targets Qt6::BundledFreetype )
list(APPEND _cmake_import_check_files_for_Qt6::BundledFreetype "${_IMPORT_PREFIX}/lib/Qt6BundledFreetyped.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
