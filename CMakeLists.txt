cmake_minimum_required(VERSION 3.16)
project(PrimedGun VERSION 0.9.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "Windows only")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── OpenVR ────────────────────────────────────────────────
set(OPENVR_DIR "C:/openvr" CACHE PATH "Path to OpenVR SDK")
find_path(OPENVR_INCLUDE_DIR openvr.h HINTS ${OPENVR_DIR}/headers)
find_library(OPENVR_LIB openvr_api HINTS ${OPENVR_DIR}/lib/win64)

if(NOT OPENVR_INCLUDE_DIR OR NOT OPENVR_LIB)
    message(FATAL_ERROR "OpenVR not found. Expected C:/openvr (git clone https://github.com/ValveSoftware/openvr C:/openvr)")
endif()
message(STATUS "OpenVR include: ${OPENVR_INCLUDE_DIR}")
message(STATUS "OpenVR lib:     ${OPENVR_LIB}")

# ── ImGui ─────────────────────────────────────────────────
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/vendor/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

# ── Sources ───────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    ${IMGUI_SOURCES}
)

add_executable(PrimedGun WIN32 ${SOURCES})

target_include_directories(PrimedGun PRIVATE
    src/
    ${OPENVR_INCLUDE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(PrimedGun
    d3d11
    dxgi
    d3dcompiler_47
    dwmapi
    psapi
    winmm
    ${OPENVR_LIB}
)

# Copy openvr_api.dll next to exe for distribution
set(OPENVR_DLL "${OPENVR_DIR}/bin/win64/openvr_api.dll")
if(EXISTS "${OPENVR_DLL}")
    add_custom_command(TARGET PrimedGun POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENVR_DLL}"
            "$<TARGET_FILE_DIR:PrimedGun>/openvr_api.dll"
        COMMENT "Copying openvr_api.dll"
    )
endif()

# Copy MinGW runtime DLLs next to exe for distribution builds.
if(MINGW)
    get_filename_component(MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(MINGW_RUNTIME_DLLS
        libgcc_s_seh-1.dll
        libstdc++-6.dll
        libwinpthread-1.dll
    )

    foreach(DLL_NAME IN LISTS MINGW_RUNTIME_DLLS)
        set(DLL_PATH "${MINGW_BIN_DIR}/${DLL_NAME}")
        if(EXISTS "${DLL_PATH}")
            add_custom_command(TARGET PrimedGun POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DLL_PATH}"
                    "$<TARGET_FILE_DIR:PrimedGun>/${DLL_NAME}"
                COMMENT "Copying ${DLL_NAME}"
            )
        else()
            message(WARNING "MinGW runtime DLL not found: ${DLL_PATH}")
        endif()
    endforeach()
endif()
