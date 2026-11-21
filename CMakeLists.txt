cmake_minimum_required(VERSION 3.16)
project(LoadGraph VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# Windows: the binaries must run on a clean OS install (Windows 7 SP1+) with
# nothing installed by the user.
#
# By default MSVC links the CRT dynamically, so the executables import
# VCRUNTIME140.dll, VCRUNTIME140_1.dll, MSVCP140.dll and the Universal CRT
# api-ms-win-crt-*.dll forwarders.  None of them exist on a fresh Windows
# install (they come with the "Visual C++ Redistributable"), and the process
# simply refuses to start with a missing-DLL error.  Selecting the static
# runtime makes the linker pull the CRT (and the static UCRT) into the .exe,
# so the only remaining imports are base system DLLs.
#
# IMPORTANT: every static library that is linked in must use the SAME runtime.
# The wxWidgets libraries below are rebuilt with the static CRT as well -
# mixing /MT application objects with /MD-built libraries would link two
# independent CRT copies (two heaps, two locale states, two stack-protector
# contexts) which is undefined behaviour.  See README, "Static CRT build".
# ============================================================================
if(MSVC)
    # MultiThreaded = /MT (release), MultiThreadedDebug = /MTd (debug).
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # Windows 7 SP1 is the oldest supported OS.  Pinning the SDK API level makes
    # the headers hide everything newer, so using a Windows 8+ API becomes a
    # compile error instead of a "entry point not found" crash on the target.
    add_compile_definitions(WINVER=0x0601 _WIN32_WINNT=0x0601)
endif()

# Force static library naming convention: lib<name>.a on Linux, <name>.lib on Windows.
set(CMAKE_STATIC_LIBRARY_PREFIX "lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ============================================================================
# wxWidgets configuration (cross-platform) - only needed by LoadGraphGUI.
# ============================================================================
if(DEFINED ENV{WXSTATIC_ROOT})
    set(WXSTATIC_ROOT "$ENV{WXSTATIC_ROOT}")
    message(STATUS "Using static wxWidgets from: ${WXSTATIC_ROOT}")

    # A stale WX_CONFIG_EXECUTABLE left in the CMake cache (for example from a
    # configure run made without WXSTATIC_ROOT) must not re-enable the
    # wx-config branch below: mixing system wx headers with static libraries
    # breaks the build (wxUSE_GRAPHICS_CONTEXT=0 in the distro wx 3.0).
    unset(WX_CONFIG_EXECUTABLE CACHE)

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
        jbig expat pcre2-32 png jpeg tiff lzma z X11 SM xkbcommon gthread-2.0 m dl pthread
    )
    set(wxWidgets_LINK_OPTIONS "-pthread")

elseif(DEFINED ENV{WX_CONFIG})
    # WX_CONFIG points to the wx-config binary itself (e.g. /opt/wx/3.2/bin/wx-config).
    set(WX_CONFIG_EXECUTABLE "$ENV{WX_CONFIG}")
elseif(WIN32)
    set(WX_CONFIG_PATH "C:/wxWidgets" CACHE PATH "wxWidgets installation root")
    # Static wxWidgets libraries built with the STATIC MSVC CRT (/MT), matching
    # CMAKE_MSVC_RUNTIME_LIBRARY above.  The plain "build_static" tree in this
    # checkout was built with /MD and cannot be used any more: linking it into
    # an /MT application would pull in VCRUNTIME140/MSVCP140 again and would
    # create a second CRT copy inside the process.
    set(WX_WIN_BUILD_DIR "build_static_mt" CACHE STRING
        "Static-CRT wxWidgets build directory, relative to WX_CONFIG_PATH")
else()
    find_program(WX_CONFIG_EXECUTABLE wx-config-3.2)
    if(NOT WX_CONFIG_EXECUTABLE)
        find_program(WX_CONFIG_EXECUTABLE wx-config)
    endif()
    if(NOT WX_CONFIG_EXECUTABLE)
        message(FATAL_ERROR "wx-config not found. Install: sudo apt install libwxgtk3.2-dev")
    endif()
endif()

# Run wx-config to extract flags/libs - needed for both WX_CONFIG env and auto-detect.
if(DEFINED WX_CONFIG_EXECUTABLE AND NOT WIN32)
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
    set(WX_LIB_DIR "${WX_CONFIG_PATH}/${WX_WIN_BUILD_DIR}/lib/vc_x64_lib")
    set(wxWidgets_INCLUDE_DIRS
        "${WX_CONFIG_PATH}/include"
        "${WX_LIB_DIR}/mswu"
        "${WX_LIB_DIR}/mswu/wx"
    )
    set(wxWidgets_LIBRARIES
        "${WX_LIB_DIR}/wxmsw32u_core.lib"
        "${WX_LIB_DIR}/wxbase32u.lib"
        "${WX_LIB_DIR}/wxpng.lib"
        "${WX_LIB_DIR}/wxtiff.lib"
        "${WX_LIB_DIR}/wxjpeg.lib"
        "${WX_LIB_DIR}/wxregexu.lib"
        "${WX_LIB_DIR}/wxzlib.lib"
    )

    # Fail early with the exact recipe instead of failing at link time with a
    # wall of unresolved externals.
    set(_wx_missing "")
    foreach(_wx_lib ${wxWidgets_LIBRARIES})
        if(NOT EXISTS "${_wx_lib}")
            string(APPEND _wx_missing "  ${_wx_lib}\n")
        endif()
    endforeach()
    if(_wx_missing)
        message(FATAL_ERROR
            "Static-CRT wxWidgets libraries are missing:\n${_wx_missing}"
            "Build them once with (release only, static CRT):\n"
            "  cmake -S ${WX_CONFIG_PATH} -B ${WX_CONFIG_PATH}/${WX_WIN_BUILD_DIR} "
            "-G \"Visual Studio 17 2022\" -A x64 -DBUILD_SHARED_LIBS=OFF "
            "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded\n"
            "  cmake --build ${WX_CONFIG_PATH}/${WX_WIN_BUILD_DIR} --config Release "
            "--target wxbase wxcore wxpng wxjpeg wxtiff wxzlib wxregex")
    endif()
endif()

# ============================================================================
# Common libraries (shared by all targets).
# ============================================================================
find_library(NVML_LIB nvidia-ml PATHS /usr/lib/x86_64-linux-gnu /usr/local/cuda/lib64 /usr/lib)
if(NVML_LIB)
    message(STATUS "Found NVML: ${NVML_LIB}")
else()
    message(STATUS "NVML not found - GPU monitoring unavailable on this system")
endif()

# ============================================================================
# Common headers (shared by all targets).
# ============================================================================
set(COMMON_HEADERS
    src/platform.h
)

# ============================================================================
# libloadgraph_logger - minimal vfprintf-based logger, wxWidgets-free.
# Linked by all other libs so the static member Logger::s_file is defined once.
# ============================================================================
add_library(loadgraph_logger STATIC src/lib_logger/logger.h src/lib_logger/logger.cpp)
target_include_directories(loadgraph_logger PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib_logger
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ============================================================================
# libloadgraph_config - AppConfig + platform config storage + color_utils.
# Dependencies: logger.
# ============================================================================
set(CONFIG_SOURCES
    src/lib_config/app_config.h
    src/lib_config/app_config.cpp
    src/lib_config/color_utils.h
    src/lib_config/color_utils.cpp
)


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
# libloadgraph_sensors - all hardware sensor polling behind MetricsGet* API.
# Dependencies: logger, platform.  No wxWidgets.
# ============================================================================
set(SENSORS_SOURCES
    src/lib_sensors/metrics_collector.h
    src/lib_sensors/metrics_collector.cpp
    src/lib_sensors/cpu_monitor.h
    src/lib_sensors/cpu_monitor.cpp
    src/lib_sensors/gpu_monitor.h
    src/lib_sensors/gpu_monitor.cpp
    src/lib_sensors/amd_monitor.h
    src/lib_sensors/amd_monitor.cpp
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
    # Windows sensors - all files are used as-is.
elseif(UNIX AND NOT APPLE)
    # Linux sensor implementations replace the Win32-only files above.
    list(REMOVE_ITEM SENSORS_SOURCES
        src/lib_sensors/cpu_monitor.cpp
        src/lib_sensors/gpu_monitor.cpp
        src/lib_sensors/ram_monitor.cpp
        # Windows-only MSRs / ring0 driver / WMI - all code inside #ifdef _WIN32,
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
# libloadgraph_theme - console terminal colors only (wxWidgets-free).
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
# libloadgraph_http - cross-platform HTTP server + web dashboard HTML.
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
# LoadGraphGUI - wxWidgets desktop application.
# ============================================================================
set(GUI_SOURCES
    src/main.cpp
    src/app_signals.h
    src/app_signals.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib_logger
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
    # Subsystem version 6.01 = Windows 7: the PE header then declares the real
    # minimum OS.  /OPT:REF /OPT:ICF drop the unused parts of the statically
    # linked CRT and wxWidgets, keeping the single-file build small.
    target_link_options(LoadGraphGUI PRIVATE
        /SUBSYSTEM:WINDOWS,6.01 /OPT:REF /OPT:ICF)
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
# LoadGraphTTY - console-only binary (htop-like, no wxWidgets).
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
    # Same Windows 7 baseline and dead-code stripping as LoadGraphGUI.
    target_link_options(LoadGraphTTY PRIVATE
        /SUBSYSTEM:CONSOLE,6.01 /OPT:REF /OPT:ICF)
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

# ============================================================================
# Valgrind custom targets (Linux only).
# Usage: cmake --build build_linux --target valgrind-gui
#        cmake --build build_linux --target valgrind-tty
# Output: build_linux/bin/log.log (GUI) / build_linux/bin/log_tty.log (TTY)
# ============================================================================
if(UNIX AND NOT APPLE)
    add_custom_target(valgrind-gui
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_valgrind_gui.sh
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running LoadGraphGUI under Valgrind (output: bin/log.log)"
    )

    add_custom_target(valgrind-tty
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_valgrind_tty.sh
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running LoadGraphTTY under Valgrind (output: bin/log_tty.log)"
    )
endif()
