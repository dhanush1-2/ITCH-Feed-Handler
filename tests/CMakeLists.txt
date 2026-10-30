include(FetchContent)
# SYSTEM keeps GoogleTest's own headers out of our -Wold-style-cast.
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
    SYSTEM)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(itch_tests
    test_parser.cpp)
target_include_directories(itch_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(itch_tests PRIVATE itch GTest::gtest_main)
gtest_discover_tests(itch_tests)
