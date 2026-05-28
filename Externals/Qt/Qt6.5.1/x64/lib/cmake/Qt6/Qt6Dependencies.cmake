set(Qt6_FOUND FALSE)

set(__qt_platform_requires_host_info_package "FALSE")
set(__qt_platform_initial_qt_host_path "")
set(__qt_platform_initial_qt_host_path_cmake_dir "")

_qt_internal_setup_qt_host_path(
    "${__qt_platform_requires_host_info_package}"
    "${__qt_platform_initial_qt_host_path}"
    "${__qt_platform_initial_qt_host_path_cmake_dir}")
_qt_internal_find_host_info_package(${__qt_platform_requires_host_info_package})

# note: _third_party_deps example: "ICU\\;FALSE\\;1.0\\;i18n uc data;ZLIB\\;FALSE\\;\\;"
set(__qt_third_party_deps "Threads\;FALSE\;\;\;")

if(NOT QT_NO_THREADS_PREFER_PTHREAD_FLAG)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
endif()

# Don't propagate REQUIRED so we don't immediately FATAL_ERROR, rather let the find_dependency calls
# set _NOT_FOUND_MESSAGE which will be displayed by the includer of the Dependencies file.
set(${CMAKE_FIND_PACKAGE_NAME}_FIND_REQUIRED FALSE)

_qt_internal_find_third_party_dependencies(Qt6 __qt_third_party_deps)

set(Qt6_FOUND TRUE)
