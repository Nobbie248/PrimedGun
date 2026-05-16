cmake_minimum_required(VERSION 3.16)
project(PrimedGun VERSION 0.9.4)

set(PRIMEDGUN_VERSION_STRING "0.9.4d")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "Windows only")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# OpenXR headers
set(OPENXR_DIR "C:/OpenXR-SDK" CACHE PATH "Path to OpenXR SDK")
find_path(OPENXR_INCLUDE_DIR openxr/openxr.h HINTS ${OPENXR_DIR}/include)
if(NOT OPENXR_INCLUDE_DIR)
    message(FATAL_ERROR "OpenXR SDK headers not found. Expected C:/OpenXR-SDK")
endif()
message(STATUS "OpenXR include: ${OPENXR_INCLUDE_DIR}")

find_path(VULKAN_INCLUDE_DIR vulkan/vulkan.h
    HINTS
        C:/prime-xr/Externals/Vulkan-Headers/include
        C:/Users/Roman/dolphin-vr-no-ovr-VR-Hydra/Externals/Vulkan/Include
        C:/msys64/mingw64/include
)
if(NOT VULKAN_INCLUDE_DIR)
    message(FATAL_ERROR "Vulkan headers not found")
endif()
message(STATUS "Vulkan include: ${VULKAN_INCLUDE_DIR}")

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
    src/PrimedGun.rc
    ${IMGUI_SOURCES}
)

add_executable(PrimedGun WIN32 ${SOURCES})

target_compile_definitions(PrimedGun PRIVATE
    PRIMEDGUN_VERSION_STRING="${PRIMEDGUN_VERSION_STRING}"
)

target_include_directories(PrimedGun PRIVATE
    src/
    src/hook_common
    ${OPENXR_INCLUDE_DIR}
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
    windowscodecs
)

# Dolphin hook sidecar. PrimedGun.exe starts/finds Dolphin, then injects this DLL.
add_library(PrimedGunHookCommon INTERFACE)
target_include_directories(PrimedGunHookCommon INTERFACE
    src/hook_common
    ${OPENXR_INCLUDE_DIR}
    ${VULKAN_INCLUDE_DIR}
)

add_library(PrimedGun_DolphinHook SHARED
    src/dolphin_hook/dllmain.cpp
    src/dolphin_hook/DolphinXrBridge.cpp
    src/dolphin_hook/GameTimingHooks.cpp
    src/dolphin_hook/GraphicsHooks.cpp
    src/dolphin_hook/HookRuntime.cpp
    src/dolphin_hook/Ipc.cpp
    src/dolphin_hook/JitHooks.cpp
    src/dolphin_hook/OpenXrHooks.cpp
    src/dolphin_hook/VulkanHooks.cpp
)
target_link_libraries(PrimedGun_DolphinHook PRIVATE PrimedGunHookCommon)
target_compile_features(PrimedGun_DolphinHook PRIVATE cxx_std_20)
target_compile_definitions(PrimedGun_DolphinHook PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN PRIMEDGUN_DOLPHINHOOK_EXPORTS)
target_link_libraries(PrimedGun_DolphinHook PRIVATE psapi d3d11)
set_target_properties(PrimedGun_DolphinHook PROPERTIES PREFIX "")

# Copy runtime DLLs next to the built binaries for direct build\bin launches.
set(PRIMEDGUN_RUNTIME_TARGETS
    PrimedGun
    PrimedGun_DolphinHook
)
set(D3DCOMPILER_47_DLL "$ENV{SystemRoot}/System32/D3DCompiler_47.dll")
set(PRIMEDGUN_README "${CMAKE_SOURCE_DIR}/README.md")

foreach(RUNTIME_TARGET IN LISTS PRIMEDGUN_RUNTIME_TARGETS)
    add_custom_command(TARGET ${RUNTIME_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:${RUNTIME_TARGET}>/assets"
        COMMENT "Copying PrimedGun assets for ${RUNTIME_TARGET}"
    )

    if(EXISTS "${PRIMEDGUN_README}")
        add_custom_command(TARGET ${RUNTIME_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PRIMEDGUN_README}"
                "$<TARGET_FILE_DIR:${RUNTIME_TARGET}>/README.md"
            COMMENT "Copying README.md for ${RUNTIME_TARGET}"
        )
    endif()

    if(EXISTS "${D3DCOMPILER_47_DLL}")
        add_custom_command(TARGET ${RUNTIME_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${D3DCOMPILER_47_DLL}"
                "$<TARGET_FILE_DIR:${RUNTIME_TARGET}>/D3DCompiler_47.dll"
            COMMENT "Copying D3DCompiler_47.dll for ${RUNTIME_TARGET}"
        )
    endif()
endforeach()

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
            foreach(RUNTIME_TARGET IN LISTS PRIMEDGUN_RUNTIME_TARGETS)
                add_custom_command(TARGET ${RUNTIME_TARGET} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${DLL_PATH}"
                        "$<TARGET_FILE_DIR:${RUNTIME_TARGET}>/${DLL_NAME}"
                    COMMENT "Copying ${DLL_NAME} for ${RUNTIME_TARGET}"
                )
            endforeach()
        else()
            message(WARNING "MinGW runtime DLL not found: ${DLL_PATH}")
        endif()
    endforeach()
endif()
