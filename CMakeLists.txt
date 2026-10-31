cmake_minimum_required(VERSION 3.25)
project(itch_feed LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ITCH_BUILD_TESTS       "Build tests"                  ON)
option(ITCH_BUILD_TOOLS       "Build tools"                  ON)
option(ITCH_BUILD_BENCH       "Build the benchmark harness"  ON)
option(ITCH_BUILD_FUZZ        "Build libFuzzer targets (needs clang)" OFF)
option(ITCH_VERIFY            "Enable ITCH_ASSERT checks"    OFF)
option(ITCH_SANITIZE          "Enable ASan + UBSan"          OFF)
option(ITCH_REQUIRE_PYTEST     "Fail configure if pytest is missing" OFF)
option(ITCH_WARNINGS_AS_ERRORS "Treat warnings as errors"    OFF)

# Project 1 (orderbook-cpp) is pinned to an exact commit. Its option names map
# one-to-one onto ours; its tests are never built here.
set(OB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OB_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(OB_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(OB_BUILD_FUZZ  OFF CACHE BOOL "" FORCE)
set(OB_SANITIZE ${ITCH_SANITIZE} CACHE BOOL "" FORCE)
set(OB_WARNINGS_AS_ERRORS ${ITCH_WARNINGS_AS_ERRORS} CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(orderbook
    GIT_REPOSITORY https://github.com/dhanush1-2/orderbook-cpp.git
    GIT_TAG        9c433dd8849b564c5ccddb6a4c69d5663249f13a)
FetchContent_MakeAvailable(orderbook)

# The header-only parser. Linking ob brings in Project 1's warning set
# (ob_warnings) and its sanitizer flags (ob_sanitize), so both apply to our code.
add_library(itch INTERFACE)
target_include_directories(itch INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
# clock.hpp and histogram.hpp live in Project 1's bench/ directory.
target_include_directories(itch SYSTEM INTERFACE ${orderbook_SOURCE_DIR}/bench)
target_link_libraries(itch INTERFACE ob)
if(ITCH_VERIFY)
    target_compile_definitions(itch INTERFACE ITCH_VERIFY=1)
endif()

# Provenance stamp (include/itch/provenance.hpp). The commit seen here is the BUILD commit,
# recorded at configure time. The tool resolves HEAD and the dirty state again at run time
# and compares: a binary whose build commit no longer matches HEAD is stamped
# "-stale-build", and uncommitted changes outside results/ and data/ add "-dirty".
find_package(Git QUIET)
set(_commit "unknown")
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rc ERROR_QUIET)
    if(_rc EQUAL 0)
        set(_commit "${_head}")
        execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain -- .
                ":(exclude)results" ":(exclude)data"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE _dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_dirty)
            set(_commit "${_commit}-dirty")
        endif()
    endif()
endif()

# Everything that changes what was built: the user and build-type flags, the standard,
# the sanitizer flags Project 1's ob_sanitize adds, ITCH_VERIFY and -Werror.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
set(_flags "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_bt}} -std=c++20")
if(ITCH_SANITIZE)
    string(APPEND _flags " -fsanitize=address,undefined -fno-omit-frame-pointer"
        " -fno-sanitize-recover=all -g")
endif()
if(ITCH_VERIFY)
    string(APPEND _flags " -DITCH_VERIFY=1")
endif()
if(ITCH_WARNINGS_AS_ERRORS)
    string(APPEND _flags " -Werror")
endif()
string(REGEX REPLACE "[ ]+" " " _flags "${_flags}")
string(STRIP "${_flags}" _flags)
target_compile_definitions(itch INTERFACE
    "ITCH_GIT_COMMIT=\"${_commit}\""
    "ITCH_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
    "ITCH_COMPILER=\"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\""
    "ITCH_BUILD_FLAGS=\"${_flags}\"")

if(ITCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ITCH_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(ITCH_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(ITCH_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()
