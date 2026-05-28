if(NOT DEFINED QT_DEFAULT_MAJOR_VERSION)
    set(QT_DEFAULT_MAJOR_VERSION 6)
endif()

# include(\"${CMAKE_CURRENT_LIST_DIR}/Qt5CoreConfigExtrasMkspecDir.cmake\")
#
# foreach(_dir ${_qt5_corelib_extra_includes})
#     _qt5_Core_check_file_exists(${_dir})
# endforeach()

# list(APPEND Qt5Core_INCLUDE_DIRS ${_qt5_corelib_extra_includes})
# set_property(TARGET Qt6::Core APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${_qt5_corelib_extra_includes})
# set(_qt5_corelib_extra_includes)

if (NOT QT_NO_CREATE_TARGETS)
    set(__qt_core_target Qt6::Core)
    get_property(__qt_core_aliased_target TARGET ${__qt_core_target} PROPERTY ALIASED_TARGET)
    if(__qt_core_aliased_target)
        set(__qt_core_target "${__qt_core_aliased_target}")
    endif()
    unset(__qt_core_aliased_target)
    if (NOT "" STREQUAL "")
        set_property(TARGET ${__qt_core_target} APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS QT_NAMESPACE=)
    endif()
    set_property(TARGET ${__qt_core_target} APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS $<$<NOT:$<CONFIG:Debug>>:QT_NO_DEBUG>)
    set_property(TARGET ${__qt_core_target} PROPERTY INTERFACE_COMPILE_FEATURES cxx_decltype)
endif()

list(APPEND CMAKE_AUTOMOC_MACRO_NAMES Q_OBJECT Q_GADGET Q_GADGET_EXPORT Q_NAMESPACE Q_NAMESPACE_EXPORT)
list(REMOVE_DUPLICATES CMAKE_AUTOMOC_MACRO_NAMES)

include("${CMAKE_CURRENT_LIST_DIR}/QtInstallPaths.cmake")

set(QT6_IS_SHARED_LIBS_BUILD "ON")

set(_Qt6CTestMacros "${CMAKE_CURRENT_LIST_DIR}/Qt6CTestMacros.cmake")

_qt_internal_setup_deploy_support()



if(ANDROID_PLATFORM)
    include("${CMAKE_CURRENT_LIST_DIR}/Qt6AndroidMacros.cmake")
    _qt_internal_create_global_android_targets()
    _qt_internal_collect_default_android_abis()
    if(NOT QT_NO_CREATE_TARGETS)
        set_property(TARGET ${__qt_core_target} APPEND PROPERTY
            INTERFACE_QT_EXECUTABLE_FINALIZERS
                _qt_internal_android_executable_finalizer
        )
    endif()
endif()

if(QT_FEATURE_permissions AND APPLE)
    if(NOT QT_NO_CREATE_TARGETS)
        set_property(TARGET ${__qt_core_target} APPEND PROPERTY
            INTERFACE_QT_EXECUTABLE_FINALIZERS
                _qt_internal_darwin_permission_finalizer
        )
    endif()
endif()

if(EMSCRIPTEN)
    set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
    include("${CMAKE_CURRENT_LIST_DIR}/Qt6WasmMacros.cmake")
endif()

_qt_internal_override_example_install_dir_to_dot()

unset(__qt_core_target)
