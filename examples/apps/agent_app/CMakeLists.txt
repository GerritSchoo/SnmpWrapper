# Stand-alone project: this is all an application needs to use snmpwrap as an AGENT.
#
#   cmake -S . -B build -DCMAKE_PREFIX_PATH="<snmpwrap install prefix>;<net-snmp prefix>"
#   cmake --build build
cmake_minimum_required(VERSION 3.16)
project(agent_app CXX)

find_package(snmpwrap REQUIRED)            # provides snmpwrap::snmpwrap and snmpwrap_add_mib()

# MIB -> C++: generates my_app_mib.hpp/.cpp at build time (and again whenever the MIB changes)
add_library(my_app_mib STATIC)
snmpwrap_add_mib(my_app_mib MODULE MY-APP-MIB MIB ${CMAKE_CURRENT_SOURCE_DIR}/../mibs/MY-APP-MIB.txt)

add_executable(agent_app main.cpp)
target_link_libraries(agent_app PRIVATE my_app_mib)   # brings snmpwrap::snmpwrap along
