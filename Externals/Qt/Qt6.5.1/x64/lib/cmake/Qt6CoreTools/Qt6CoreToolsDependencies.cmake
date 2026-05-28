# Find "ModuleTools" dependencies, which are other ModuleTools packages.
set(Qt6CoreTools_FOUND FALSE)
set(__qt_CoreTools_tool_deps "")
foreach(__qt_CoreTools_target_dep ${__qt_CoreTools_tool_deps})
    list(GET __qt_CoreTools_target_dep 0 __qt_CoreTools_pkg)
    list(GET __qt_CoreTools_target_dep 1 __qt_CoreTools_version)

    if (NOT ${__qt_CoreTools_pkg}_FOUND)
        find_dependency(${__qt_CoreTools_pkg} ${__qt_CoreTools_version})
    endif()
endforeach()

set(Qt6CoreTools_FOUND TRUE)
