# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Qt6 REQUIRED COMPONENTS Test)

function(termx_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE termx_core Qt6::Test)
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src/core)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
endfunction()

termx_add_test(tst_telnetprotocol)
termx_add_test(tst_vtscreen)
termx_add_test(tst_commandcapture)
termx_add_test(tst_ansistripper)
termx_add_test(tst_sessionconfig)
termx_add_test(tst_historystore)
termx_add_test(tst_jsonfile)
