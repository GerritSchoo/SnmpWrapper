/**
 * @file agent.hpp
 * @brief The SNMP agent: an AgentX subagent that serves your data through a running snmpd.
 *
 * How it fits together:
 * @verbatim
 *   SNMP manager --(v1/v2c/v3, UDP 161)--> snmpd (master) --(AgentX)--> your process: snmpwrap::Agent --> Mib / Handler --> your data
 * @endverbatim
 * snmpd handles the protocol versions, communities, SNMPv3 users and access control; your process
 * only answers for the OID subtrees it registered.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "snmpwrap/mib.hpp"

namespace snmpwrap {

/**
 * @brief Settings of an Agent.
 */
struct AgentConfig {
    /// Application name, used for Net-SNMP logging and its config file lookup ("<name>.conf").
    std::string name = "snmpwrap-agent";
    /// Address of the master agent (snmpd with "master agentx"), e.g. "tcp:127.0.0.1:705" or
    /// "/var/agentx/master" (Unix socket, snmpd's default; needs matching permissions).
    std::string agentxSocket = "tcp:127.0.0.1:705";
    /// Seconds between AgentX pings; the subagent reconnects automatically after a master restart. 0 = off.
    int pingIntervalSec = 15;
    /// Send Net-SNMP log output to stderr.
    bool logToStderr = true;
    /// Ignore SIGPIPE so a vanishing master cannot kill the process. Affects the whole process.
    bool ignoreSigpipe = true;
};

/**
 * @brief SNMP AgentX subagent.
 *
 * Connects to a running snmpd (master) and answers requests for the registered OID subtrees.
 * MIB-independent: it only deals with OIDs and typed values.
 *
 * @code
 * Agent agent({"my-agent", "tcp:127.0.0.1:705"});
 * Mib& mib = agent.addMib(Oid::parse("1.3.6.1.4.1.99999"));
 * mib.scalar({1, 1}, {Type::OctetString, [] { return Value::string("hello"); }});
 * agent.run();   // until agent.stop() is called (e.g. from a signal handler)
 * @endcode
 *
 * @note Net-SNMP keeps process-wide state:
 *       - only ONE Agent may exist per process at a time;
 *       - run(), poll(), addHandler(), addMib() and sendTrap() belong to the single thread running
 *         the loop, and all Handler / Mib callbacks are called from that thread – protect data that
 *         other threads touch with a mutex; stop() is the only thread- and signal-safe member;
 *       - with snmpwrap::Client in the same process, construct the Agent FIRST (the constructor
 *         throws otherwise) and do not use Clients after the Agent was destroyed (its destructor
 *         shuts the Net-SNMP library down).
 * @note Protocol versions, communities and SNMPv3 users are configured in snmpd.conf
 *       (rocommunity / rwcommunity / createUser / rouser / rwuser). Counter64 values are invisible
 *       to SNMPv1 managers (snmpd skips / rejects them).
 */
class Agent {
public:
    /**
     * @brief Initializes Net-SNMP as AgentX subagent (does not connect yet, see start()).
     * @param[in] config Name, master address and options.
     * @throws Error if another Agent exists, a Client was created before, or init_agent() fails.
     */
    explicit Agent(AgentConfig config = {});

    /// @brief Disconnects and shuts down the Net-SNMP library.
    ~Agent();

    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    /**
     * @brief Registers a handler for an OID subtree.
     * @param[in] root    Subtree root; all requests for OIDs below it are routed to @p handler.
     * @param[in] handler Your handler (shared ownership; kept alive by the Agent).
     * @throws Error if @p handler is null, @p root is empty or Net-SNMP refuses the registration
     *         (e.g. the same subtree is already registered).
     * @note Can be called before or after start().
     */
    void addHandler(const Oid& root, std::shared_ptr<Handler> handler);

    /**
     * @brief Creates a Mib below @p root, registers it and returns it for adding scalars and tables.
     * @param[in] root Subtree root, e.g. your enterprise OID.
     * @return The new Mib; the reference stays valid for the lifetime of the Agent.
     * @throws Error as addHandler().
     */
    Mib& addMib(const Oid& root);

    /**
     * @brief Connects to the master agent (idempotent).
     *
     * Called implicitly by run(), poll() and sendTrap(). If the master is not reachable yet,
     * Net-SNMP keeps retrying in the background (see AgentConfig::pingIntervalSec).
     */
    void start();

    /// @brief Processes requests until stop() is called.
    void run();

    /**
     * @brief Processes pending requests once – for integration into your own main loop.
     * @param[in] block True: wait for work, but at most about one second (internal heartbeat).
     *                  False: return immediately.
     * @return False once stop() was called, true otherwise.
     *
     * @code
     * while (agent.poll()) {
     *     doOtherWork();   // runs at least once per second
     * }
     * @endcode
     */
    bool poll(bool block = true);

    /// @brief Makes run() / poll() return within about one second. Thread- and signal-safe.
    void stop() noexcept;

    /**
     * @brief Sends an SNMPv2 notification through the master.
     * @param[in] trapOid The NOTIFICATION-TYPE OID (becomes snmpTrapOID.0).
     * @param[in] vars    Additional varbinds (the OBJECTS of the notification).
     * @throws Error if a varbind cannot be encoded.
     * @note sysUpTime.0 and snmpTrapOID.0 are added automatically. snmpd forwards the notification
     *       to its configured sinks (trapsink = v1, trap2sink / informsink = v2c, trapsess = v3).
     *       It is dropped if the master is not connected.
     */
    void sendTrap(const Oid& trapOid, const std::vector<VarBind>& vars = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace snmpwrap
