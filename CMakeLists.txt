cmake_minimum_required(VERSION 3.24)
project(Lumen LANGUAGES CXX)

if (NOT WIN32)
    message(FATAL_ERROR "Lumen targets Windows only.")
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# Lumen builds against a prebuilt "WebKit SDK" produced by the webkit-sdk
# workflow (include/, lib/, runtime/). It is not built inside WebKit's tree.
set(LUMEN_SDK_DIR "" CACHE PATH "Extracted WebKit SDK directory")
option(LUMEN_COPY_RUNTIME "Copy the WebKit runtime files next to Lumen.exe after building" ON)

if (NOT LUMEN_SDK_DIR OR NOT IS_DIRECTORY "${LUMEN_SDK_DIR}")
    message(FATAL_ERROR
        "Set LUMEN_SDK_DIR to the extracted WebKit SDK folder (the one containing include/, lib/ and runtime/).")
endif ()

set(_lumen_webkit_lib "${LUMEN_SDK_DIR}/lib/WebKit2.lib")
if (NOT EXISTS "${_lumen_webkit_lib}")
    message(FATAL_ERROR "WebKit2.lib not found in the SDK: ${_lumen_webkit_lib}")
endif ()
if (NOT EXISTS "${LUMEN_SDK_DIR}/include/WebKit/WKView.h")
    message(FATAL_ERROR "WebKit/WKView.h not found in the SDK include directory.")
endif ()

add_executable(Lumen WIN32
    src/main.cpp
    src/browser.cpp
    src/dialogs.cpp
    src/downloads.cpp
    src/library.cpp
    src/util.cpp
    src/webkit_host.cpp
)

target_include_directories(Lumen PRIVATE
    src
    "${LUMEN_SDK_DIR}/include"
)

target_compile_definitions(Lumen PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(Lumen PRIVATE
    "${_lumen_webkit_lib}"
    user32
    gdi32
    gdiplus
    advapi32
    comdlg32
    shell32
)

if (MSVC)
    target_compile_options(Lumen PRIVATE /W4 /permissive- /utf-8)
endif ()

if (LUMEN_COPY_RUNTIME)
    add_custom_command(TARGET Lumen POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${LUMEN_SDK_DIR}/runtime" "$<TARGET_FILE_DIR:Lumen>"
        COMMENT "Copying WebKit runtime next to Lumen.exe"
    )
endif ()
