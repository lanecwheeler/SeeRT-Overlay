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

set(SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/crt.hlsl")

# Each entry: <output name>|<profile>|<entry point>.  All come from crt.hlsl.
set(SHADER_ENTRIES
    "crt_vs|vs_5_0|VS_Main"               # full-screen triangle
    "sprite_vs|vs_5_0|VS_Sprite"          # positioned quad (cursor sprites)
    "crt_ps|ps_5_0|PS_Main"               # CRT effect
    "crt_blit_ps|ps_5_0|PS_Blit"          # pass-through copy
    "composite_ps|ps_5_0|PS_Composite"    # composite signal bandwidth limits
    "persist_ps|ps_5_0|PS_Persist"        # phosphor persistence (frame history)
    "lum_down_ps|ps_5_0|PS_LumDown"       # scene luminance grid (raster bloom)
    "lum_smooth_ps|ps_5_0|PS_LumSmooth"   # temporal smoothing of average luminance
    "glow_down_ps|ps_5_0|PS_GlowDown"     # linearise + 1/4 downsample
    "halo_down_ps|ps_5_0|PS_HaloDown"     # threshold + 1/8 downsample
    "blur_ps|ps_5_0|PS_Blur"              # separable Gaussian blur
)

set(SHADER_OUTPUTS)
set(SHADER_COPY_COMMANDS)
foreach(entry IN LISTS SHADER_ENTRIES)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 name)
    list(GET parts 1 profile)
    list(GET parts 2 entry_point)
    set(cso "${CMAKE_CURRENT_BINARY_DIR}/${name}.cso")

    add_custom_command(
        OUTPUT  "${cso}"
        COMMAND "${FXC_PATH}" /nologo /T ${profile} /E ${entry_point} /Fo "${cso}" "${SHADER_SRC}"
        DEPENDS "${SHADER_SRC}"
        COMMENT "fxc: compiling ${entry_point} (${name}.cso)"
    )
    list(APPEND SHADER_OUTPUTS "${cso}")
    list(APPEND SHADER_COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${cso}" "$<TARGET_FILE_DIR:SeeRTOverlay>/${name}.cso")
endforeach()

# Custom target that the main executable depends on.
add_custom_target(SeeRTOverlay_Shaders ALL DEPENDS ${SHADER_OUTPUTS})
add_dependencies(SeeRTOverlay SeeRTOverlay_Shaders)

# Copy compiled shaders next to the executable after build.
add_custom_command(TARGET SeeRTOverlay POST_BUILD
    ${SHADER_COPY_COMMANDS}
    COMMENT "Copying shader .cso files to output directory"
)

# ---------------------------------------------------------------------------
# Cursor asset — copy the default cursor image next to the exe.
# ---------------------------------------------------------------------------
add_custom_command(TARGET SeeRTOverlay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/src/render/assets/cursor.png"
        "$<TARGET_FILE_DIR:SeeRTOverlay>/cursor.png"
    COMMENT "Copying cursor asset"
)

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
