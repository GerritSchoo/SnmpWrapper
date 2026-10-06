add_executable(snmpwrap_unit_tests unit_tests.cpp)
target_link_libraries(snmpwrap_unit_tests PRIVATE snmpwrap::snmpwrap)
add_test(NAME unit COMMAND snmpwrap_unit_tests)

add_executable(snmpwrap_mib_model_tests mib_model_tests.cpp)
target_link_libraries(snmpwrap_mib_model_tests PRIVATE snmpwrap::snmpwrap)
target_compile_definitions(snmpwrap_mib_model_tests PRIVATE SNMPWRAP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME mib_model COMMAND snmpwrap_mib_model_tests)


# Code generated from the test MIBs (generated code must compile without warnings)
if(SNMPWRAP_BUILD_MIBGEN)
    add_library(snmpwrapper_test_mib STATIC)
    snmpwrap_add_mib(snmpwrapper_test_mib MODULE SNMPWRAPPER-TEST-MIB MIB ${CMAKE_CURRENT_SOURCE_DIR}/mibs/SNMPWRAPPER-TEST-MIB.txt)
    add_library(nested_test_mib STATIC)
    snmpwrap_add_mib(nested_test_mib MODULE NESTED-TEST-MIB MIB ${CMAKE_CURRENT_SOURCE_DIR}/mibs/NESTED-TEST-MIB.txt)
    # types from another MIB of our own (found through MIB_DIRS), every object inside one group
    add_library(nested_group_mib STATIC)
    snmpwrap_add_mib(nested_group_mib MODULE NESTED-GROUP-MIB MIB ${CMAKE_CURRENT_SOURCE_DIR}/mibs/NESTED-GROUP-MIB.txt
                     MIB_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/mibs)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(snmpwrapper_test_mib PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(nested_test_mib PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(nested_group_mib PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_executable(snmpwrap_data_model_tests data_model_tests.cpp)
    target_link_libraries(snmpwrap_data_model_tests PRIVATE nested_test_mib nested_group_mib snmpwrapper_test_mib)
    add_test(NAME data_model COMMAND snmpwrap_data_model_tests)
endif()

add_executable(snmpwrap_lifecycle_test lifecycle_test.cpp)
target_link_libraries(snmpwrap_lifecycle_test PRIVATE snmpwrap::snmpwrap)
add_test(NAME lifecycle_client_then_agent COMMAND snmpwrap_lifecycle_test client-then-agent)
add_test(NAME lifecycle_agent_twice COMMAND snmpwrap_lifecycle_test agent-twice)

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
            set_tests_properties(${test_name} PROPERTIES TIMEOUT 120 RESOURCE_LOCK snmpd_ports)
        endforeach()
        # example applications (examples/apps): agent_app + client_app against a private snmpd
        if(TARGET agent_app AND TARGET client_app)
            add_test(NAME apps
                COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_apps.sh
                        $<TARGET_FILE:agent_app> $<TARGET_FILE:client_app> ${NetSNMP_PREFIX})
            set_tests_properties(apps PROPERTIES TIMEOUT 60 RESOURCE_LOCK snmpd_ports)
        endif()
        if(TARGET remote_client_check)
            set_property(TEST integration_data PROPERTY ENVIRONMENT "EXTRA_CHECK=$<TARGET_FILE:remote_client_check>")
        endif()
    endif()
endif()
