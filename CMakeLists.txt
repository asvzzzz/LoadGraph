cmake_minimum_required(VERSION 3.16)
project(LoadGraph VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ----------------------------------------------------------------------------
# wxWidgets configuration (cross-platform)
# ----------------------------------------------------------------------------
if(DEFINED ENV{WXSTATIC_ROOT})
    # Static wxWidgets build (built from source).
    set(WXSTATIC_ROOT "$ENV{WXSTATIC_ROOT}")
    message(STATUS "Using static wxWidgets from: ${WXSTATIC_ROOT}")

    # Layout: <WXSTATIC_ROOT>/{libwx*.a, include/wx/*.h, include/gtk3-unicode-static-3.2/wx/setup.h}
    set(wxWidgets_INCLUDE_DIRS
        "${WXSTATIC_ROOT}/include"
        "${WXSTATIC_ROOT}/include/gtk3-unicode-static-3.2"
    )
    set(wxWidgets_LIBRARY_DIRS "${WXSTATIC_ROOT}")
    # wxWidgets static libraries required by the current feature set:
    #   core       — all widgets, scrolled windows, sizers (mandatory)
    #   base       — fundamental classes (strings, containers, IO)
    #   base_net   — wxWebRequest / networking primitives
    #   base_xml   — wxXmlDocument / config parsing
    # If you add features that pull in wxHtml, wxAui, wxMedia or wxImage with
    # handlers (PNG/JPEG), extend this list accordingly — otherwise linking
    # will fail with undefined references at link time.
    set(wxWidgets_LIBRARIES
        "${WXSTATIC_ROOT}/libwx_gtk3u_core-3.2.a"
        "${WXSTATIC_ROOT}/libwx_baseu-3.2.a"
        "${WXSTATIC_ROOT}/libwx_baseu_net-3.2.a"
        "${WXSTATIC_ROOT}/libwx_baseu_xml-3.2.a"
    )
    set(wxWidgets_COMPILE_OPTIONS "-D_FILE_OFFSET_BITS=64;-DwxDEBUG_LEVEL=0;-D__WXGTK__;-pthread")
    # System libraries required by static wxWidgets (GTK3 stack).
    set(wxWidgets_SYSTEM_LIBS
        gtk-3 gdk-3 pangocairo-1.0 atk-1.0 cairo-gobject cairo
        gdk_pixbuf-2.0 gio-2.0 Xtst pangoft2-1.0 pango-1.0
        gobject-2.0 glib-2.0 harfbuzz fontconfig freetype
        jbig expat pcre2-32 png jpeg lzma z X11 SM xkbcommon gthread-2.0 m dl pthread
    )
    set(wxWidgets_LINK_OPTIONS "-pthread")

    message(STATUS "Static wxWidgets include: ${wxWidgets_INCLUDE_DIRS}")
    message(STATUS "Static wxWidgets libs: ${wxWidgets_LIBRARIES}")
elseif(DEFINED ENV{WX_CONFIG})
    set(WX_CONFIG_PATH "$ENV{WX_CONFIG}")
elseif(WIN32)
    # Windows: prefer static build at known location.
    set(WX_CONFIG_PATH "C:/wxWidgets" CACHE PATH "wxWidgets installation root")
else()
    # Linux dynamic: parse wx-config (system wxWidgets).
    find_program(WX_CONFIG_EXECUTABLE wx-config-3.2)
    if(NOT WX_CONFIG_EXECUTABLE)
        find_program(WX_CONFIG_EXECUTABLE wx-config)
    endif()
    if(NOT WX_CONFIG_EXECUTABLE)
        message(FATAL_ERROR "wx-config not found. Install: sudo apt install libwxgtk3.2-dev")
    endif()
    message(STATUS "Using wx-config: ${WX_CONFIG_EXECUTABLE}")

    execute_process(COMMAND ${WX_CONFIG_EXECUTABLE} --cflags
                   OUTPUT_VARIABLE WX_CFLAGS
                   RESULT_VARIABLE WX_CFLAGS_RESULT)
    if(NOT WX_CFLAGS_RESULT EQUAL 0)
        message(FATAL_ERROR "wx-config --cflags failed")
    endif()
    string(STRIP "${WX_CFLAGS}" WX_CFLAGS)
    message(STATUS "wxWidgets cflags: ${WX_CFLAGS}")

    execute_process(COMMAND ${WX_CONFIG_EXECUTABLE} --libs
                   OUTPUT_VARIABLE WX_LIBS_OUTPUT
                   RESULT_VARIABLE WX_LIBS_RESULT)
    if(NOT WX_LIBS_RESULT EQUAL 0)
        message(FATAL_ERROR "wx-config --libs failed")
    endif()
    string(STRIP "${WX_LIBS_OUTPUT}" WX_LIBS_OUTPUT)
    message(STATUS "wxWidgets libs: ${WX_LIBS_OUTPUT}")

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

    message(STATUS "wxWidgets include dirs: ${wxWidgets_INCLUDE_DIRS}")
    message(STATUS "wxWidgets compile options: ${wxWidgets_COMPILE_OPTIONS}")
    message(STATUS "wxWidgets libraries: ${wxWidgets_LIBRARIES}")
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
        "${WX_CONFIG_PATH}/build_static/lib/vc_x64_lib/wxmsw32u_gl.lib"
        "${WX_CONFIG_PATH}/build_static/lib/vc_x64_lib/wxpng.lib"
        "${WX_CONFIG_PATH}/build_static/lib/vc_x64_lib/wxtiff.lib"
        "${WX_CONFIG_PATH}/build_static/lib/vc_x64_lib/wxjpeg.lib"
        "${WX_CONFIG_PATH}/build_static/lib/vc_x64_lib/wxregexu.lib"
        "${WX_CONFIG_PATH}/build_static/lib/vc_x64_lib/wxzlib.lib"
    )
