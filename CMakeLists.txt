cmake_minimum_required(VERSION 3.16)
project(LoadGraph VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ----------------------------------------------------------------------------
# wxWidgets configuration (cross-platform)
# ----------------------------------------------------------------------------
if(DEFINED ENV{WX_CONFIG})
    set(WX_CONFIG_PATH "$ENV{WX_CONFIG}")
elseif(WIN32)
    # Windows: prefer static build at known location.
    set(WX_CONFIG_PATH "C:/wxWidgets" CACHE PATH "wxWidgets installation root")
else()
    # Linux: parse wx-config manually (system wxWidgets often lacks cmake config files).
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

    # Parse cflags and libs using separate_arguments (handles quoting correctly).
    separate_arguments(WX_CFLAGS_LIST NATIVE_COMMAND "${WX_CFLAGS}")
    separate_arguments(WX_LIBS_LIST NATIVE_COMMAND "${WX_LIBS_OUTPUT}")

    foreach(arg ${WX_CFLAGS_LIST})
        if(arg MATCHES "^-I(.+)$")
            list(APPEND wxWidgets_INCLUDE_DIRS "${CMAKE_MATCH_1}")
        else()
            # -D defines, -pthread, etc. must reach the compiler as compile options.
            list(APPEND wxWidgets_COMPILE_OPTIONS "${arg}")
        endif()
    endforeach()

    foreach(arg ${WX_LIBS_LIST})
        if(arg MATCHES "^-l(.+)$")
            list(APPEND wxWidgets_LIBRARIES "${CMAKE_MATCH_1}")
        elseif(arg MATCHES "^-L(.+)$")
            list(APPEND wxWidgets_LIBRARY_DIRS "${CMAKE_MATCH_1}")
        else()
            # Pass through flags like -pthread that linkers also need.
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
        src/ram_monitor.cpp
        src/gpu_monitor.cpp
        src/power_table.cpp
        src/graph_panel.h
        src/graph_panel.cpp
        src/theme.cpp
        src/config_win.cpp
        src/main_frame.cpp
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
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${wxWidgets_INCLUDE_DIRS}
        "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/shared"
        "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/um"
    )
    
    # Windows system libraries.
    set(WINDOWS_LIBS ws2_32 rpcrt4 odbc32 shlwapi comctl32 ole32 oleaut32 uuid winmm psapi advapi32 dwmapi)
    
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
    
    # MSVC-specific settings.
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX-)
    target_link_options(${PROJECT_NAME} PRIVATE /SUBSYSTEM:WINDOWS)
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
    
    target_link_libraries(${PROJECT_NAME}
        ${wxWidgets_LIBRARIES}
        ${LINUX_LIBS}
    )
    
    set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    
    # GCC/Clang settings.
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

