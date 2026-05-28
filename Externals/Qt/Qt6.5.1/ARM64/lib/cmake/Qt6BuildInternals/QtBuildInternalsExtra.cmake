# Propagate common variables via BuildInternals package.
set(QT_BUILD_SHARED_LIBS ON)
option(BUILD_SHARED_LIBS "Build Qt statically or dynamically" ON)
set(QT_CMAKE_EXPORT_NAMESPACE Qt6)
set(INSTALL_CMAKE_NAMESPACE Qt6)
set(QT_BUILD_INTERNALS_PATH "${CMAKE_CURRENT_LIST_DIR}")

# The relocatable install prefix is meant to be used to find things like host binaries (syncqt),
# when the CMAKE_INSTALL_PREFIX is overridden to point to a different path (like when building a
# a Qt repo using Conan, which will set a random install prefix instead of installing into the
# original Qt install prefix).
get_filename_component(QT_BUILD_INTERNALS_RELOCATABLE_INSTALL_PREFIX
                       ${CMAKE_CURRENT_LIST_DIR}/../../../
                       ABSOLUTE)

# Stores in out_var the new install/staging prefix for this build.
#
# new_prefix: the new prefix for this repository
# orig_prefix: the prefix that was used when qtbase was configured
#
# On Windows hosts: if the original prefix does not start with a drive letter, this function removes
# the drive letter from the new prefix. This is needed for installation with DESTDIR set.
function(qt_internal_new_prefix out_var new_prefix orig_prefix)
    if(CMAKE_HOST_WIN32)
        set(drive_letter_regexp "^[a-zA-Z]:")
        if(new_prefix MATCHES "${drive_letter_regexp}"
            AND NOT orig_prefix MATCHES "${drive_letter_regexp}")
            string(SUBSTRING "${new_prefix}" 2 -1 new_prefix)
        endif()
    endif()
    set(${out_var} "${new_prefix}" PARENT_SCOPE)
endfunction()

# If no explicit CMAKE_INSTALL_PREFIX is provided, force set the original Qt installation prefix,
# so that further modules / repositories are  installed into same original location.
# This means by default when configuring qtsvg / qtdeclarative, they will be installed the regular
# Qt installation prefix.
# If an explicit installation prefix is specified,  honor it.
# This is an attempt to support Conan, aka handle installation of modules into a
# different installation prefix than the original one. Also allow to opt out via a special variable.
# In a top-level build, QtSetup.cmake takes care of setting CMAKE_INSTALL_PREFIX.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND
        NOT QT_BUILD_INTERNALS_NO_FORCE_SET_INSTALL_PREFIX
        AND NOT QT_SUPERBUILD)
    set(qtbi_orig_prefix "E:/qsc/dist/dolphin.arm64_6.5.1")
    set(qtbi_orig_staging_prefix "")
    qt_internal_new_prefix(qtbi_new_prefix
        "${QT_BUILD_INTERNALS_RELOCATABLE_INSTALL_PREFIX}"
        "${qtbi_orig_prefix}")
    if(NOT qtbi_orig_staging_prefix STREQUAL ""
            AND "${CMAKE_STAGING_PREFIX}" STREQUAL ""
            AND NOT QT_BUILD_INTERNALS_NO_FORCE_SET_STAGING_PREFIX)
        qt_internal_new_prefix(qtbi_new_staging_prefix
            "${qtbi_new_prefix}"
            "${qtbi_orig_staging_prefix}")
        set(CMAKE_STAGING_PREFIX "${qtbi_new_staging_prefix}" CACHE PATH
            "Staging path prefix, prepended onto install directories on the host machine." FORCE)
        set(qtbi_new_prefix "${qtbi_orig_prefix}")
    endif()
    set(CMAKE_INSTALL_PREFIX "${qtbi_new_prefix}" CACHE PATH
        "Install path prefix, prepended onto install directories." FORCE)
    unset(qtbi_orig_prefix)
    unset(qtbi_new_prefix)
    unset(qtbi_orig_staging_prefix)
    unset(qtbi_new_staging_prefix)
endif()

# Propagate developer builds to other modules via BuildInternals package.
if(OFF)
    set(FEATURE_developer_build ON CACHE BOOL "Developer build." FORCE)
endif()

# Propagate non-prefix builds.
set(QT_WILL_INSTALL ON CACHE BOOL
    "Boolean indicating if doing a Qt prefix build (vs non-prefix build)." FORCE)

set(QT_SOURCE_TREE "E:/qsc/qt-everywhere-src-6.5.1/qtbase" CACHE PATH
"A path to the source tree of the previously configured QtBase project." FORCE)

