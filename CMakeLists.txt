cmake_minimum_required(VERSION 3.16)
project(LoadGraph VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Force static library naming convention: lib<name>.a on Linux, <name>.lib on Windows.
set(CMAKE_STATIC_LIBRARY_PREFIX "lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ============================================================================
# wxWidgets configuration (cross-platform) — only needed by LoadGraphGUI.
# ============================================================================
if(DEFINED ENV{WXSTATIC_ROOT})
    set(WXSTATIC_ROOT "$ENV{WXSTATIC_ROOT}")
    message(STATUS "Using static wxWidgets from: ${WXSTATIC_ROOT}")

    set(wxWidgets_INCLUDE_DIRS
        "${WXSTATIC_ROOT}/include"
        "${WXSTATIC_ROOT}/include/gtk3-unicode-static-3.2"
    )
    set(wxWidgets_LIBRARY_DIRS "${WXSTATIC_ROOT}")
    set(wxWidgets_LIBRARIES
        "${WXSTATIC_ROOT}/libwx_gtk3u_core-3.2.a"
        "${WXSTATIC_ROOT}/libwx_baseu-3.2.a"
        "${WXSTATIC_ROOT}/libwx_baseu_net-3.2.a"
        "${WXSTATIC_ROOT}/libwx_baseu_xml-3.2.a"
    )
    set(wxWidgets_COMPILE_OPTIONS "-D_FILE_OFFSET_BITS=64;-DwxDEBUG_LEVEL=0;-D__WXGTK__;-pthread")
    set(wxWidgets_SYSTEM_LIBS
        gtk-3 gdk-3 pangocairo-1.0 atk-1.0 cairo-gobject cairo
        gdk_pixbuf-2.0 gio-2.0 Xtst pangoft2-1.0 pango-1.0
        gobject-2.0 glib-2.0 harfbuzz fontconfig freetype
        jbig expat pcre2-32 png jpeg lzma z X11 SM xkbcommon gthread-2.0 m dl pthread
    )
    set(wxWidgets_LINK_OPTIONS "-pthread")

elseif(DEFINED ENV{WX_CONFIG})
    # WX_CONFIG points to the wx-config binary itself (e.g. /opt/wx/3.2/bin/wx-config).
    set(WX_CONFIG_EXECUTABLE "$ENV{WX_CONFIG}")
elseif(WIN32)
    set(WX_CONFIG_PATH "C:/wxWidgets" CACHE PATH "wxWidgets installation root")
else()
    find_program(WX_CONFIG_EXECUTABLE wx-config-3.2)
    if(NOT WX_CONFIG_EXECUTABLE)
        find_program(WX_CONFIG_EXECUTABLE wx-config)
    endif()
    if(NOT WX_CONFIG_EXECUTABLE)
        message(FATAL_ERROR "wx-config not found. Install: sudo apt install libwxgtk3.2-dev")
    endif()
    message(STATUS "Using wx-config: ${WX_CONFIG_EXECUTABLE}")

    execute_process(COMMAND ${WX_CONFIG_EXECUTABLE} --cflags
                   OUTPUT_VARIABLE WX_CFLAGS RESULT_VARIABLE WX_CFLAGS_RESULT)
    if(NOT WX_CFLAGS_RESULT EQUAL 0)
        message(FATAL_ERROR "wx-config --cflags failed")
    endif()
    string(STRIP "${WX_CFLAGS}" WX_CFLAGS)

    execute_process(COMMAND ${WX_CONFIG_EXECUTABLE} --libs
                   OUTPUT_VARIABLE WX_LIBS_OUTPUT RESULT_VARIABLE WX_LIBS_RESULT)
    if(NOT WX_LIBS_RESULT EQUAL 0)
        message(FATAL_ERROR "wx-config --libs failed")
    endif()
    string(STRIP "${WX_LIBS_OUTPUT}" WX_LIBS_OUTPUT)

    separate_arguments(WX_CFLAGS_LIST NATIVE_COMMAND "${WX_CFLAGS}")
    separate_arguments(WX_LIBS_LIST NATIVE_COMMAND "${WX_LIBS_OUTPUT}")

    foreach(arg ${WX_CFLAGS_LIST})
        if(arg MATCHES "^-I(.+)$")
            list(APPEND wxWidgets_INCLUDE_DIRS "${CMAKE_MATCH_1}")
        else()
            list(APPEND wxWidgets_COMPILE_OPTIONS "${arg}")
        endif()
    endforeach()

    foreach(arg ${WX_LIBS_LIST})
        if(arg MATCHES "^-l(.+)$")
            list(APPEND wxWidgets_LIBRARIES "${CMAKE_MATCH_1}")
        elseif(arg MATCHES "^-L(.+)$")
            list(APPEND wxWidgets_LIBRARY_DIRS "${CMAKE_MATCH_1}")
        else()
            list(APPEND wxWidgets_LINK_OPTIONS "${arg}")
        endif()
    endforeach()
endif()

if(WIN32 AND DEFINED WX_CONFIG_PATH)
    set(wxWidgets_INCLUDE_DIRS
        "${WX_CONFIG_PATH}/include"
        "${WX_CONFIG_PATH}/build_static/lib/vc_x64_lib/mswu"
        "${WX_CONFIG_PATH}/build_static/lib/vc_x64_lib/mswu/wx"
    )
    set(wxWidgets_LIBRARIES
        "${WX_CONFIG_PATH}/build_static/lib/vc_x64_lib/wxmsw32u_core.lib"
        "${WX_CONFIG_PATH}/build_static/lib/vc_x64_lib/wxbase32u.lib"
        "${WX_CONFIG_PATH}/build_static/lib/vc_x64_lib/wxpng.lib"
        "${WX_CONFIG_PATH}/build_static/lib/vc_x64_lib/wxtiff.lib"
        "${WX_CONFIG_PATH}/build_static/lib/vc_x64_lib/wxjpeg.lib"
        "${WX_CONFIG_PATH}/build_static/lib/vc_x64_lib/wxregexu.lib"
        "${WX_CONFIG_PATH}/build_static/lib/vc_x64_lib/wxzlib.lib"
    )
endif()

# ============================================================================
# Common libraries (shared by all targets).
# ============================================================================
find_library(NVML_LIB nvidia-ml PATHS /usr/lib/x86_64-linux-gnu /usr/local/cuda/lib64 /usr/lib)
if(NVML_LIB)
    message(STATUS "Found NVML: ${NVML_LIB}")
else()
    message(STATUS "NVML not found — GPU monitoring unavailable on this system")
endif()

# ============================================================================
# Common headers (shared by all targets).
# ============================================================================
set(COMMON_HEADERS
    src/platform.h
)

# ============================================================================
# libloadgraph_logger — minimal vfprintf-based logger, wxWidgets-free.
# Linked by all other libs so the static member Logger::s_file is defined once.
# ============================================================================
add_library(loadgraph_logger STATIC src/lib_logger/logger.h src/lib_logger/logger.cpp)
target_include_directories(loadgraph_logger PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib_logger
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ============================================================================
# libloadgraph_config — AppConfig + platform config storage + color_utils.
# Dependencies: logger.
# ============================================================================
set(CONFIG_SOURCES
    src/lib_config/app_config.h
    src/lib_config/app_config.cpp
    src/lib_config/color_utils.h
    src/lib_config/color_utils.cpp
)
if(WIN32)
    list(APPEND CONFIG_SOURCES src/lib_config/config_win.cpp)
elseif(UNIX AND NOT APPLE)
    list(APPEND CONFIG_SOURCES src/lib_config/config_linux.cpp)
endif()

add_library(loadgraph_config STATIC ${CONFIG_SOURCES} ${COMMON_HEADERS})
target_include_directories(loadgraph_config PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib_config
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(loadgraph_config PRIVATE loadgraph_logger)
if(WIN32)
    target_compile_definitions(loadgraph_config PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ============================================================================
# libloadgraph_sensors — all hardware sensor polling behind MetricsGet* API.
# Dependencies: logger, platform.  No wxWidgets.
# ============================================================================
set(SENSORS_SOURCES
    src/lib_sensors/metrics_collector.h
    src/lib_sensors/metrics_collector.cpp
    src/lib_sensors/cpu_monitor.h
    src/lib_sensors/cpu_monitor.cpp
    src/lib_sensors/gpu_monitor.h
    src/lib_sensors/gpu_monitor.cpp
    src/lib_sensors/ram_monitor.h
    src/lib_sensors/ram_monitor.cpp
    src/lib_sensors/storage_monitor.h
    src/lib_sensors/storage_monitor.cpp
    src/lib_sensors/power_table.h
    src/lib_sensors/power_table.cpp
    src/lib_sensors/metrics_api.h
    src/lib_sensors/metrics_api.cpp
    src/lib_sensors/msr_sensor.h
    src/lib_sensors/msr_sensor.cpp
    src/lib_sensors/ring0_msr.h
    src/lib_sensors/ring0_msr.cpp
)
if(WIN32)
    # Windows sensors — all files are used as-is.
elseif(UNIX AND NOT APPLE)
    # Linux sensor implementations replace the Win32-only files above.
    list(REMOVE_ITEM SENSORS_SOURCES
        src/lib_sensors/cpu_monitor.cpp
        src/lib_sensors/gpu_monitor.cpp
        src/lib_sensors/ram_monitor.cpp
        # Windows-only MSRs / ring0 driver / WMI — all code inside #ifdef _WIN32,
        # so on Linux they compile to empty TUs and leave missing symbols.
        src/lib_sensors/msr_sensor.cpp
        src/lib_sensors/ring0_msr.cpp
    )
    list(APPEND SENSORS_SOURCES
        src/lib_sensors/cpu_monitor_linux.cpp
        src/lib_sensors/gpu_monitor_linux.cpp
        src/lib_sensors/ram_monitor_linux.cpp
    )
endif()

add_library(loadgraph_sensors STATIC ${SENSORS_SOURCES} ${COMMON_HEADERS})
target_include_directories(loadgraph_sensors PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib_sensors
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# lib_config + logger are dependencies of lib_sensors.
target_link_libraries(loadgraph_sensors PRIVATE loadgraph_config loadgraph_logger)
if(WIN32)
    target_compile_definitions(loadgraph_sensors PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ============================================================================
# libloadgraph_theme — console terminal colors only (wxWidgets-free).
# GUI-specific theme (theme.cpp / theme_linux.cpp) is compiled directly into
# LoadGraphGUI since it depends on wxColour.
# Dependencies: lib_config (HexToRgb).
# ============================================================================
set(THEME_SOURCES
    src/lib_theme/console_colors.h
    src/lib_theme/console_colors.cpp
)

add_library(loadgraph_theme STATIC ${THEME_SOURCES} ${COMMON_HEADERS})
target_include_directories(loadgraph_theme PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib_theme
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(loadgraph_theme PRIVATE loadgraph_config loadgraph_logger)
if(WIN32)
    target_compile_definitions(loadgraph_theme PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ============================================================================
# libloadgraph_http — cross-platform HTTP server + web dashboard HTML.
# Dependencies: lib_sensors (MetricsGet* API), lib_config, logger.
# ============================================================================
set(HTTP_SOURCES
    src/lib_http/http_server.h
    src/lib_http/http_server.cpp
    src/lib_http/dashboard_html.h
    src/lib_http/dashboard_html.cpp
)

add_library(loadgraph_http STATIC ${HTTP_SOURCES} ${COMMON_HEADERS})
target_include_directories(loadgraph_http PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib_http
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(loadgraph_http PRIVATE loadgraph_sensors loadgraph_config loadgraph_logger)
if(WIN32)
    target_compile_definitions(loadgraph_http PRIVATE _CRT_SECURE_NO_WARNINGS USE_HTTP_SERVER)
endif()

# ============================================================================
# LoadGraphGUI — wxWidgets desktop application.
# ============================================================================
set(GUI_SOURCES
    src/main.cpp
    src/main_frame.h
    src/main_frame.cpp
    src/graph_panel.h
    src/graph_panel.cpp
    src/lib_theme/theme.h
)
if(WIN32)
    list(APPEND GUI_SOURCES src/lib_theme/theme.cpp)
elseif(UNIX AND NOT APPLE)
    list(APPEND GUI_SOURCES src/lib_theme/theme_linux.cpp)
endif()

add_executable(LoadGraphGUI ${GUI_SOURCES} ${COMMON_HEADERS})
target_include_directories(LoadGraphGUI PRIVATE
    src
    ${wxWidgets_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib_sensors
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib_http
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib_config
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib_theme
)
if(WIN32)
    target_link_libraries(LoadGraphGUI
        loadgraph_logger
        loadgraph_sensors
        loadgraph_config
        loadgraph_theme
        loadgraph_http
        ${wxWidgets_LIBRARIES}
        wbemuuid pdh psapi
        ws2_32 rpcrt4 odbc32 shlwapi shell32 comctl32 ole32 oleaut32 uuid winmm advapi32 dwmapi
    )
    set_target_properties(LoadGraphGUI PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        WIN32_EXECUTABLE ON
    )

    # Embed icon as compiled resource.
    # Using CMake's native RC support instead of manual execute_process:
    #   - correct CWD so relative paths in .rc resolve properly
    #   - include dirs from target_include_directories passed to rc.exe via /I
    #   - automatic rebuild when loadgraph.ico changes (dependency tracking)
    enable_language(RC)
    target_include_directories(LoadGraphGUI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_sources(LoadGraphGUI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/icon.rc")

    target_compile_definitions(LoadGraphGUI PRIVATE _CRT_SECURE_NO_WARNINGS USE_HTTP_SERVER)
    target_compile_options(LoadGraphGUI PRIVATE /W4 /WX-)
    target_link_options(LoadGraphGUI PRIVATE /SUBSYSTEM:WINDOWS)
else()
    # Linux build.
    target_link_libraries(LoadGraphGUI
        loadgraph_sensors
        loadgraph_config
        loadgraph_theme
        loadgraph_http
        ${wxWidgets_LIBRARIES}
        ${wxWidgets_SYSTEM_LIBS}
        pthread dl
    )
    target_compile_options(LoadGraphGUI PRIVATE ${wxWidgets_COMPILE_OPTIONS} -Wall -Wextra -Wpedantic)
    target_link_options(LoadGraphGUI PRIVATE ${wxWidgets_LINK_OPTIONS})
    if(NVML_LIB)
        target_link_libraries(LoadGraphGUI ${NVML_LIB})
    endif()

    set_target_properties(LoadGraphGUI PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

# ============================================================================
# LoadGraphTTY — console-only binary (htop-like, no wxWidgets).
# ============================================================================
set(TTY_SOURCES
    src/console_app.cpp
)

add_executable(LoadGraphTTY ${TTY_SOURCES})
target_include_directories(LoadGraphTTY PRIVATE
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib_sensors
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib_http
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib_theme
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib_config
)

if(WIN32)
    target_link_libraries(LoadGraphTTY
        loadgraph_logger
        loadgraph_sensors
        loadgraph_config
        loadgraph_theme
        loadgraph_http
        wbemuuid pdh psapi ws2_32 rpcrt4 odbc32 shlwapi shell32 comctl32 ole32 oleaut32 uuid winmm advapi32
    )
    set_target_properties(LoadGraphTTY PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    target_compile_definitions(LoadGraphTTY PRIVATE _CRT_SECURE_NO_WARNINGS USE_HTTP_SERVER)
    target_compile_options(LoadGraphTTY PRIVATE /W4 /WX-)
else()
    target_link_libraries(LoadGraphTTY
        loadgraph_logger
        loadgraph_sensors
        loadgraph_config
        loadgraph_theme
        loadgraph_http
        pthread dl
    )
    if(NVML_LIB)
        target_link_libraries(LoadGraphTTY ${NVML_LIB})
    endif()
    set_target_properties(LoadGraphTTY PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    target_compile_options(LoadGraphTTY PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
