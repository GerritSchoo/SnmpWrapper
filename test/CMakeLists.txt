# Tests. Only added when BUILD_TESTING is on (see the top-level CMakeLists.txt).
#
#   unit        - every class and function on its own, collaborators replaced by mocks; no Agent, no Client,
#                 no network. One test file per source file, in the same folders:
#                   snmpwrap/<Name>Test.cpp   <- src/snmpwrap/<Name>.cpp (or include/snmpwrap/<Name>.hpp)
#                   mibgen/<Name>Test.cpp     <- tools/mibgen/<Name>.cpp
#                   generated/<Class>Test.cpp <- the classes snmpwrap-mibgen generates
#                 Mocks lie next to the tests: <Interface>Mock.hpp for the interface in <Interface>.hpp.
#                 File names are UpperCamelCase after what they hold; only main.cpp / test_main.cpp keep their names.
#   integration - the real Agent / Client / NotificationReceiver with Net-SNMP, and a private snmpd
#                 (integration/). Slower; run with `ctest -L integration`.
#
# Every GoogleTest program uses test_main.cpp. Each TEST is a CTest test of its own (gtest_discover_tests).

if(NOT SNMPWRAP_BUILD_MIBGEN)
    message(STATUS "snmpwrap: tests skipped - they need the generator (SNMPWRAP_BUILD_MIBGEN=ON)")
    return()
endif()

find_package(GTest 1.11 CONFIG REQUIRED)  # apt install libgtest-dev libgmock-dev
include(GoogleTest)

add_library(snmpwrap_test_main OBJECT test_main.cpp)
target_link_libraries(snmpwrap_test_main PUBLIC GTest::gtest)

# snmpwrap_gtest(<target> <label> <sources...>): a GoogleTest program with test_main.cpp and gmock
function(snmpwrap_gtest target label)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE snmpwrap_test_main GTest::gmock GTest::gtest)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})  # "TestSupport.hpp", "snmpwrap/...Mock.hpp"
    target_compile_definitions(${target} PRIVATE SNMPWRAP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # partial aggregate init (ScalarDef{type, get}) is how the API is meant to be used
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
    endif()
    gtest_discover_tests(${target} DISCOVERY_MODE PRE_TEST PROPERTIES LABELS ${label})
endfunction()

# --- code generated from the test MIBs (it must compile without warnings) --------------------------
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

# --- unit tests -----------------------------------------------------------------------------------
snmpwrap_gtest(snmpwrap_unit_tests unit
    snmpwrap/OidTest.cpp
    snmpwrap/ValueTest.cpp
    snmpwrap/IndexTest.cpp
    snmpwrap/ErrorTest.cpp
    snmpwrap/ISessionTest.cpp
    snmpwrap/MibTest.cpp
    snmpwrap/MibModelTest.cpp
    snmpwrap/ConvertTest.cpp
    snmpwrap/AgentDetailTest.cpp
    snmpwrap/ClientDetailTest.cpp
    snmpwrap/NotificationDetailTest.cpp
    mibgen/GeneratorTest.cpp
    mibgen/CliTest.cpp
    generated/DataAgentTest.cpp
    generated/RemoteTest.cpp
    generated/NotificationsTest.cpp
    generated/TextOutputTest.cpp)
# the internal classes (src/snmpwrap/*_detail.hpp, Convert.hpp) work on Net-SNMP's structures
target_include_directories(snmpwrap_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/snmpwrap)
target_link_libraries(snmpwrap_unit_tests PRIVATE snmpwrap::snmpwrap NetSNMP::NetSNMP snmpwrap_mibgen_core
                      nested_test_mib nested_group_mib snmpwrapper_test_mib keyword_mib)

# --- integration tests ----------------------------------------------------------------------------
# Net-SNMP keeps process-wide state: once a Client exists, no Agent can be created in the process - so the Agent
# tests and the Client tests are two programs (CTest runs every test in a process of its own anyway).
snmpwrap_gtest(snmpwrap_integration_agent_tests integration
    integration/AgentTest.cpp
    integration/LifecycleTest.cpp)
target_link_libraries(snmpwrap_integration_agent_tests PRIVATE snmpwrapper_test_mib)

snmpwrap_gtest(snmpwrap_integration_client_tests integration
    integration/ClientTest.cpp
    integration/NotificationReceiverTest.cpp)  # sends its notifications with Net-SNMP directly
target_link_libraries(snmpwrap_integration_client_tests PRIVATE snmpwrap::snmpwrap NetSNMP::NetSNMP)

# Against a private snmpd: the Net-SNMP tools, snmpwrap::Client and the generated Remote; needs the examples.
if(SNMPWRAP_BUILD_EXAMPLES AND UNIX)
    find_program(BASH_EXECUTABLE bash)
    if(BASH_EXECUTABLE)
        # The same suite runs against every implementation of SNMPWRAPPER-TEST-MIB:
        #   integration          - integration/agents/TestAgent.cpp (hand-written OIDs, core Mib API)
        #   integration_data     - integration/agents/DataTestAgent.cpp (the generated Data structure)
        #                          + the generated Remote client (remote_client_check)
        add_executable(test_agent integration/agents/TestAgent.cpp)
        target_link_libraries(test_agent PRIVATE snmpwrap::snmpwrap)
        add_executable(data_test_agent integration/agents/DataTestAgent.cpp)
        target_link_libraries(data_test_agent PRIVATE snmpwrapper_test_mib)
        add_executable(remote_client_check integration/agents/RemoteClientCheck.cpp)
        target_link_libraries(remote_client_check PRIVATE snmpwrapper_test_mib)

        foreach(variant "integration:test_agent" "integration_data:data_test_agent")
            string(REPLACE ":" ";" parts "${variant}")
            list(GET parts 0 test_name)
            list(GET parts 1 agent_target)
            add_test(NAME ${test_name}
                COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/run_integration.sh
                        $<TARGET_FILE:${agent_target}> $<TARGET_FILE:client_cli>
                        ${NetSNMP_PREFIX} ${CMAKE_CURRENT_SOURCE_DIR}/mibs)
            # all variants use the same local ports
            set_tests_properties(${test_name} PROPERTIES TIMEOUT 120 RESOURCE_LOCK snmpd_ports LABELS integration)
        endforeach()
        set_property(TEST integration_data PROPERTY ENVIRONMENT "EXTRA_CHECK=$<TARGET_FILE:remote_client_check>")

        # example applications (examples/apps): agent_app + client_app against a private snmpd
        if(TARGET agent_app AND TARGET client_app)
            add_test(NAME apps
                COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/run_apps.sh
                        $<TARGET_FILE:agent_app> $<TARGET_FILE:client_app> ${NetSNMP_PREFIX})
            set_tests_properties(apps PROPERTIES TIMEOUT 60 RESOURCE_LOCK snmpd_ports LABELS integration)
        endif()
    endif()
endif()
