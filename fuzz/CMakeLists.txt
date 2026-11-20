# SPDX-License-Identifier: GPL-3.0-or-later
# libFuzzer targets (clang only):
#   cmake -B build/fuzz -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DTERMX_BUILD_FUZZERS=ON -DTERMX_BUILD_TESTS=OFF
#   build/fuzz/fuzz/fuzz_vtscreen -max_total_time=600
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Fuzzers need clang (libFuzzer)")
endif()

function(termx_add_fuzzer name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE termx_core)
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src/core)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
endfunction()

termx_add_fuzzer(fuzz_vtscreen)  # regressions: fuzz/regressions/vtscreen
termx_add_fuzzer(fuzz_telnet)
termx_add_fuzzer(fuzz_ansistripper)
termx_add_fuzzer(fuzz_socks)
