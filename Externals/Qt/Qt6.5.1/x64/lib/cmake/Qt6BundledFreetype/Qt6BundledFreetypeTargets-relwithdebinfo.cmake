#----------------------------------------------------------------
# Generated CMake target import file for configuration "RelWithDebInfo".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Qt6::BundledFreetype" for configuration "RelWithDebInfo"
set_property(TARGET Qt6::BundledFreetype APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(Qt6::BundledFreetype PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELWITHDEBINFO "C"
  IMPORTED_LOCATION_RELWITHDEBINFO "${_IMPORT_PREFIX}/lib/Qt6BundledFreetype.lib"
  )

list(APPEND _cmake_import_check_targets Qt6::BundledFreetype )
list(APPEND _cmake_import_check_files_for_Qt6::BundledFreetype "${_IMPORT_PREFIX}/lib/Qt6BundledFreetype.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
