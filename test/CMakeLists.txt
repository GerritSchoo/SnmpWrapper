# Tests. Only added when BUILD_TESTING is on (see the top-level CMakeLists.txt).
#   unit/       - library parts without snmpd (Oid, Value, index codec, Mib, RowStatus, MibModel)
#   generated/  - the code snmpwrap-mibgen generates (Data, DataAgent, Remote, notifications, text output)
#   mibgen/     - the generator itself (generate(), its refusals, the command line)
#   agent/      - Agent: registering and removing subtrees (no snmpd: the Agent never connects)
#   lifecycle   - Agent / Client construction order (each case in a child process)
#   integration - against a real snmpd (run_integration.sh, run_apps.sh)
# The GoogleTest suites are registered test by test (gtest_discover_tests): `ctest -L unit` runs them all.

find_package(GTest 1.11 CONFIG REQUIRED)  # apt install libgtest-dev libgmock-dev
include(GoogleTest)

# snmpwrap_gtest(<target> <prefix> <sources...>): a GoogleTest executable, every TEST a CTest test "<prefix>.<Suite>.<Name>"
function(snmpwrap_gtest target prefix)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE GTest::gmock_main)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})  # "support/...", "generated/..."
    target_compile_definitions(${target} PRIVATE SNMPWRAP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # partial aggregate init (ScalarDef{type, get}) is how the API is meant to be used
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
    endif()
    gtest_discover_tests(${target} TEST_PREFIX "${prefix}." DISCOVERY_MODE PRE_TEST PROPERTIES LABELS unit)
endfunction()

snmpwrap_gtest(snmpwrap_unit_tests unit
    unit/oid_test.cpp
    unit/value_test.cpp
    unit/index_test.cpp
    unit/mib_test.cpp
    unit/mib_rowstatus_test.cpp
    unit/mib_model_test.cpp)
target_link_libraries(snmpwrap_unit_tests PRIVATE snmpwrap::snmpwrap)

snmpwrap_gtest(snmpwrap_lifecycle_test lifecycle lifecycle_test.cpp)
target_link_libraries(snmpwrap_lifecycle_test PRIVATE snmpwrap::snmpwrap)

if(SNMPWRAP_BUILD_MIBGEN)
    # Code generated from the test MIBs (generated code must compile without warnings)
    add_library(snmpwrapper_test_mib STATIC)
    snmpwrap_add_mib(snmpwrapper_test_mib MODULE SNMPWRAPPER-TEST-MIB MIB ${CMAKE_CURRENT_SOURCE_DIR}/mibs/SNMPWRAPPER-TEST-MIB.txt)
    add_library(nested_test_mib STATIC)
    snmpwrap_add_mib(nested_test_mib MODULE NESTED-TEST-MIB MIB ${CMAKE_CURRENT_SOURCE_DIR}/mibs/NESTED-TEST-MIB.txt)
    # types from another MIB of our own (found through MIB_DIRS), every object inside one group
    add_library(nested_group_mib STATIC)
    snmpwrap_add_mib(nested_group_mib MODULE NESTED-GROUP-MIB MIB ${CMAKE_CURRENT_SOURCE_DIR}/mibs/NESTED-GROUP-MIB.txt
                     MIB_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/mibs)
    # C++ keywords as MIB names: the escaped identifiers must compile
    add_library(keyword_mib STATIC)
    snmpwrap_add_mib(keyword_mib MODULE KEYWORD-MIB MIB ${CMAKE_CURRENT_SOURCE_DIR}/mibs/mibgen/KEYWORD-MIB.txt)
    foreach(lib snmpwrapper_test_mib nested_test_mib nested_group_mib keyword_mib)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${lib} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endforeach()

    snmpwrap_gtest(snmpwrap_generated_tests generated
        generated/data_agent_test.cpp
        generated/remote_test.cpp
        generated/notifications_test.cpp
        generated/text_output_test.cpp)
    target_link_libraries(snmpwrap_generated_tests PRIVATE nested_test_mib nested_group_mib snmpwrapper_test_mib keyword_mib)

    # Agent: register / remove subtrees, also through DataAgent (an Agent per test, never connected)
    snmpwrap_gtest(snmpwrap_agent_tests agent agent/agent_test.cpp)
    target_link_libraries(snmpwrap_agent_tests PRIVATE snmpwrapper_test_mib)

    snmpwrap_gtest(snmpwrap_mibgen_tests mibgen mibgen/generator_test.cpp)
    target_link_libraries(snmpwrap_mibgen_tests PRIVATE snmpwrap_mibgen_core)

    find_program(BASH_FOR_CLI bash)
    if(BASH_FOR_CLI)
        add_test(NAME mibgen.cli
            COMMAND ${BASH_FOR_CLI} ${CMAKE_CURRENT_SOURCE_DIR}/mibgen/cli_test.sh
                    $<TARGET_FILE:snmpwrap-mibgen> ${CMAKE_CURRENT_SOURCE_DIR}/mibs ${CMAKE_CURRENT_BINARY_DIR}/mibgen_cli)
        set_tests_properties(mibgen.cli PROPERTIES LABELS unit TIMEOUT 60)
    endif()
endif()

# Integration test: needs the Net-SNMP daemon tools (snmpd, snmpget, snmpwalk, ...) and the examples.
if(SNMPWRAP_BUILD_EXAMPLES AND UNIX)
    find_program(BASH_EXECUTABLE bash)
    if(BASH_EXECUTABLE)
        # The same suite runs against every implementation of SNMPWRAPPER-TEST-MIB:
        #   integration          - test/agents/test_agent.cpp (hand-written OIDs, core Mib API)
        add_executable(test_agent agents/test_agent.cpp)
        target_link_libraries(test_agent PRIVATE snmpwrap::snmpwrap)

        set(variants "integration:test_agent")

        if(TARGET snmpwrapper_test_mib)
            #   integration_data - test/agents/data_test_agent.cpp (the same MIB served from the generated Data structure)
            #                      + the generated Remote client (remote_client_check)
            add_executable(data_test_agent agents/data_test_agent.cpp)
            target_link_libraries(data_test_agent PRIVATE snmpwrapper_test_mib)
            add_executable(remote_client_check agents/remote_client_check.cpp)
            target_link_libraries(remote_client_check PRIVATE snmpwrapper_test_mib)
            list(APPEND variants "integration_data:data_test_agent")
        endif()

        foreach(variant IN LISTS variants)
            string(REPLACE ":" ";" parts "${variant}")
            list(GET parts 0 test_name)
            list(GET parts 1 agent_target)
            add_test(NAME ${test_name}
                COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_integration.sh
                        $<TARGET_FILE:${agent_target}> $<TARGET_FILE:client_cli>
                        ${NetSNMP_PREFIX} ${CMAKE_CURRENT_SOURCE_DIR}/mibs)
            # all variants use the same local ports
            set_tests_properties(${test_name} PROPERTIES TIMEOUT 120 RESOURCE_LOCK snmpd_ports LABELS integration)
        endforeach()
        # example applications (examples/apps): agent_app + client_app against a private snmpd
        if(TARGET agent_app AND TARGET client_app)
            add_test(NAME apps
                COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_apps.sh
                        $<TARGET_FILE:agent_app> $<TARGET_FILE:client_app> ${NetSNMP_PREFIX})
            set_tests_properties(apps PROPERTIES TIMEOUT 60 RESOURCE_LOCK snmpd_ports LABELS integration)
        endif()
        if(TARGET remote_client_check)
            set_property(TEST integration_data PROPERTY ENVIRONMENT "EXTRA_CHECK=$<TARGET_FILE:remote_client_check>")
        endif()
    endif()
endif()
