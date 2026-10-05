add_executable(snmpwrap_unit_tests unit_tests.cpp)
target_link_libraries(snmpwrap_unit_tests PRIVATE snmpwrap::snmpwrap)
add_test(NAME unit COMMAND snmpwrap_unit_tests)

add_executable(snmpwrap_mib_model_tests mib_model_tests.cpp)
target_link_libraries(snmpwrap_mib_model_tests PRIVATE snmpwrap::snmpwrap)
target_compile_definitions(snmpwrap_mib_model_tests PRIVATE SNMPWRAP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME mib_model COMMAND snmpwrap_mib_model_tests)

add_executable(snmpwrap_mib_binder_tests mib_binder_tests.cpp)
target_link_libraries(snmpwrap_mib_binder_tests PRIVATE snmpwrap::snmpwrap)
target_compile_definitions(snmpwrap_mib_binder_tests PRIVATE SNMPWRAP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME mib_binder COMMAND snmpwrap_mib_binder_tests)

add_executable(snmpwrap_lifecycle_test lifecycle_test.cpp)
target_link_libraries(snmpwrap_lifecycle_test PRIVATE snmpwrap::snmpwrap)
add_test(NAME lifecycle_client_then_agent COMMAND snmpwrap_lifecycle_test client-then-agent)
add_test(NAME lifecycle_agent_twice COMMAND snmpwrap_lifecycle_test agent-twice)

# Integration test: needs the Net-SNMP daemon tools (snmpd, snmpget, snmpwalk, ...) and the examples.
if(SNMPWRAP_BUILD_EXAMPLES AND UNIX)
    find_program(BASH_EXECUTABLE bash)
    if(BASH_EXECUTABLE)
        # The same suite runs against every implementation of SNMPWRAPPER-TEST-MIB:
        #   integration          - examples/test_agent.cpp (hand-written OIDs, Mib API)
        #   integration_runtime  - test/agents/runtime_test_agent.cpp (MIB loaded at run time, MibBinder)
        add_executable(runtime_test_agent agents/runtime_test_agent.cpp)
        target_link_libraries(runtime_test_agent PRIVATE snmpwrap::snmpwrap)
        target_compile_definitions(runtime_test_agent PRIVATE SNMPWRAP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

        set(variants "integration:test_agent" "integration_runtime:runtime_test_agent")

        #   integration_generated - test/agents/generated_test_agent.cpp (code generated from the MIB at build time)
        #                           + the generated typed client (generated_client_check)
        if(TARGET snmpwrapper_test_mib)  # generated/snmpwrapper_test_mib
            add_executable(generated_test_agent agents/generated_test_agent.cpp)
            target_link_libraries(generated_test_agent PRIVATE snmpwrapper_test_mib)
            add_executable(generated_client_check agents/generated_client_check.cpp)
            target_link_libraries(generated_client_check PRIVATE snmpwrapper_test_mib)
            list(APPEND variants "integration_generated:generated_test_agent")
        endif()

        foreach(variant IN LISTS variants)
            string(REPLACE ":" ";" parts "${variant}")
            list(GET parts 0 test_name)
            list(GET parts 1 agent_target)
            add_test(NAME ${test_name}
                COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_integration.sh
                        $<TARGET_FILE:${agent_target}> $<TARGET_FILE:client_cli>
                        ${NetSNMP_PREFIX} ${PROJECT_SOURCE_DIR}/mibs)
            # all variants use the same local ports
            set_tests_properties(${test_name} PROPERTIES TIMEOUT 120 RESOURCE_LOCK snmpd_ports)
        endforeach()
        if(TARGET generated_client_check)
            set_property(TEST integration_generated PROPERTY ENVIRONMENT "EXTRA_CHECK=$<TARGET_FILE:generated_client_check>")
        endif()
    endif()
endif()