endif()

# ----------------------------------------------------------------------------
# Sources (platform-specific files included via conditionals)
# ----------------------------------------------------------------------------
set(SOURCES
    src/main.cpp
    src/icon_data.h
    src/logger.h
    src/logger.cpp
    src/platform.h
    src/metrics_collector.h
    src/metrics_collector.cpp
    src/cpu_monitor.h
    src/config.h
)

if(WIN32)
    list(APPEND SOURCES
        src/cpu_monitor.cpp
        src/msr_sensor.h
        src/msr_sensor.cpp
        src/ring0_msr.h
        src/ring0_msr.cpp
        src/ram_monitor.cpp
        src/gpu_monitor.cpp
        src/power_table.cpp
        src/graph_panel.h
        src/graph_panel.cpp
        src/theme.cpp
        src/config_win.cpp
        src/main_frame.cpp
        src/metrics_api.h
        src/metrics_api.cpp
        src/http_server.h
        src/http_server.cpp
    )
elseif(UNIX AND NOT APPLE)
    # Linux
    list(APPEND SOURCES
        src/cpu_monitor_linux.cpp
        src/ram_monitor_linux.cpp
        src/gpu_monitor_linux.cpp
        src/power_table.cpp
        src/graph_panel.h
        src/graph_panel.cpp
        src/theme_linux.cpp
        src/config_linux.cpp
        src/main_frame.cpp
    )
else()
    message(WARNING "Unsupported platform: CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}")
endif()

# ----------------------------------------------------------------------------
# Executable
# ----------------------------------------------------------------------------
add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    src
)

