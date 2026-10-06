#include "snmpwrap/notification.hpp"

#include <sys/select.h>

#include <atomic>
#include <cstdlib>

#include "convert.hpp"
#include "snmpwrap/error.hpp"

namespace snmpwrap {

namespace {

const Oid kSysUpTime{1, 3, 6, 1, 2, 1, 1, 3, 0};
const Oid kSnmpTrapOid{1, 3, 6, 1, 6, 3, 1, 1, 4, 1, 0};
const Oid kSnmpTraps{1, 3, 6, 1, 6, 3, 1, 1, 5};  // generic v1 traps: snmpTraps.(generic + 1)

}  // namespace

struct NotificationReceiver::Impl {
    void* handle = nullptr;  // from snmp_sess_add()
    std::vector<std::function<void(const Notification&)>> handlers;
    std::atomic<bool> running{true};

    ~Impl() {
        if (handle) snmp_sess_close(handle);
    }

    static int callback(int op, netsnmp_session*, int, netsnmp_pdu* pdu, void* magic) {
        if (op != NETSNMP_CALLBACK_OP_RECEIVED_MESSAGE || !pdu) return 1;
        auto* self = static_cast<Impl*>(magic);
        try {
            self->received(pdu);
        } catch (...) {  // never let exceptions reach Net-SNMP's C frames
        }
        return 1;
    }

    void received(netsnmp_pdu* pdu) {
        Notification n;
        if (pdu->community) n.community.assign(reinterpret_cast<const char*>(pdu->community), pdu->community_len);
        if (netsnmp_transport* t = snmp_sess_transport(handle); t && t->f_fmtaddr) {
            char* addr = t->f_fmtaddr(t, pdu->transport_data, pdu->transport_data_length);
            if (addr) n.source = addr;
            free(addr);
        }

        switch (pdu->command) {
            case SNMP_MSG_TRAP: {  // SNMPv1: enterprise / generic / specific -> snmpTrapOID (RFC 3584 3.1)
                n.version = 1;
                n.uptime = static_cast<std::uint32_t>(pdu->time);
                if (pdu->trap_type != SNMP_TRAP_ENTERPRISESPECIFIC) {
                    n.trapOid = kSnmpTraps + static_cast<SubId>(pdu->trap_type + 1);
                } else {
                    n.trapOid = detail::fromNetOid(pdu->enterprise, pdu->enterprise_length) + SubId{0} +
                                static_cast<SubId>(pdu->specific_type);
                }
                break;
            }
            case SNMP_MSG_TRAP2:
            case SNMP_MSG_INFORM:
                n.version = 2;
                n.inform = pdu->command == SNMP_MSG_INFORM;
                break;
            default:
                return;  // not a notification
        }

        for (netsnmp_variable_list* v = pdu->variables; v; v = v->next_variable) {
            VarBind vb{detail::fromNetOid(v->name, v->name_length), Value()};
            try {
                vb.value = detail::fromVar(v);
            } catch (const Error&) {
                continue;  // a type snmpwrap cannot represent
            }
            if (n.version == 2 && vb.oid == kSysUpTime && vb.value.type() == Type::TimeTicks) {
                n.uptime = vb.value.asUInt();
            } else if (n.version == 2 && vb.oid == kSnmpTrapOid && vb.value.type() == Type::ObjectId) {
                n.trapOid = vb.value.asOid();
            } else {
                n.vars.push_back(std::move(vb));
            }
        }

        if (n.inform) {  // acknowledge: the same PDU back as a response
            netsnmp_pdu* reply = snmp_clone_pdu(pdu);
            if (reply) {
                reply->command = SNMP_MSG_RESPONSE;
                reply->errstat = 0;
                reply->errindex = 0;
                if (!snmp_sess_send(handle, reply)) snmp_free_pdu(reply);
            }
        }

        for (const auto& h : handlers) {
            try {
                h(n);
            } catch (...) {
            }
        }
    }
};

NotificationReceiver::NotificationReceiver(const std::string& address) : impl_(std::make_unique<Impl>()) {
    detail::initLibrary("snmpwrap-receiver");

    netsnmp_transport* transport = netsnmp_transport_open_server("snmptrap", address.c_str());
    if (!transport) throw TransportError("cannot listen on " + address + " (address in use, or no permission for ports < 1024?)");

    netsnmp_session sess;
    snmp_sess_init(&sess);
    sess.peername = SNMP_DEFAULT_PEERNAME;
    sess.version = SNMP_DEFAULT_VERSION;
    sess.community_len = SNMP_DEFAULT_COMMUNITY_LEN;
    sess.retries = SNMP_DEFAULT_RETRIES;
    sess.timeout = SNMP_DEFAULT_TIMEOUT;
    sess.callback = &Impl::callback;
    sess.callback_magic = impl_.get();
    sess.isAuthoritative = SNMP_SESS_UNKNOWNAUTH;

    impl_->handle = snmp_sess_add(&sess, transport, nullptr, nullptr);  // takes over the transport
    if (!impl_->handle) throw TransportError("cannot open a notification session on " + address);
}

NotificationReceiver::~NotificationReceiver() = default;

void NotificationReceiver::onNotification(std::function<void(const Notification&)> handler) {
    if (handler) impl_->handlers.push_back(std::move(handler));
}

bool NotificationReceiver::poll(std::chrono::milliseconds timeout) {
    int numfds = 0;
    fd_set fds;
    FD_ZERO(&fds);
    struct timeval tv {};
    int block = 1;
    snmp_sess_select_info(impl_->handle, &numfds, &fds, &tv, &block);
    struct timeval wait {};
    wait.tv_sec = static_cast<long>(timeout.count() / 1000);
    wait.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    const int n = select(numfds, &fds, nullptr, nullptr, &wait);
    if (n > 0) {
        snmp_sess_read(impl_->handle, &fds);
        return true;
    }
    if (n == 0) snmp_sess_timeout(impl_->handle);
    return false;
}

void NotificationReceiver::run() {
    while (impl_->running) poll(std::chrono::milliseconds(1000));
}

void NotificationReceiver::stop() noexcept { impl_->running = false; }

}  // namespace snmpwrap
