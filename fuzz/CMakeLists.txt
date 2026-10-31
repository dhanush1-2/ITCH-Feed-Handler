# libFuzzer is a clang feature. Fail loudly rather than silently building nothing.
#
# Apple clang (Command Line Tools) ships NO libFuzzer runtime: linking fails with
# "library 'libclang_rt.fuzzer_osx.a' not found". Build this in Docker with real LLVM
# clang instead; see scripts/fuzz.sh.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ITCH_BUILD_FUZZ=ON requires clang (libFuzzer)")
endif()

add_executable(fuzz_parser fuzz_parser.cpp)
target_link_libraries(fuzz_parser PRIVATE itch)
# -UNDEBUG and ITCH_VERIFY keep the view type assertions live while fuzzing.
target_compile_definitions(fuzz_parser PRIVATE ITCH_VERIFY=1)
target_compile_options(fuzz_parser PRIVATE
    -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -UNDEBUG)
target_link_options(fuzz_parser PRIVATE -fsanitize=fuzzer,address,undefined)

add_executable(fuzz_mold fuzz_mold.cpp)
target_link_libraries(fuzz_mold PRIVATE itch)
target_compile_definitions(fuzz_mold PRIVATE ITCH_VERIFY=1)
target_compile_options(fuzz_mold PRIVATE
    -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g -UNDEBUG)
target_link_options(fuzz_mold PRIVATE -fsanitize=fuzzer,address,undefined)
