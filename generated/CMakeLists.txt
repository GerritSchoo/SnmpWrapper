# Code generated from the MIBs in ../mibs - one static library per MIB.
#
# The build writes the generated sources into this folder (generated/<name>/<name>.hpp/.cpp) so they
# can be read and reviewed like normal code. They are regenerated automatically whenever the MIB
# changes - never edit them by hand.

function(snmpwrap_mib_library name module mib_file)
    add_library(${name} STATIC)
    snmpwrap_add_mib(${name}
        MODULE ${module}
        MIB ${mib_file}
        NAME ${name}
        OUTPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${name})
    set_target_properties(${name} PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)  # generated code must be warning-free
    endif()
endfunction()

# demo device of examples/mib_agent.cpp and examples/mib_client.cpp
snmpwrap_mib_library(snmpwrapper_demo_mib SNMPWRAPPER-DEMO-MIB ${PROJECT_SOURCE_DIR}/mibs/SNMPWRAPPER-DEMO-MIB.txt)

# example MIB used by the tests (test/agents/generated_*.cpp)
snmpwrap_mib_library(snmpwrapper_test_mib SNMPWRAPPER-TEST-MIB ${PROJECT_SOURCE_DIR}/mibs/SNMPWRAPPER-TEST-MIB.txt)
