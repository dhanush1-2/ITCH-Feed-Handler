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
    test_parser.cpp
    test_mapped_file.cpp
    test_provenance.cpp
    test_book.cpp
    test_builder.cpp
    test_market_state.cpp
    test_invariants.cpp
    test_oracle.cpp
    test_stats.cpp)
target_include_directories(itch_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(itch_tests PRIVATE itch GTest::gtest_main)
gtest_discover_tests(itch_tests)

# CLI tests: pytest against the built tool. Without pytest they are skipped with a message,
# unless ITCH_REQUIRE_PYTEST=ON (CI), where that is an error.
find_package(Python3 COMPONENTS Interpreter)
if(ITCH_BUILD_TOOLS AND Python3_FOUND)
    execute_process(COMMAND ${Python3_EXECUTABLE} -m pytest --version
        RESULT_VARIABLE _pytest_rc OUTPUT_QUIET ERROR_QUIET)
    if(_pytest_rc EQUAL 0)
        add_test(NAME replay_cli
            COMMAND ${Python3_EXECUTABLE} -m pytest -q ${CMAKE_CURRENT_SOURCE_DIR}/test_replay_cli.py)
        set_tests_properties(replay_cli PROPERTIES
            ENVIRONMENT "ITCH_REPLAY=$<TARGET_FILE:itch_replay>")
        add_test(NAME reference_verify
            COMMAND ${Python3_EXECUTABLE} -m pytest -q ${CMAKE_CURRENT_SOURCE_DIR}/test_verify.py)
        set_tests_properties(reference_verify PROPERTIES
            ENVIRONMENT "ITCH_REPLAY=$<TARGET_FILE:itch_replay>")
    elseif(ITCH_REQUIRE_PYTEST)
        message(FATAL_ERROR "pytest is required (ITCH_REQUIRE_PYTEST=ON) but "
                            "'${Python3_EXECUTABLE} -m pytest' failed")
    else()
        message(STATUS "pytest not found for ${Python3_EXECUTABLE}: replay_cli test not registered")
    endif()
endif()

# Real-data slice (testdata/slice.itch.gz): expanded into the build tree, replayed with every
# check on, and its export compared with testdata/expected_slice.json.
if(ITCH_BUILD_TOOLS AND Python3_FOUND AND EXISTS ${CMAKE_SOURCE_DIR}/testdata/slice.itch.gz)
    set(_slice_dir ${CMAKE_BINARY_DIR}/testdata)
    add_test(NAME slice_prepare
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/prepare_slice.py
                --out ${_slice_dir}/slice.itch)
    set_tests_properties(slice_prepare PROPERTIES FIXTURES_SETUP slice)
    add_test(NAME slice_golden
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_slice.py
                --replay $<TARGET_FILE:itch_replay> --slice ${_slice_dir}/slice.itch
                --work ${_slice_dir})
    set_tests_properties(slice_golden PROPERTIES FIXTURES_REQUIRED slice)
endif()
