include_guard(DIRECTORY)


if(NOT QT_NO_CREATE_TARGETS AND NOT QT_SKIP_AUTO_PLUGIN_INCLUSION)
    __qt_internal_include_plugin_packages(Widgets)
endif()
