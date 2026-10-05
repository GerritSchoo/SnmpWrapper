# Small, self-explaining demo programs (see examples/README.md)
add_executable(simple_agent simple_agent.cpp)
target_link_libraries(simple_agent PRIVATE snmpwrap::snmpwrap)

add_executable(simple_client simple_client.cpp)
target_link_libraries(simple_client PRIVATE snmpwrap::snmpwrap)

# The same demo device built from its MIB: the code generated from SNMPWRAPPER-DEMO-MIB lives in
# generated/snmpwrapper_demo_mib (library snmpwrapper_demo_mib, see generated/CMakeLists.txt)
if(TARGET snmpwrapper_demo_mib)
    add_executable(mib_agent mib_agent.cpp)
    target_link_libraries(mib_agent PRIVATE snmpwrapper_demo_mib)

    add_executable(mib_client mib_client.cpp)
    target_link_libraries(mib_client PRIVATE snmpwrapper_demo_mib)
    target_compile_definitions(mib_client PRIVATE SNMPWRAP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()

# Full-featured agent for SNMPWRAPPER-TEST-MIB and a command line client (used by the integration test)
add_executable(test_agent test_agent.cpp)
target_link_libraries(test_agent PRIVATE snmpwrap::snmpwrap)

add_executable(client_cli client_cli.cpp)
target_link_libraries(client_cli PRIVATE snmpwrap::snmpwrap)
