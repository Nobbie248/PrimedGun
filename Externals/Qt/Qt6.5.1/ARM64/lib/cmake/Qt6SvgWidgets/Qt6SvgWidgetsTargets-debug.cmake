#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Qt6::SvgWidgets" for configuration "Debug"
set_property(TARGET Qt6::SvgWidgets APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(Qt6::SvgWidgets PROPERTIES
  IMPORTED_IMPLIB_DEBUG "${_IMPORT_PREFIX}/lib/Qt6SvgWidgetsd.lib"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/bin/Qt6SvgWidgetsd.dll"
  )

list(APPEND _cmake_import_check_targets Qt6::SvgWidgets )
list(APPEND _cmake_import_check_files_for_Qt6::SvgWidgets "${_IMPORT_PREFIX}/lib/Qt6SvgWidgetsd.lib" "${_IMPORT_PREFIX}/bin/Qt6SvgWidgetsd.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
