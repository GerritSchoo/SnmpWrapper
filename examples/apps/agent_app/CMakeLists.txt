# This is all an application needs to use snmpwrap. Works stand-alone against an installed snmpwrap
#
#   cmake -S . -B build -DCMAKE_PREFIX_PATH="<snmpwrap install prefix>;<net-snmp prefix>"
#   cmake --build build
#
# and as part of the snmpwrap build (examples/CMakeLists.txt), where the targets already exist.
cmake_minimum_required(VERSION 3.16)
project(agent_app CXX)

if(NOT TARGET snmpwrap::snmpwrap)
    find_package(snmpwrap REQUIRED)        # provides snmpwrap::snmpwrap and snmpwrap_add_mib()
endif()

# MIB -> C++ (my_app_mib.hpp/.cpp) at build time, again whenever the MIB changes
if(NOT TARGET my_app_mib)                  # agent_app and client_app share it inside the snmpwrap build
    add_library(my_app_mib STATIC)
    snmpwrap_add_mib(my_app_mib MODULE MY-APP-MIB MIB ${CMAKE_CURRENT_SOURCE_DIR}/../mibs/MY-APP-MIB.txt)
endif()

add_executable(agent_app main.cpp)
target_link_libraries(agent_app PRIVATE my_app_mib)   # brings snmpwrap::snmpwrap along
