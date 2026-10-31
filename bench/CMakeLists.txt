# Benchmark harness. macOS only (mach task_info for the memory figures, QOS classes).
if(APPLE)
    add_executable(bench_feed bench_feed.cpp)
    target_include_directories(bench_feed PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(bench_feed PRIVATE itch)
endif()