# Propagate decision of building tests and examples to other repositories.
set(QT_BUILD_TESTS FALSE CACHE BOOL "Build the testing tree.")
set(QT_BUILD_EXAMPLES FALSE CACHE BOOL "Build Qt examples")
set(QT_BUILD_BENCHMARKS OFF CACHE BOOL "Build Qt Benchmarks")
set(QT_BUILD_MANUAL_TESTS OFF CACHE BOOL "Build Qt manual tests")
set(QT_BUILD_MINIMAL_STATIC_TESTS OFF CACHE BOOL
    "Build minimal subset of tests for static Qt builds")
set(QT_BUILD_MINIMAL_ANDROID_MULTI_ABI_TESTS OFF CACHE BOOL
    "Build minimal subset of tests for Android multi-ABI Qt builds")

set(QT_BUILD_TESTS_BATCHED OFF CACHE BOOL
    "Should all tests be batched into a single binary.")

set(QT_BUILD_TESTS_BY_DEFAULT ON CACHE BOOL
    "Should tests be built as part of the default 'all' target.")
set(QT_BUILD_EXAMPLES_BY_DEFAULT ON CACHE BOOL
    "Should examples be built as part of the default 'all' target.")
set(QT_BUILD_TOOLS_BY_DEFAULT OFF CACHE BOOL
    "Should tools be built as part of the default 'all' target.")

set(QT_BUILD_EXAMPLES_AS_EXTERNAL "OFF" CACHE BOOL
    "Should examples be built as ExternalProjects.")

# Propagate usage of ccache.
set(QT_USE_CCACHE OFF CACHE BOOL "Enable the use of ccache")

# Propagate usage of unity build.
set(QT_UNITY_BUILD OFF CACHE BOOL "Enable unity (jumbo) build")
set(QT_UNITY_BUILD_BATCH_SIZE "32" CACHE STRING "Unity build batch size")

# Propragate the value of WARNINGS_ARE_ERRORS.
set(WARNINGS_ARE_ERRORS "OFF" CACHE BOOL "Build Qt with warnings as errors")

# Propagate usage of versioned hard link.
set(QT_CREATE_VERSIONED_HARD_LINK "ON" CACHE BOOL
    "Enable the use of versioned hard link")

# The minimum version required to build Qt.
set(QT_SUPPORTED_MIN_CMAKE_VERSION_FOR_BUILDING_QT "3.16")
set(QT_COMPUTED_MIN_CMAKE_VERSION_FOR_BUILDING_QT "3.16")

# The lower and upper CMake version policy range as computed by qtbase.
# These values are inherited when building other Qt repositories, unless overridden
# in the respective repository .cmake.conf file.
# These are not cache variables, so that they can be overridden in each repo directory scope.
if(NOT DEFINED QT_MIN_NEW_POLICY_CMAKE_VERSION)
    set(QT_MIN_NEW_POLICY_CMAKE_VERSION "3.16")
endif()
if(NOT DEFINED QT_MAX_NEW_POLICY_CMAKE_VERSION)
    set(QT_MAX_NEW_POLICY_CMAKE_VERSION "3.21")
endif()

