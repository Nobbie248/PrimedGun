# Find "ModuleTools" dependencies, which are other ModuleTools packages.
set(Qt6WidgetsTools_FOUND FALSE)
set(__qt_WidgetsTools_tool_deps "Qt6CoreTools\;6.5.1;Qt6GuiTools\;6.5.1")
foreach(__qt_WidgetsTools_target_dep ${__qt_WidgetsTools_tool_deps})
    list(GET __qt_WidgetsTools_target_dep 0 __qt_WidgetsTools_pkg)
    list(GET __qt_WidgetsTools_target_dep 1 __qt_WidgetsTools_version)

    if (NOT ${__qt_WidgetsTools_pkg}_FOUND)
        find_dependency(${__qt_WidgetsTools_pkg} ${__qt_WidgetsTools_version})
    endif()
endforeach()

set(Qt6WidgetsTools_FOUND TRUE)
