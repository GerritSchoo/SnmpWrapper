cmake_minimum_required(VERSION 3.16)

project(snmpwrap
    VERSION 0.2.0
    DESCRIPTION "C++17 wrapper for the Net-SNMP agent (AgentX subagent) and client API"
    LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(SNMPWRAP_IS_TOP_LEVEL ON)
else()
    set(SNMPWRAP_IS_TOP_LEVEL OFF)
endif()

option(SNMPWRAP_BUILD_EXAMPLES "Build example programs" ${SNMPWRAP_IS_TOP_LEVEL})
option(SNMPWRAP_BUILD_TESTS "Build tests" ${SNMPWRAP_IS_TOP_LEVEL})
option(SNMPWRAP_INSTALL "Generate install rules" ${SNMPWRAP_IS_TOP_LEVEL})
option(SNMPWRAP_BUILD_MIBGEN "Build the MIB code generator snmpwrap-mibgen" ON)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
find_package(NetSNMP 5.9 REQUIRED)

# --- library --------------------------------------------------------------------------------
add_library(snmpwrap
    src/snmpwrap/oid.cpp
    src/snmpwrap/value.cpp
    src/snmpwrap/mib.cpp
    src/snmpwrap/index.cpp
    src/snmpwrap/mib_model.cpp
    src/snmpwrap/convert.cpp
    src/snmpwrap/agent.cpp
    src/snmpwrap/client.cpp)
add_library(snmpwrap::snmpwrap ALIAS snmpwrap)

target_include_directories(snmpwrap
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_compile_features(snmpwrap PUBLIC cxx_std_17)
set_target_properties(snmpwrap PROPERTIES
    CXX_EXTENSIONS OFF
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
    # installed shared library still finds a Net-SNMP in a custom prefix
    INSTALL_RPATH_USE_LINK_PATH TRUE)

# net-snmp stays an implementation detail: no net-snmp header is included by a public header.
target_link_libraries(snmpwrap PRIVATE NetSNMP::NetSNMP)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(snmpwrap PRIVATE -Wall -Wextra -Wpedantic)
endif()

# --- MIB code generator ---------------------------------------------------------------------
if(SNMPWRAP_BUILD_MIBGEN)
    add_executable(snmpwrap-mibgen tools/mibgen/main.cpp tools/mibgen/generator.cpp)
    target_link_libraries(snmpwrap-mibgen PRIVATE snmpwrap)
    # the installed generator runs during other projects' builds: it must find libsnmpwrap
    # (next to it in ../lib) and Net-SNMP (wherever it was linked from) without LD_LIBRARY_PATH
    set_target_properties(snmpwrap-mibgen PROPERTIES
        EXPORT_NAME mibgen
        CXX_EXTENSIONS OFF
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
        INSTALL_RPATH_USE_LINK_PATH TRUE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(snmpwrap-mibgen PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_executable(snmpwrap::mibgen ALIAS snmpwrap-mibgen)
    include(SnmpwrapMibGen)   # snmpwrap_add_mib()
endif()

# --- examples / tests -----------------------------------------------------------------------
if(SNMPWRAP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(SNMPWRAP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# --- install --------------------------------------------------------------------------------
if(SNMPWRAP_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS snmpwrap EXPORT snmpwrapTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(SNMPWRAP_BUILD_MIBGEN)
        install(TARGETS snmpwrap-mibgen EXPORT snmpwrapTargets RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
    install(DIRECTORY include/snmpwrap DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    set(_cfg_dir ${CMAKE_INSTALL_LIBDIR}/cmake/snmpwrap)
    install(EXPORT snmpwrapTargets NAMESPACE snmpwrap:: DESTINATION ${_cfg_dir})

    configure_package_config_file(cmake/snmpwrapConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/snmpwrapConfig.cmake
        INSTALL_DESTINATION ${_cfg_dir})
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/snmpwrapConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/snmpwrapConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/snmpwrapConfigVersion.cmake
        cmake/FindNetSNMP.cmake
        cmake/SnmpwrapMibGen.cmake
        DESTINATION ${_cfg_dir})
endif()
