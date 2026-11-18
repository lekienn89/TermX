cmake_minimum_required(VERSION 3.21)

project(TermX VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TERMX_BUILD_TESTS   "Build unit tests" ON)
option(TERMX_BUILD_FUZZERS "Build libFuzzer targets (clang only)" OFF)
option(TERMX_WERROR        "Treat warnings in TermX sources as errors" OFF)
option(TERMX_ASAN          "Build with AddressSanitizer" OFF)

# Qt 6.8 LTS is the release target. 6.4+ is accepted so the core can be
# built and unit-tested on stock Linux distributions.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network SerialPort)

# --- libssh2 (vcpkg ships a CMake config; distros usually ship pkg-config) ---
find_package(libssh2 CONFIG QUIET)
if(TARGET libssh2::libssh2)
    set(TERMX_SSH2_TARGET libssh2::libssh2)
elseif(TARGET libssh2::libssh2_shared)
    set(TERMX_SSH2_TARGET libssh2::libssh2_shared)
elseif(TARGET libssh2::libssh2_static)
    set(TERMX_SSH2_TARGET libssh2::libssh2_static)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBSSH2 REQUIRED IMPORTED_TARGET libssh2)
    set(TERMX_SSH2_TARGET PkgConfig::LIBSSH2)
endif()

find_package(SQLite3 REQUIRED)

add_subdirectory(third_party/libvterm)

# Common compile settings for TermX's own code.
add_library(termx_options INTERFACE)
target_compile_definitions(termx_options INTERFACE
    QT_NO_CAST_FROM_BYTEARRAY
    QT_NO_URL_CAST_FROM_STRING
    QT_USE_QSTRINGBUILDER
    QT_DISABLE_DEPRECATED_UP_TO=0x060400
    TERMX_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_compile_definitions(termx_options INTERFACE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()
if(MSVC)
    target_compile_options(termx_options INTERFACE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /guard:cf)
    target_link_options(termx_options INTERFACE /GUARD:CF /DYNAMICBASE /HIGHENTROPYVA /CETCOMPAT)
    if(TERMX_WERROR)
        target_compile_options(termx_options INTERFACE /WX)
    endif()
    if(TERMX_ASAN)
        target_compile_options(termx_options INTERFACE /fsanitize=address)
    endif()
else()
    target_compile_options(termx_options INTERFACE -Wall -Wextra -Wno-unused-parameter)
    if(TERMX_WERROR)
        target_compile_options(termx_options INTERFACE -Werror)
    endif()
    if(TERMX_ASAN)
        target_compile_options(termx_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(termx_options INTERFACE -fsanitize=address,undefined)
    endif()
endif()

add_subdirectory(src/core)
add_subdirectory(src/app)

if(TERMX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TERMX_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
