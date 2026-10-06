# Example applications (each one is also a stand-alone CMake project, see examples/README.md)
#   apps/agent_app   publishes the data of MY-APP-MIB (generated Data + DataAgent)
#   apps/client_app  reads and changes it (generated Remote)
if(TARGET snmpwrap::mibgen)
    add_subdirectory(apps/agent_app)
    add_subdirectory(apps/client_app)
endif()

# Command line client (get, getnext, getbulk, walk, set) - also used by the integration tests
add_executable(client_cli client_cli.cpp)
target_link_libraries(client_cli PRIVATE snmpwrap::snmpwrap)
