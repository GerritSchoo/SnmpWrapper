#[=======================================================================[.rst:
snmpwrap_add_mib
----------------

Generates typed C++ code for a MIB module with ``snmpwrap-mibgen`` and adds it to a target.

::

  snmpwrap_add_mib(<target>
      MODULE <MODULE-NAME>              # e.g. SNMPWRAPPER-TEST-MIB
      MIB <file> [<file>...]            # the MIB file(s) to read
      [MIB_DIRS <dir>...]               # where imported modules are searched (Net-SNMP's default dir is always used)
      [NAME <base>]                     # file / namespace name, default: module in snake_case
      [ROOT <node>]                     # registration root, default: common prefix of all objects
      [OUTPUT_DIR <dir>])               # default: ${CMAKE_CURRENT_BINARY_DIR}/snmpwrap_generated

The code is regenerated whenever a MIB file or the generator changes. In your sources::

  #include "<base>.hpp"      // e.g. snmpwrapper_test_mib.hpp, namespace snmpwrapper_test_mib

Recommended: one library per MIB, so the code is generated once and shared by agent and client::

  add_library(my_mib STATIC)
  snmpwrap_add_mib(my_mib MODULE MY-MIB MIB mibs/MY-MIB.txt
                   OUTPUT_DIR ${PROJECT_SOURCE_DIR}/generated/my_mib)   # optional: keep the code visible in the source tree
  target_link_libraries(my_agent PRIVATE my_mib)
  target_link_libraries(my_client PRIVATE my_mib)

For library targets the include directory and the snmpwrap dependency are PUBLIC.
#]=======================================================================]

function(snmpwrap_add_mib target)
    cmake_parse_arguments(ARG "" "MODULE;NAME;ROOT;OUTPUT_DIR" "MIB;MIB_DIRS" ${ARGN})
    if(NOT ARG_MODULE OR NOT ARG_MIB)
        message(FATAL_ERROR "snmpwrap_add_mib(${target}): MODULE and MIB are required")
    endif()
    if(NOT TARGET snmpwrap::mibgen)
        message(FATAL_ERROR "snmpwrap_add_mib: the generator target snmpwrap::mibgen is not available")
    endif()
    if(NOT ARG_NAME)
        string(TOLOWER "${ARG_MODULE}" ARG_NAME)
        string(REGEX REPLACE "[^a-z0-9]" "_" ARG_NAME "${ARG_NAME}")
    endif()
    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/snmpwrap_generated")
    endif()

    set(mibs "")
    foreach(f IN LISTS ARG_MIB)
        get_filename_component(abs "${f}" ABSOLUTE)
        list(APPEND mibs "${abs}")
    endforeach()
    set(dir_args "")
    foreach(d IN LISTS ARG_MIB_DIRS)
        get_filename_component(abs "${d}" ABSOLUTE)
        list(APPEND dir_args --mib-dir "${abs}")
    endforeach()
    set(root_args "")
    if(ARG_ROOT)
        set(root_args --root "${ARG_ROOT}")
    endif()

    # build order: depend on the real generator target when it is part of this build
    get_target_property(gen_real snmpwrap::mibgen ALIASED_TARGET)
    if(gen_real)
        set(gen_dep ${gen_real})
    else()
        set(gen_dep "$<TARGET_FILE:snmpwrap::mibgen>")
    endif()

    set(hpp "${ARG_OUTPUT_DIR}/${ARG_NAME}.hpp")
    set(cpp "${ARG_OUTPUT_DIR}/${ARG_NAME}.cpp")
    add_custom_command(
        OUTPUT "${hpp}" "${cpp}"
        COMMAND "$<TARGET_FILE:snmpwrap::mibgen>" --module "${ARG_MODULE}" --out "${ARG_OUTPUT_DIR}"
                --name "${ARG_NAME}" ${root_args} ${dir_args} ${mibs}
        DEPENDS ${mibs} ${gen_dep}
        COMMENT "snmpwrap-mibgen: ${ARG_MODULE} -> ${ARG_NAME}.hpp/.cpp"
        VERBATIM)
    # a library made from a MIB passes its generated header and snmpwrap on to its users
    get_target_property(target_type ${target} TYPE)
    if(target_type MATCHES "LIBRARY")
        set(visibility PUBLIC)
    else()
        set(visibility PRIVATE)
    endif()
    target_sources(${target} PRIVATE "${hpp}" "${cpp}")
    target_include_directories(${target} ${visibility} "${ARG_OUTPUT_DIR}")
    target_link_libraries(${target} ${visibility} snmpwrap::snmpwrap)
endfunction()