if(WIN32)
    # Windows SDK include paths are supplied automatically by the MSVC generator;
    # hard-coding a specific SDK version (10.0.26100.0) would break on machines
    # with any other SDK revision installed.
    target_include_directories(${PROJECT_NAME} PRIVATE ${wxWidgets_INCLUDE_DIRS})

    set(WINDOWS_LIBS ws2_32 rpcrt4 odbc32 shlwapi shell32 comctl32 ole32 oleaut32 uuid winmm psapi advapi32 dwmapi)

    target_link_libraries(${PROJECT_NAME}
        ${wxWidgets_LIBRARIES}
        wbemuuid
        pdh
        psapi
        ${WINDOWS_LIBS}
    )

    set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        WIN32_EXECUTABLE ON
    )

    # Copy icon PNG to build dir — loaded at runtime by wxImage and recolored green.
    if(EXISTS "${CMAKE_SOURCE_DIR}/loadgraph.png")
        configure_file("${CMAKE_SOURCE_DIR}/loadgraph.png"
                       "${CMAKE_BINARY_DIR}/bin/loadgraph.png" COPYONLY)
    endif()

    # Suppress C4996 (unsafe CRT) from our own code; wxWidgets headers emit it
    # regardless, and we use safe equivalents where it matters (snprintf/strncpy).
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(${PROJECT_NAME} PRIVATE USE_HTTP_SERVER)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX-)
    target_link_options(${PROJECT_NAME} PRIVATE /SUBSYSTEM:WINDOWS)

    # ----------------------------------------------------------------------------
    # Console-only binary (htop-like, no wxWidgets dependency).
    # ----------------------------------------------------------------------------
    set(CONSOLE_SOURCES src/console_app.cpp)
    add_executable(loadgraph-console ${CONSOLE_SOURCES})
    target_include_directories(loadgraph-console PRIVATE src)
    target_compile_definitions(loadgraph-console PRIVATE USE_HTTP_SERVER _CRT_SECURE_NO_WARNINGS)
    target_compile_options(loadgraph-console PRIVATE /W4 /WX-)
    target_link_libraries(loadgraph-console pdh psapi)
    target_compile_options(loadgraph-console PRIVATE /W4 /WX-)
    target_link_libraries(loadgraph-console pdh psapi iphlpapi wbemuuid)
    set_target_properties(loadgraph-console PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
elseif(UNIX AND NOT APPLE)
    # Linux-specific settings.
    target_include_directories(${PROJECT_NAME} PRIVATE ${wxWidgets_INCLUDE_DIRS})
    target_link_directories(${PROJECT_NAME} PRIVATE ${wxWidgets_LIBRARY_DIRS})
    target_compile_options(${PROJECT_NAME} PRIVATE ${wxWidgets_COMPILE_OPTIONS})
    target_link_options(${PROJECT_NAME} PRIVATE ${wxWidgets_LINK_OPTIONS})

    find_library(NVML_LIB nvidia-ml PATHS /usr/lib/x86_64-linux-gnu /usr/local/cuda/lib64 /usr/lib)

    set(LINUX_LIBS pthread dl)
    if(NVML_LIB)
        list(APPEND LINUX_LIBS ${NVML_LIB})
        message(STATUS "Found NVML: ${NVML_LIB}")
    else()
        message(WARNING "NVML not found - GPU monitoring will use fallback or be disabled")
    endif()

    # GTK3 stack required by static wxWidgets (matches pkg-config gtk+-3.0).
    set(wxWidgets_SYSTEM_LIBS
        gtk-3 gdk-3 pangocairo-1.0 atk-1.0 cairo-gobject cairo
        gdk_pixbuf-2.0 gio-2.0 Xtst pangoft2-1.0 pango-1.0
        gobject-2.0 glib-2.0 harfbuzz fontconfig freetype
        jbig expat pcre2-32 png jpeg lzma z X11 SM xkbcommon gthread-2.0
    )

    # Point linker at our bundled .so files.
    target_link_directories(${PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/libs")
    # Embed rpath so the binary finds bundled .so files at runtime without
    # LD_LIBRARY_PATH. BUILD_RPATH covers the build tree; INSTALL_RPATH uses
    # $ORIGIN/../libs so a copied tree (install or zip distribution) works too.
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUILD_RPATH "${CMAKE_SOURCE_DIR}/libs"
        INSTALL_RPATH "$ORIGIN/../libs"
        INSTALL_RPATH_USE_LINK_PATH OFF
    )

    target_link_libraries(${PROJECT_NAME}
        ${wxWidgets_LIBRARIES}
        ${wxWidgets_SYSTEM_LIBS}
        ${LINUX_LIBS}
    )

    set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()
