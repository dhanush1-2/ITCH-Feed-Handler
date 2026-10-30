add_executable(itch_stat itch_stat.cpp)
target_link_libraries(itch_stat PRIVATE itch)

# Writes the seed corpus for fuzz_parser from the test message builders.
add_executable(itch_seed_corpus seed_corpus.cpp)
target_link_libraries(itch_seed_corpus PRIVATE itch)
target_include_directories(itch_seed_corpus PRIVATE ${CMAKE_SOURCE_DIR}/tests)
