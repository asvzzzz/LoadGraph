cmake_minimum_required(VERSION 3.16)
project(LoadGraph VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ----------------------------------------------------------------------------
# wxWidgets configuration
# ----------------------------------------------------------------------------
# Prefer environment variable or CMake cache entry; fall back to common paths.
if(NOT DEFINED WX_DIR)
    set(WX_DIR "C:/wxWidgets" CACHE PATH "wxWidgets installation root")
endif()

set(wxWidgets_INCLUDE_DIRS
    "${WX_DIR}/include"
    "${WX_DIR}/build_static/lib/vc_x64_lib/mswu"
    "${WX_DIR}/build_static/lib/vc_x64_lib/mswu/wx"
)
set(wxWidgets_LIBRARIES
    "${WX_DIR}/build_static/lib/vc_x64_lib/wxmsw32u_core.lib"
    "${WX_DIR}/build_static/lib/vc_x64_lib/wxbase32u.lib"
    "${WX_DIR}/build_static/lib/vc_x64_lib/wxmsw32u_gl.lib"
    "${WX_DIR}/build_static/lib/vc_x64_lib/wxpng.lib"
    "${WX_DIR}/build_static/lib/vc_x64_lib/wxtiff.lib"
    "${WX_DIR}/build_static/lib/vc_x64_lib/wxjpeg.lib"
    "${WX_DIR}/build_static/lib/vc_x64_lib/wxregexu.lib"
    "${WX_DIR}/build_static/lib/vc_x64_lib/wxzlib.lib"
)

# Windows system libraries.
set(WINDOWS_LIBS ws2_32 rpcrt4 odbc32 shlwapi comctl32 ole32 oleaut32 uuid winmm psapi advapi32 dwmapi)

# ----------------------------------------------------------------------------
# Sources
# ----------------------------------------------------------------------------
set(SOURCES
    src/main.cpp
    src/logger.h
    src/logger.cpp
    src/metrics_collector.h
    src/metrics_collector.cpp
    src/cpu_monitor.h
    src/cpu_monitor.cpp
    src/ram_monitor.h
    src/ram_monitor.cpp
    src/gpu_monitor.h
    src/gpu_monitor.cpp
    src/graph_panel.h
    src/graph_panel.cpp
    src/theme.h
    src/theme.cpp
    src/main_frame.h
    src/main_frame.cpp
)

# ----------------------------------------------------------------------------
# Executable
# ----------------------------------------------------------------------------
add_executable(${PROJECT_NAME} WIN32 ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    ${wxWidgets_INCLUDE_DIRS}
    "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/shared"
    "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/um"
)

target_link_libraries(${PROJECT_NAME}
    ${wxWidgets_LIBRARIES}
    wbemuuid
    pdh
    psapi
    ${WINDOWS_LIBS}
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ----------------------------------------------------------------------------
# MSVC-specific settings
# ----------------------------------------------------------------------------
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX-)
    target_link_options(${PROJECT_NAME} PRIVATE /SUBSYSTEM:WINDOWS)
endif()

# ----------------------------------------------------------------------------
# MinGW-specific settings
# ----------------------------------------------------------------------------
if(MINGW)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()
