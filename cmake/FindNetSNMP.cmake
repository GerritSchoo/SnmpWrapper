#[=======================================================================[.rst:
FindNetSNMP
-----------

Finds Net-SNMP (>= 5.9) through its ``net-snmp-config`` script and provides the imported target
``NetSNMP::NetSNMP`` (client + agent libraries).

Hints:
  ``NETSNMP_CONFIG_EXECUTABLE``  path to ``net-snmp-config``
  ``NetSNMP_ROOT`` / ``CMAKE_PREFIX_PATH``  install prefix (``<prefix>/bin/net-snmp-config``)
#]=======================================================================]

find_program(NETSNMP_CONFIG_EXECUTABLE net-snmp-config
    HINTS ${NetSNMP_ROOT} ENV NetSNMP_ROOT
    PATH_SUFFIXES bin)

if(NETSNMP_CONFIG_EXECUTABLE)
    execute_process(COMMAND ${NETSNMP_CONFIG_EXECUTABLE} --version
        OUTPUT_VARIABLE NetSNMP_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${NETSNMP_CONFIG_EXECUTABLE} --base-cflags
        OUTPUT_VARIABLE _nsnmp_cflags OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${NETSNMP_CONFIG_EXECUTABLE} --agent-libs
        OUTPUT_VARIABLE _nsnmp_libs OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${NETSNMP_CONFIG_EXECUTABLE} --prefix
        OUTPUT_VARIABLE NetSNMP_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)

    # --base-cflags: "-I/prefix/include -g -O2 ..." -> keep only -I and -D, the rest are build flags of net-snmp itself
    separate_arguments(_nsnmp_cflags_list UNIX_COMMAND "${_nsnmp_cflags}")
    set(NetSNMP_INCLUDE_DIRS "")
    set(NetSNMP_DEFINITIONS "")
    foreach(f IN LISTS _nsnmp_cflags_list)
        if(f MATCHES "^-I(.+)")
            list(APPEND NetSNMP_INCLUDE_DIRS "${CMAKE_MATCH_1}")
        elseif(f MATCHES "^-D(.+)" AND NOT f MATCHES "^-Dlinux=")  # -Dlinux=linux is a leftover of net-snmp's own build
            list(APPEND NetSNMP_DEFINITIONS "${CMAKE_MATCH_1}")
        endif()
    endforeach()

    # --agent-libs: "-L/prefix/lib -lnetsnmpmibs -lnetsnmpagent -lnetsnmp -lm ..."
    # The net-snmp libraries are resolved to full paths, so CMake puts their directory into the RPATH
    # of executables (needed for a net-snmp in a custom prefix, e.g. when the build runs snmpwrap-mibgen).
    separate_arguments(_nsnmp_libs_list UNIX_COMMAND "${_nsnmp_libs}")
    set(NetSNMP_LIBRARY_DIRS "")
    foreach(f IN LISTS _nsnmp_libs_list)
        if(f MATCHES "^-L(.+)")
            list(APPEND NetSNMP_LIBRARY_DIRS "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    set(NetSNMP_LIBRARIES "")
    foreach(f IN LISTS _nsnmp_libs_list)
        if(f MATCHES "^-l(netsnmp.*)")
            set(_lib_name "${CMAKE_MATCH_1}")
            find_library(NetSNMP_LIB_${_lib_name} NAMES ${_lib_name} HINTS ${NetSNMP_LIBRARY_DIRS} NO_DEFAULT_PATH)
            find_library(NetSNMP_LIB_${_lib_name} NAMES ${_lib_name})
            mark_as_advanced(NetSNMP_LIB_${_lib_name})
            if(NetSNMP_LIB_${_lib_name})
                list(APPEND NetSNMP_LIBRARIES "${NetSNMP_LIB_${_lib_name}}")
            else()
                list(APPEND NetSNMP_LIBRARIES "${f}")
            endif()
        elseif(NOT f MATCHES "^-L")
            list(APPEND NetSNMP_LIBRARIES "${f}")  # system libraries such as -lm, -lssl
        endif()
    endforeach()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NetSNMP
    REQUIRED_VARS NETSNMP_CONFIG_EXECUTABLE NetSNMP_LIBRARIES
    VERSION_VAR NetSNMP_VERSION)

if(NetSNMP_FOUND AND NOT TARGET NetSNMP::NetSNMP)
    add_library(NetSNMP::NetSNMP INTERFACE IMPORTED)
    set_target_properties(NetSNMP::NetSNMP PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${NetSNMP_INCLUDE_DIRS}"
        INTERFACE_COMPILE_DEFINITIONS "${NetSNMP_DEFINITIONS}"
        INTERFACE_LINK_LIBRARIES "${NetSNMP_LIBRARIES}")
    # net-snmp installs shared libs with an rpath-less prefix; make executables find them
    if(NetSNMP_LIBRARY_DIRS)
        set_property(TARGET NetSNMP::NetSNMP PROPERTY INTERFACE_LINK_DIRECTORIES "${NetSNMP_LIBRARY_DIRS}")
    endif()
endif()

mark_as_advanced(NETSNMP_CONFIG_EXECUTABLE)
