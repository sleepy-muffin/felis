cmake_minimum_required(VERSION 3.31)

project(felis CXX)


# ========== ---------- ---------- ---------- ==========
#                   PLATFORM VARIABLES
# ========== ---------- ---------- ---------- ==========


# Platform string. Android sets UNIX as well, so it has to be tested first.
if(WIN32)
    set(PLATFORM "windows")
elseif(APPLE)
    set(PLATFORM "macos")
elseif(ANDROID)
    set(PLATFORM "android")
elseif(UNIX)
    set(PLATFORM "linux")
else()
    set(PLATFORM "unknown")
endif()


# Architecture
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(ARCH "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
    set(ARCH "arm64")
else()
    set(ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

# ========== ---------- ---------- ---------- ==========
#                         OPTIONS
# ========== ---------- ---------- ---------- ==========


# Modules with heavy external dependencies are opt-out. Consumers that do not
# need them can turn them off before adding felis (add_subdirectory/FetchContent)
# and CMake will not look for their dependencies at all.
option(FELIS_BUILD_NET "Build the external module (Boost.JSON helpers)" ON)
option(FELIS_BUILD_SQLITE3 "Build the sqlite3 module" ON)
option(FELIS_BUILD_IMAGE "Build the image module (OpenGL/JPEG/PNG)" ON)
option(FELIS_BUILD_AUDIO "Build the audio module (opus/opusfile/mpg123/lame)" ON)

set(FELIS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# ========== ---------- ---------- ---------- ==========
#                         SET UP
# ========== ---------- ---------- ---------- ==========


set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF CACHE BOOL
    "Scan C++ sources for module dependencies")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Register a module as its own target, plus a felis::<name> alias.
#
#   felis_add_module(<name> INTERFACE [PUBLIC <targets...>])
#   felis_add_module(<name> STATIC
#       SOURCES <files...>
#       [PUBLIC <targets...>]
#       [PRIVATE <targets...>]
#       [PRIVATE_INCLUDES <dirs...>])
function(felis_add_module name type)
    cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC;PRIVATE;PRIVATE_INCLUDES" ${ARGN})

    set(target "felis_${name}")

    add_library(${target} ${type} ${ARG_SOURCES})
    add_library(felis::${name} ALIAS ${target})

    if(type STREQUAL "INTERFACE")
        target_include_directories(${target} INTERFACE ${FELIS_INCLUDE_DIR})
        # target_compile_features(${target} INTERFACE cxx_std_20)

        if(ARG_PUBLIC)
            target_link_libraries(${target} INTERFACE ${ARG_PUBLIC})
        endif()
    else()
        target_include_directories(${target} PUBLIC ${FELIS_INCLUDE_DIR})
        if(ARG_PRIVATE_INCLUDES)
            target_include_directories(${target} PRIVATE ${ARG_PRIVATE_INCLUDES})
        endif()

        # target_compile_features(${target} PUBLIC cxx_std_20)

        if(ARG_PUBLIC)
            target_link_libraries(${target} PUBLIC ${ARG_PUBLIC})
        endif()
        if(ARG_PRIVATE)
            target_link_libraries(${target} PRIVATE ${ARG_PRIVATE})
        endif()

        set_target_properties(${target} PROPERTIES
                OUTPUT_NAME "felis-${name}-${PLATFORM}-${ARCH}"
        )

        target_compile_options(${target} PRIVATE -O2)

        # When felis is vendored (add_subdirectory/FetchContent), only compile
        # the modules a consuming target actually links. Building felis itself
        # as the top-level project still builds everything.
        if(NOT PROJECT_IS_TOP_LEVEL)
            set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL TRUE)
        endif()
    endif()

    set(FELIS_MODULE_TARGETS ${FELIS_MODULE_TARGETS} ${target} PARENT_SCOPE)
endfunction()


# ========== ---------- ---------- ---------- ==========
#                   STANDARD C++ CORE MODULE
# ========== ---------- ---------- ---------- ==========


# common, log, sync, async, math, str, legacy, toml and json only need the
# standard library, so they are compiled into a single target.
felis_add_module(
        common STATIC
        SOURCES
        src/str.cpp
        src/utf8.cpp
        src/legacy.cpp
        src/toml.cpp
        src/json.cpp
        src/file/file.cpp
        src/file/search.cpp

        PUBLIC
        Boost::json
        Boost::regex
)


# ========== ---------- ---------- ---------- ==========
#                     EXTERNAL MODULES
# ========== ---------- ---------- ---------- ==========


if(FELIS_BUILD_NET)
    find_package(Boost REQUIRED COMPONENTS json regex url)
    find_package(OpenSSL REQUIRED)
    find_package(Threads REQUIRED)

    # The TLS client loads the OS root store on Windows so HTTPS works without
    # a bundled CA file. crypt32 backs the root-store calls in http.cpp.
    set(FELIS_NET_PLATFORM_LIBRARIES)
    if(WIN32)
        list(APPEND FELIS_NET_PLATFORM_LIBRARIES crypt32)
    endif()

    # external.hpp exposes boost::json in its public API.
    felis_add_module(
            net STATIC
            SOURCES
            src/net/http.cpp
            src/net/proxy.cpp
            src/net/llm.cpp

            PUBLIC
            felis::common

            Boost::json
            Boost::regex
            Boost::url

            OpenSSL::SSL
            OpenSSL::Crypto

            Threads::Threads

            PRIVATE
            ${FELIS_NET_PLATFORM_LIBRARIES}
    )


endif()

if(FELIS_BUILD_SQLITE3)
    find_package(SQLite3 REQUIRED)

    # SQLite3::SQLite3 is the current target name; older CMake only provides
    # SQLite::SQLite3.
    if(TARGET SQLite3::SQLite3)
        set(FELIS_SQLITE3_TARGET SQLite3::SQLite3)
    else()
        set(FELIS_SQLITE3_TARGET SQLite::SQLite3)
    endif()

    felis_add_module(sqlite3 STATIC
            SOURCES src/sqlite3.cpp
            PUBLIC felis::common
            PRIVATE ${FELIS_SQLITE3_TARGET}
    )
endif()

if(FELIS_BUILD_IMAGE)
    find_package(OpenGL REQUIRED)
    find_package(JPEG REQUIRED)
    find_package(PNG REQUIRED)

    # image.hpp includes <GL/gl.h> in its public API.
    felis_add_module(image STATIC
            SOURCES src/media/image.cpp
            PUBLIC OpenGL::GL
            PRIVATE JPEG::JPEG PNG::PNG
    )
endif()

if(FELIS_BUILD_AUDIO)
    find_library(OPUSENC_LIBRARY NAMES opusenc REQUIRED)
    find_path(OPUS_INCLUDE_DIR NAMES opus.h PATH_SUFFIXES opus REQUIRED)
    find_library(OPUSFILE_LIBRARY NAMES opusfile REQUIRED)
    find_path(MPG123_INCLUDE_DIR NAMES mpg123.h REQUIRED)
    find_library(MPG123_LIBRARY NAMES mpg123 REQUIRED)
    find_path(LAME_INCLUDE_DIR NAMES lame/lame.h lame.h REQUIRED)
    find_library(LAME_LIBRARY NAMES mp3lame lame REQUIRED)

    felis_add_module(audio STATIC
            SOURCES src/media/audio.cpp
            PRIVATE
            ${OPUSENC_LIBRARY} ${OPUSFILE_LIBRARY}
            ${MPG123_LIBRARY} ${LAME_LIBRARY}
            PRIVATE_INCLUDES
            ${OPUS_INCLUDE_DIR} ${MPG123_INCLUDE_DIR} ${LAME_INCLUDE_DIR}
    )
endif()


# ========== ---------- ---------- ---------- ==========
#                     UMBRELLA TARGET
# ========== ---------- ---------- ---------- ==========


# Kept for existing consumers: linking `felis` gives every enabled module.
# Lean consumers should link `felis::<module>` instead.
add_library(felis INTERFACE)
add_library(felis::felis ALIAS felis)

target_include_directories(felis INTERFACE ${FELIS_INCLUDE_DIR})
# target_compile_features(felis INTERFACE cxx_std_20)
target_link_libraries(felis INTERFACE ${FELIS_MODULE_TARGETS})

# ========== ---------- ---------- ---------- ==========
#                          TEST
# ========== ---------- ---------- ---------- ==========


# Regression tests for the public core API.
add_subdirectory(tests)
