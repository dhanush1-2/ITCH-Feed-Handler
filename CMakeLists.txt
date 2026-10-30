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
option(ITCH_VERIFY            "Enable ITCH_ASSERT checks"    OFF)
option(ITCH_SANITIZE          "Enable ASan + UBSan"          OFF)
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

if(ITCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
