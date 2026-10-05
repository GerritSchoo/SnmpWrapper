#include "snmpwrap/agent.hpp"

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

#include <atomic>
#include <csignal>
#include <optional>
#include <string>

#include "convert.hpp"

namespace snmpwrap {

namespace {

std::atomic<bool> g_agentExists{false};

/// Per registered subtree; pointer is stored in the net-snmp handler (`myvoid`).
struct Registration {
    Oid root;
    std::shared_ptr<Handler> handler;
};

/// Transaction state of one SET request for one registration. Attached to the agent request info
/// (it lives across the RESERVE1 / ACTION / COMMIT / UNDO phases) and freed together with it.
struct TxnHolder {
    std::unique_ptr<SetTransaction> txn;
};

void freeTxnHolder(void* p) { delete static_cast<TxnHolder*>(p); }

std::string txnKey(const Registration& reg) { return "snmpwrap.txn." + reg.root.str(); }

Oid requestOid(const netsnmp_request_info* r) {
    return detail::fromNetOid(r->requestvb->name, r->requestvb->name_length);
}

void doGet(Registration& reg, netsnmp_agent_request_info* ari, netsnmp_request_info* requests) {
    for (auto* r = requests; r; r = r->next) {
        if (r->processed) continue;
        try {
            const Oid oid = requestOid(r);
            auto v = reg.handler->get(oid);
            if (v)
                detail::assign(r->requestvb, *v);
            else
                netsnmp_set_request_error(ari, r, reg.handler->missing(oid) == Type::NoSuchObject ? SNMP_NOSUCHOBJECT
                                                                                                  : SNMP_NOSUCHINSTANCE);
        } catch (const std::exception&) {
            netsnmp_set_request_error(ari, r, SNMP_ERR_GENERR);
        }
    }
}

void doGetNext(Registration& reg, netsnmp_agent_request_info* ari, netsnmp_request_info* requests) {
    for (auto* r = requests; r; r = r->next) {
        if (r->processed) continue;
        try {
            const Oid start = requestOid(r);
            std::optional<VarBind> hit;
            if (r->inclusive) {
                if (auto v = reg.handler->get(start)) hit = VarBind{start, std::move(*v)};
            }
            if (!hit) hit = reg.handler->getNext(start);
            // Nothing (more) in this subtree: leave the request untouched so the agent core
            // continues with the next registered subtree.
            if (!hit || !reg.root.isPrefixOf(hit->oid)) continue;
            auto name = detail::toNetOid(hit->oid);
            snmp_set_var_objid(r->requestvb, name.data(), name.size());
            detail::assign(r->requestvb, hit->value);
        } catch (const std::exception&) {
            netsnmp_set_request_error(ari, r, SNMP_ERR_GENERR);
        }
    }
}

/// Reports `e` on the varbind it names (index into the request list), or on the first one.
void failSet(netsnmp_agent_request_info* ari, netsnmp_request_info* requests, const SetError& e) {
    netsnmp_request_info* target = requests;
    if (e.index() != SetError::kUnknownIndex) {
        std::size_t k = 0;
        for (auto* r = requests; r; r = r->next, ++k)
            if (k == e.index()) {
                target = r;
                break;
            }
    }
    netsnmp_set_request_error(ari, target, static_cast<int>(e.status()));
}

void doSet(Registration& reg, int mode, netsnmp_agent_request_info* ari, netsnmp_request_info* requests) {
    const std::string key = txnKey(reg);
    auto* holder = static_cast<TxnHolder*>(netsnmp_agent_get_list_data(ari, key.c_str()));

    switch (mode) {
        case MODE_SET_RESERVE1: {
            std::vector<VarBind> sets;
            for (auto* r = requests; r; r = r->next) {
                try {
                    sets.push_back({requestOid(r), detail::fromVar(r->requestvb)});
                } catch (const std::exception&) {
                    netsnmp_set_request_error(ari, r, SNMP_ERR_WRONGTYPE);  // ASN.1 type snmpwrap cannot represent
                    return;
                }
            }
            try {
                auto txn = reg.handler->prepare(sets);
                if (!txn) {
                    netsnmp_set_request_error(ari, requests, SNMP_ERR_GENERR);
                    return;
                }
                auto* h = new TxnHolder{std::move(txn)};
                netsnmp_agent_add_list_data(ari, netsnmp_create_data_list(key.c_str(), h, freeTxnHolder));
            } catch (const SetError& e) {
                failSet(ari, requests, e);
            } catch (const std::exception&) {
                netsnmp_set_request_error(ari, requests, SNMP_ERR_GENERR);
            }
            break;
        }
        case MODE_SET_ACTION:
            if (!holder) return;
            try {
                holder->txn->apply();
            } catch (const SetError& e) {
                failSet(ari, requests, e);
            } catch (const std::exception&) {
                netsnmp_set_request_error(ari, requests, SNMP_ERR_COMMITFAILED);
            }
            break;
        case MODE_SET_COMMIT:
            if (holder) holder->txn->commit();
            break;
        case MODE_SET_UNDO:
            if (holder) holder->txn->undo();
            break;
        default:  // RESERVE2, FREE: the holder is released together with the request info
            break;
    }
}

// Exceptions must never propagate through net-snmp's C frames.
int dispatch(netsnmp_mib_handler* nh, netsnmp_handler_registration*, netsnmp_agent_request_info* ari,
             netsnmp_request_info* requests) {
    auto& reg = *static_cast<Registration*>(nh->myvoid);
    try {
        switch (ari->mode) {
            case MODE_GET: doGet(reg, ari, requests); break;
            case MODE_GETNEXT: doGetNext(reg, ari, requests); break;  // GETBULK arrives as GETNEXT
            case MODE_SET_RESERVE1:
            case MODE_SET_RESERVE2:
            case MODE_SET_ACTION:
            case MODE_SET_COMMIT:
            case MODE_SET_FREE:
            case MODE_SET_UNDO: doSet(reg, ari->mode, ari, requests); break;
            default: return SNMP_ERR_GENERR;
        }
    } catch (...) {
        return SNMP_ERR_GENERR;
    }
    return SNMP_ERR_NOERROR;
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

void Agent::addHandler(const Oid& root, std::shared_ptr<Handler> handler) {
    if (!handler) throw Error("null handler");
    if (root.empty()) throw Error("empty root OID");

    auto reg = std::make_unique<Registration>(Registration{root, std::move(handler)});
    auto name = detail::toNetOid(root);
    const std::string label = "snmpwrap:" + root.str();

    netsnmp_handler_registration* nreg =
        netsnmp_create_handler_registration(label.c_str(), dispatch, name.data(), name.size(), HANDLER_CAN_RWRITE);
    if (!nreg) throw Error("netsnmp_create_handler_registration failed");
    nreg->handler->myvoid = reg.get();

    const int rc = netsnmp_register_handler(nreg);  // net-snmp owns nreg from here on
    if (rc != MIB_REGISTERED_OK) throw Error("registering " + root.str() + " failed (code " + std::to_string(rc) + ")");
    impl_->registrations.push_back(std::move(reg));
}

Mib& Agent::addMib(const Oid& root) {
    auto mib = std::make_shared<Mib>(root);
    addHandler(root, mib);
    impl_->mibs.push_back(mib);
    return *mib;
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
    // blocking waits at most until the next alarm: the 1 s heartbeat (or an earlier net-snmp timer)
    agent_check_and_process(block ? 1 : 0);
    return impl_->running;
}

void Agent::run() {
    start();
    while (impl_->running) agent_check_and_process(1);
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