get_property(__qt_internal_extras_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

# We want the same build type to be used when configuring all Qt repos or standalone
# tests or single tests.
# To do that, we need to force-set the CMAKE_BUILD_TYPE cache var because CMake itself
# initializes it with the value of CMAKE_BUILD_TYPE_INIT at the start of project
# configuration, so we need to override it.
# Note the value of CMAKE_BUILD_TYPE_INIT is different based on the platform, most
# Linux and macOS platforms will have it empty, but Windows platforms will have a value.
#
# We can't reliably differentiate between a value set on the command line by the user
# and one set by CMake, so we use a few heuristics:
# 1) When using a qt.toolchain.cmake file, we rely on the toolchain file to tell us
#    if a value was set by the user at initial configure time. On a 2nd run there will
#    always be a value in the cache, but at that point we've already set it to whatever it needs
#    to be.
# 2) If a toolchain file is not used, we rely on the value of the CMake internal
#    CMAKE_BUILD_TYPE_INIT variable.
#    This won't work reliably on Windows where CMAKE_BUILD_TYPE_INIT is non-empty.
#
# Both cases won't handle an empty "" config set by the user, but we claim that's an
# unsupported config when building Qt.
#
# Allow an opt out when QT_NO_FORCE_SET_CMAKE_BUILD_TYPE is set.
# Finally, don't set the variable if a multi-config generator is used. This can happen
# when qtbase is built with a single config, but a test is built with a multi-config generator.
function(qt_internal_force_set_cmake_build_type_conditionally value)
    # STREQUAL check needs to be expanded variables because an undefined var is not equal to an
    # empty defined var.
    if("${CMAKE_BUILD_TYPE}" STREQUAL "${CMAKE_BUILD_TYPE_INIT}"
        AND NOT __qt_toolchain_cmake_build_type_before_project_call
        AND NOT QT_NO_FORCE_SET_CMAKE_BUILD_TYPE
        AND NOT __qt_internal_extras_is_multi_config)
        set(CMAKE_BUILD_TYPE "${value}" CACHE STRING "Choose the type of build." FORCE)
    endif()
endfunction()

# Extra set of exported variables
set(TEST_architecture_arch "arm64" CACHE INTERNAL "")
set(TEST_subarch_result "" CACHE INTERNAL "")
set(TEST_buildAbi "arm64-little_endian-lp64" CACHE INTERNAL "")
set(TEST_ld_version_script "OFF" CACHE INTERNAL "")

get_property(__qt_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(__qt_is_multi_config)
    set(CMAKE_CONFIGURATION_TYPES "RelWithDebInfo;Debug" CACHE STRING "" FORCE)
    set(CMAKE_TRY_COMPILE_CONFIGURATION "RelWithDebInfo")
endif()
unset(__qt_is_multi_config)

set(QT_MULTI_CONFIG_FIRST_CONFIG "RelWithDebInfo")

if(QT_BUILD_STANDALONE_TESTS)
    qt_internal_force_set_cmake_build_type_conditionally(
        "${QT_MULTI_CONFIG_FIRST_CONFIG}")
endif()
if(CMAKE_GENERATOR STREQUAL "Ninja Multi-Config")
    set(CMAKE_CROSS_CONFIGS "all" CACHE STRING "")
    set(CMAKE_DEFAULT_BUILD_TYPE "RelWithDebInfo" CACHE STRING "")
    set(CMAKE_DEFAULT_CONFIGS "all" CACHE STRING "")
endif()
set(BUILD_WITH_PCH "OFF" CACHE STRING "")
set(QT_QPA_DEFAULT_PLATFORM "windows" CACHE STRING "")
set(CMAKE_INSTALL_RPATH "" CACHE STRING "")

if(NOT QT_SKIP_BUILD_INTERNALS_PKG_CONFIG_FEATURE)
    set(FEATURE_pkg_config "OFF" CACHE BOOL "Using pkg-config" FORCE)
endif()

set(INSTALL_BINDIR "bin" CACHE STRING "Executables [PREFIX/bin]" FORCE)
set(INSTALL_INCLUDEDIR "include" CACHE STRING "Header files [PREFIX/include]" FORCE)
set(INSTALL_LIBDIR "lib" CACHE STRING "Libraries [PREFIX/lib]" FORCE)
set(INSTALL_MKSPECSDIR "mkspecs" CACHE STRING "Mkspecs files [PREFIX/mkspecs]" FORCE)
set(INSTALL_ARCHDATADIR "." CACHE STRING "Arch-dependent data [PREFIX]" FORCE)
set(INSTALL_PLUGINSDIR "./plugins" CACHE STRING "Plugins [ARCHDATADIR/plugins]" FORCE)
set(INSTALL_LIBEXECDIR "./bin" CACHE STRING "Helper programs [ARCHDATADIR/bin on Windows, ARCHDATADIR/libexec otherwise]" FORCE)
set(INSTALL_QMLDIR "./qml" CACHE STRING "QML imports [ARCHDATADIR/qml]" FORCE)
set(INSTALL_DATADIR "." CACHE STRING "Arch-independent data [PREFIX]" FORCE)
set(INSTALL_DOCDIR "./doc" CACHE STRING "Documentation [DATADIR/doc]" FORCE)
set(INSTALL_TRANSLATIONSDIR "./translations" CACHE STRING "Translations [DATADIR/translations]" FORCE)
set(INSTALL_SYSCONFDIR "etc/xdg" CACHE STRING "Settings used by Qt programs [PREFIX/etc/xdg]/[/Library/Preferences/Qt]" FORCE)
set(INSTALL_EXAMPLESDIR "examples" CACHE STRING "Examples [PREFIX/examples]" FORCE)
set(INSTALL_TESTSDIR "tests" CACHE STRING "Tests [PREFIX/tests]" FORCE)
set(INSTALL_DESCRIPTIONSDIR "./modules" CACHE STRING "Module description files directory" FORCE)

if(DEFINED QT_REPO_MODULE_VERSION AND NOT DEFINED QT_REPO_DEPENDENCIES AND NOT QT_SUPERBUILD)
    qt_internal_read_repo_dependencies(QT_REPO_DEPENDENCIES "${PROJECT_SOURCE_DIR}")
endif()

set(QT_COPYRIGHT_YEAR "2023" CACHE STRING "")
set(QT_COPYRIGHT "Copyright (C) 2023 The Qt Company Ltd and other contributors." CACHE STRING "")

