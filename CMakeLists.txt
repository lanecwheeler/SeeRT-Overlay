cmake_minimum_required(VERSION 3.20)
project(SeeRTOverlay VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Windows-only guard
# ---------------------------------------------------------------------------
if (NOT WIN32)
    message(FATAL_ERROR "SeeRT Overlay is Windows-only.")
endif()

# ---------------------------------------------------------------------------
# vcpkg integration
# The VCPKG_ROOT environment variable (or -DCMAKE_TOOLCHAIN_FILE) should point
# to your vcpkg installation.  If using the VS2022 bundled vcpkg, CMake picks
# it up automatically via the toolchain file passed by the IDE.
# ---------------------------------------------------------------------------
find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
set(SOURCES
    src/main.cpp
    src/App.cpp
    src/App.h
    src/capture/WGCCapture.cpp
    src/capture/WGCCapture.h
    src/render/D3DRenderer.cpp
    src/render/D3DRenderer.h
    src/ui/WindowPicker.cpp
    src/ui/WindowPicker.h
    src/ui/MonitorPicker.cpp
    src/ui/MonitorPicker.h
    src/ui/SettingsPanel.cpp
    src/ui/SettingsPanel.h
    src/settings/Settings.cpp
    src/settings/Settings.h
    src/settings/Presets.cpp
    src/settings/Presets.h
    src/audio/AudioLoopback.cpp
    src/audio/AudioLoopback.h
    src/ui/TrayIcon.cpp
    src/ui/TrayIcon.h
    src/resources.rc
)

add_executable(SeeRTOverlay WIN32 ${SOURCES})

# ---------------------------------------------------------------------------
# Windows SDK include paths for WinRT / C++/WinRT headers
# ---------------------------------------------------------------------------

# Locate the Windows SDK root.  CMake's VS generator sets WindowsSdkDir in the
# environment when it invokes the compiler, but we need it at configure time to
# locate fxc.exe and the WinRT headers.
if (NOT DEFINED ENV{WindowsSdkDir})
    # Fall back to the registry-based default for Windows 10 SDK.
    set(WIN_SDK_DIR "C:/Program Files (x86)/Windows Kits/10")
else()
    # Strip trailing backslash that VS injects.
    string(REGEX REPLACE "[/\\\\]+$" "" WIN_SDK_DIR "$ENV{WindowsSdkDir}")
endif()

# Determine the installed SDK version (pick the highest one available).
if (NOT DEFINED ENV{WindowsSdkVersion})
    file(GLOB sdk_versions LIST_DIRECTORIES true "${WIN_SDK_DIR}/Include/*")
    list(SORT sdk_versions ORDER DESCENDING)
    list(GET sdk_versions 0 sdk_ver_path)
    get_filename_component(WIN_SDK_VERSION "${sdk_ver_path}" NAME)
else()
    string(REGEX REPLACE "[/\\\\]+$" "" WIN_SDK_VERSION "$ENV{WindowsSdkVersion}")
endif()

message(STATUS "Windows SDK dir:     ${WIN_SDK_DIR}")
message(STATUS "Windows SDK version: ${WIN_SDK_VERSION}")

# C++/WinRT projection headers live under Include/<version>/cppwinrt.
target_include_directories(SeeRTOverlay PRIVATE
    "${WIN_SDK_DIR}/Include/${WIN_SDK_VERSION}/cppwinrt"
    "${WIN_SDK_DIR}/Include/${WIN_SDK_VERSION}/um"
    "${WIN_SDK_DIR}/Include/${WIN_SDK_VERSION}/shared"
    "${WIN_SDK_DIR}/Include/${WIN_SDK_VERSION}/winrt"
)

# ---------------------------------------------------------------------------
# Compiler options
# ---------------------------------------------------------------------------
target_compile_options(SeeRTOverlay PRIVATE
    /W4             # High warning level
    /WX-            # Warnings are NOT errors (WinRT headers emit some)
    /EHsc           # Standard C++ exception handling
    /MP             # Multi-processor compilation
    /utf-8          # Source and execution charset
    /DUNICODE
    /D_UNICODE
    # Suppress spurious C4324 (structure padding) from the CRTSettings struct
    # which is intentionally padded.
    /wd4324
)

# Suppress the "windows.h included before winrt" warning that some SDK versions emit.
target_compile_definitions(SeeRTOverlay PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00   # Windows 10
    WINVER=0x0A00
)

# ---------------------------------------------------------------------------
# Link libraries
# (D3D11 / DXGI / d3dcompiler are also #pragma comment(lib,...) in the .cpp
#  files, but explicit link here makes CMake-generated project files correct.)
# ---------------------------------------------------------------------------
target_link_libraries(SeeRTOverlay PRIVATE
    imgui::imgui
    nlohmann_json::nlohmann_json
    d3d11
    dxgi
    d3dcompiler
    windowsapp   # C++/WinRT / WinRT runtime
    Shlwapi
    Shell32
)

# ---------------------------------------------------------------------------
# HLSL shader compilation via fxc.exe
# ---------------------------------------------------------------------------

# Locate fxc.exe.  It lives in bin/<arch>/ under the SDK.
if (CMAKE_GENERATOR_PLATFORM STREQUAL "x64" OR
    CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(FXC_ARCH "x64")
else()
    set(FXC_ARCH "x86")
endif()

set(FXC_PATH "${WIN_SDK_DIR}/bin/${WIN_SDK_VERSION}/${FXC_ARCH}/fxc.exe")
if (NOT EXISTS "${FXC_PATH}")
    # Older SDK layout: bin/<arch>/fxc.exe
    set(FXC_PATH "${WIN_SDK_DIR}/bin/${FXC_ARCH}/fxc.exe")
endif()

if (NOT EXISTS "${FXC_PATH}")
    message(FATAL_ERROR
        "fxc.exe not found.  Expected at:\n"
        "  ${WIN_SDK_DIR}/bin/${WIN_SDK_VERSION}/${FXC_ARCH}/fxc.exe\n"
        "Install the Windows 10 SDK with the 'HLSL Tools' component.")
endif()

message(STATUS "fxc.exe: ${FXC_PATH}")

set(SHADER_SRC      "${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/crt.hlsl")
set(VS_CSO          "${CMAKE_CURRENT_BINARY_DIR}/crt_vs.cso")
set(PS_CSO          "${CMAKE_CURRENT_BINARY_DIR}/crt_ps.cso")
set(BLIT_PS_CSO     "${CMAKE_CURRENT_BINARY_DIR}/crt_blit_ps.cso")
set(SPRITE_VS_CSO   "${CMAKE_CURRENT_BINARY_DIR}/sprite_vs.cso")

# Vertex shader
add_custom_command(
    OUTPUT  "${VS_CSO}"
    COMMAND "${FXC_PATH}"
            /nologo /T vs_5_0 /E VS_Main
            /Fo "${VS_CSO}" "${SHADER_SRC}"
    DEPENDS "${SHADER_SRC}"
    COMMENT "fxc: compiling CRT vertex shader"
)

# CRT pixel shader
add_custom_command(
    OUTPUT  "${PS_CSO}"
    COMMAND "${FXC_PATH}"
            /nologo /T ps_5_0 /E PS_Main
            /Fo "${PS_CSO}" "${SHADER_SRC}"
    DEPENDS "${SHADER_SRC}"
    COMMENT "fxc: compiling CRT pixel shader"
)

# Blit (pass-through) pixel shader
add_custom_command(
    OUTPUT  "${BLIT_PS_CSO}"
    COMMAND "${FXC_PATH}"
            /nologo /T ps_5_0 /E PS_Blit
            /Fo "${BLIT_PS_CSO}" "${SHADER_SRC}"
    DEPENDS "${SHADER_SRC}"
    COMMENT "fxc: compiling blit pixel shader"
)

# Sprite vertex shader (positions a textured quad; used for cursor on secondary overlays)
add_custom_command(
    OUTPUT  "${SPRITE_VS_CSO}"
    COMMAND "${FXC_PATH}"
            /nologo /T vs_5_0 /E VS_Sprite
            /Fo "${SPRITE_VS_CSO}" "${SHADER_SRC}"
    DEPENDS "${SHADER_SRC}"
    COMMENT "fxc: compiling sprite vertex shader"
)

# Custom target that the main executable depends on.
add_custom_target(SeeRTOverlay_Shaders ALL
    DEPENDS "${VS_CSO}" "${PS_CSO}" "${BLIT_PS_CSO}" "${SPRITE_VS_CSO}"
)
add_dependencies(SeeRTOverlay SeeRTOverlay_Shaders)

# Copy compiled shaders next to the executable after build.
add_custom_command(TARGET SeeRTOverlay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VS_CSO}"        "$<TARGET_FILE_DIR:SeeRTOverlay>/crt_vs.cso"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PS_CSO}"        "$<TARGET_FILE_DIR:SeeRTOverlay>/crt_ps.cso"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${BLIT_PS_CSO}"   "$<TARGET_FILE_DIR:SeeRTOverlay>/crt_blit_ps.cso"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SPRITE_VS_CSO}" "$<TARGET_FILE_DIR:SeeRTOverlay>/sprite_vs.cso"
    COMMENT "Copying shader .cso files to output directory"
)

# ---------------------------------------------------------------------------
# Cursor asset — copy from the source tree next to the exe at build time.
# ---------------------------------------------------------------------------
set(CURSOR_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/../CRT Monitor Overlay Filter/src/renderer/assets/cursors/cooler arrow.png")
if (EXISTS "${CURSOR_SRC}")
    add_custom_command(TARGET SeeRTOverlay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CURSOR_SRC}"
            "$<TARGET_FILE_DIR:SeeRTOverlay>/cursor.png"
        COMMENT "Copying cursor asset")
else()
    message(WARNING "Cursor asset not found at: ${CURSOR_SRC}")
endif()

# ---------------------------------------------------------------------------
# Font assets — copy TTFs next to the exe so they can be loaded at runtime.
# ---------------------------------------------------------------------------
add_custom_command(TARGET SeeRTOverlay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/src/render/assets/CherryBombOne-Regular.ttf"
        "$<TARGET_FILE_DIR:SeeRTOverlay>/CherryBombOne-Regular.ttf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/src/render/assets/ChakraPetch-Regular.ttf"
        "$<TARGET_FILE_DIR:SeeRTOverlay>/ChakraPetch-Regular.ttf"
    COMMENT "Copying font assets"
)

# ---------------------------------------------------------------------------
# Output directory conventions
# ---------------------------------------------------------------------------
set_target_properties(SeeRTOverlay PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
