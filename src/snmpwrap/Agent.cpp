#include "snmpwrap/Agent.hpp"

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <memory>
#include <optional>
#include <string>

#include "AgentDetail.hpp"
#include "Convert.hpp"

namespace snmpwrap {

namespace {

std::atomic<bool> g_agentExists{false};

/// > 0 while Net-SNMP is inside dispatch(): unregistering a handler then would pull it from under the request.
int g_dispatchDepth = 0;

using detail::Registration;

// Net-SNMP's handler: the subtree answers, see detail::Registration::handle().
int dispatch(netsnmp_mib_handler* nh, netsnmp_handler_registration*, netsnmp_agent_request_info* ari,
             netsnmp_request_info* requests) {
    struct Depth {
        Depth() { ++g_dispatchDepth; }
        ~Depth() { --g_dispatchDepth; }
    } depth;
    return static_cast<Registration*>(nh->myvoid)->handle(ari, requests);
}

// Keeps the select() timeout finite so stop() is noticed within a second.
void heartbeat(unsigned int, void*) {}

}  // namespace

struct Agent::Impl {
    AgentConfig config;
    std::atomic<bool> running{true};
    bool started = false;
    bool agentInitialized = false;
    std::vector<std::unique_ptr<Registration>> registrations;
    std::vector<std::shared_ptr<Mib>> mibs;

    /// Unregisters removed subtrees from Net-SNMP, unless a request is being processed or still refers to them.
    void flushRemovals() {
        if (g_dispatchDepth > 0) return;
        for (auto it = registrations.begin(); it != registrations.end();) {
            Registration& r = **it;
            if (!r.canBeUnregistered()) {
                ++it;
                continue;
            }
            netsnmp_unregister_handler(r.netsnmpRegistration());  // also tells the master (AgentX unregister); frees nreg
            mibs.erase(std::remove_if(mibs.begin(), mibs.end(), [&](const auto& m) { return m.get() == r.handler().get(); }),
                       mibs.end());
            it = registrations.erase(it);
        }
    }
};

Agent::Agent(AgentConfig config) : impl_(std::make_unique<Impl>()) {
    if (g_agentExists.exchange(true)) throw Error("only one snmpwrap::Agent per process is supported");
    impl_->config = std::move(config);
    const auto& c = impl_->config;
    if (detail::libraryInitialized()) {
        g_agentExists = false;
        throw Error("snmpwrap::Agent must be created before the first snmpwrap::Client: net-snmp is already "
                    "initialized and the AgentX connection to the master could not be established");
    }
    try {
        if (c.ignoreSigpipe) std::signal(SIGPIPE, SIG_IGN);
        if (c.logToStderr) snmp_enable_stderrlog();
        netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_DONT_PERSIST_STATE, 1);
        netsnmp_ds_set_boolean(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_AGENT_ROLE, 1);  // subagent
        netsnmp_ds_set_string(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_AGENT_X_SOCKET, c.agentxSocket.c_str());
        if (c.pingIntervalSec > 0)
            netsnmp_ds_set_int(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_AGENT_AGENTX_PING_INTERVAL, c.pingIntervalSec);
        SOCK_STARTUP;
        if (init_agent(c.name.c_str()) != 0) throw Error("init_agent failed");
        impl_->agentInitialized = true;
    } catch (...) {
        g_agentExists = false;
        throw;
    }
}

Agent::~Agent() {
    // Also shuts down the library for Clients of this process (net-snmp state is process-wide).
    if (impl_->agentInitialized) detail::shutdownLibrary(impl_->config.name.c_str());
    SOCK_CLEANUP;
    g_agentExists = false;
}

void Agent::addHandler(const Oid& root, std::shared_ptr<IHandler> handler) {
    if (!handler) throw Error("null handler");
    if (root.empty()) throw Error("empty root OID");
    impl_->flushRemovals();  // a subtree removed just before can be registered again

    auto reg = std::make_unique<Registration>(root, std::move(handler));
    auto name = detail::toNetOid(root);
    const std::string label = "snmpwrap:" + root.str();

    netsnmp_handler_registration* nreg =
        netsnmp_create_handler_registration(label.c_str(), dispatch, name.data(), name.size(), HANDLER_CAN_RWRITE);
    if (!nreg) throw Error("netsnmp_create_handler_registration failed");
    nreg->handler->myvoid = reg.get();

    const int rc = netsnmp_register_handler(nreg);  // net-snmp owns nreg from here on
    if (rc != MIB_REGISTERED_OK) throw Error("registering " + root.str() + " failed (code " + std::to_string(rc) + ")");
    reg->setNetsnmpRegistration(nreg);
    impl_->registrations.push_back(std::move(reg));
}

void Agent::removeHandler(const Oid& root) {
    auto& regs = impl_->registrations;
    auto it = std::find_if(regs.begin(), regs.end(), [&](const auto& r) { return !r->removed() && r->root() == root; });
    if (it == regs.end()) throw Error("no handler registered at " + root.str());
    (*it)->markRemoved();
    impl_->flushRemovals();
}

void Agent::removeMib(const Mib& mib) {
    auto& regs = impl_->registrations;
    auto it = std::find_if(regs.begin(), regs.end(), [&](const auto& r) { return !r->removed() && r->handler().get() == &mib; });
    if (it == regs.end()) throw Error("Mib at " + mib.root().str() + " was not created by this Agent (or is removed already)");
    (*it)->markRemoved();
    impl_->flushRemovals();
}

std::shared_ptr<Mib> Agent::addMib(const Oid& root) {
    auto mib = std::make_shared<Mib>(root);
    addHandler(root, mib);
    impl_->mibs.push_back(mib);
    return mib;
}

void Agent::start() {
    if (impl_->started) return;
    detail::initLibrary(impl_->config.name.c_str());  // fires the AgentX connect (POST_READ_CONFIG)
    snmp_alarm_register(1, SA_REPEAT, heartbeat, nullptr);
    impl_->started = true;
}

bool Agent::poll(bool block) {
    start();
    if (!impl_->running) return false;
    impl_->flushRemovals();
    // blocking waits at most until the next alarm: the 1 s heartbeat (or an earlier net-snmp timer)
    agent_check_and_process(block ? 1 : 0);
    return impl_->running;
}

void Agent::run() {
    start();
    while (impl_->running) {
        impl_->flushRemovals();
        agent_check_and_process(1);
    }
}

void Agent::stop() noexcept { impl_->running = false; }

void Agent::sendTrap(const Oid& trapOid, const std::vector<VarBind>& vars) {
    start();
    netsnmp_variable_list* list = nullptr;
    auto add = [&](const Oid& name, const Value& value) {
        auto n = detail::toNetOid(name);
        netsnmp_variable_list* var = snmp_varlist_add_variable(&list, n.data(), n.size(), ASN_NULL, nullptr, 0);
        if (!var) throw Error("snmp_varlist_add_variable failed");
        detail::assign(var, value);
    };
    try {
        add(Oid{1, 3, 6, 1, 2, 1, 1, 3, 0}, Value::timeTicks(static_cast<std::uint32_t>(netsnmp_get_agent_uptime())));
        add(Oid{1, 3, 6, 1, 6, 3, 1, 1, 4, 1, 0}, Value::oid(trapOid));
        for (const auto& vb : vars) add(vb.oid, vb.value);
    } catch (...) {
        snmp_free_varbind(list);
        throw;
    }
    send_v2trap(list);
    snmp_free_varbind(list);
}

}  // namespace snmpwrap
