#pragma once

// Internal: the only place (together with agent.cpp / client.cpp) that touches net-snmp types.

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

#include <vector>

#include "snmpwrap/oid.hpp"
#include "snmpwrap/value.hpp"

namespace snmpwrap::detail {

std::vector<::oid> toNetOid(const Oid& o);
Oid fromNetOid(const ::oid* name, std::size_t len);

/// Stores `v` (type and data) into `var`, keeping its name.
void assign(netsnmp_variable_list* var, const Value& v);

/// Throws snmpwrap::Error for unsupported ASN.1 types.
Value fromVar(const netsnmp_variable_list* var);

// --- process-wide library state ------------------------------------------------------------
// net-snmp's init_snmp() runs only once per process (init_snmp_init_done). An AgentX subagent
// connects to its master from a POST_READ_CONFIG callback that init_agent() registers and that
// init_snmp() fires - so init_agent() must come BEFORE the first init_snmp() in the process.

/// Calls init_snmp(appName) unless the library is already initialized.
void initLibrary(const char* appName);
/// True once init_snmp() ran (and until shutdownLibrary()).
bool libraryInitialized();
/// snmp_shutdown(appName) and reset of the state above.
void shutdownLibrary(const char* appName);

}  // namespace snmpwrap::detail
