#----------------------------------------------------------------
# Generated CMake target import file for configuration "RelWithDebInfo".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Qt6::ExampleIconsPrivate" for configuration "RelWithDebInfo"
set_property(TARGET Qt6::ExampleIconsPrivate APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(Qt6::ExampleIconsPrivate PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELWITHDEBINFO "CXX"
  IMPORTED_LOCATION_RELWITHDEBINFO "${_IMPORT_PREFIX}/lib/Qt6ExampleIcons.lib"
  )

list(APPEND _cmake_import_check_targets Qt6::ExampleIconsPrivate )
list(APPEND _cmake_import_check_files_for_Qt6::ExampleIconsPrivate "${_IMPORT_PREFIX}/lib/Qt6ExampleIcons.lib" )

# Import target "Qt6::ExampleIconsPrivate_resources_1" for configuration "RelWithDebInfo"
set_property(TARGET Qt6::ExampleIconsPrivate_resources_1 APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(Qt6::ExampleIconsPrivate_resources_1 PROPERTIES
  IMPORTED_COMMON_LANGUAGE_RUNTIME_RELWITHDEBINFO ""
  IMPORTED_OBJECTS_RELWITHDEBINFO "${_IMPORT_PREFIX}/lib/objects-RelWithDebInfo/ExampleIconsPrivate_resources_1/.rcc/qrc_example_icons.cpp.obj"
  )

list(APPEND _cmake_import_check_targets Qt6::ExampleIconsPrivate_resources_1 )
list(APPEND _cmake_import_check_files_for_Qt6::ExampleIconsPrivate_resources_1 "${_IMPORT_PREFIX}/lib/objects-RelWithDebInfo/ExampleIconsPrivate_resources_1/.rcc/qrc_example_icons.cpp.obj" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
