#include "snmpwrap/NotificationReceiver.hpp"

#include <sys/select.h>

#include <atomic>
#include <cstdlib>

#include "Convert.hpp"
#include "NotificationDetail.hpp"
#include "snmpwrap/Error.hpp"

namespace snmpwrap {


struct NotificationReceiver::Impl {
    void* handle = nullptr;  // from snmp_sess_add()
    std::vector<std::function<void(const Notification&)>> handlers;
    detail::NotificationDecoder decoder;
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
        std::string source;
        if (netsnmp_transport* t = snmp_sess_transport(handle); t && t->f_fmtaddr) {
            char* addr = t->f_fmtaddr(t, pdu->transport_data, pdu->transport_data_length);
            if (addr) source = addr;
            free(addr);
        }
        auto received = decoder.decode(*pdu, std::move(source));
        if (!received) return;
        const Notification& n = *received;

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

void NotificationReceiver::acceptCommunities(std::vector<std::string> communities) {
    impl_->decoder.acceptCommunities(std::move(communities));
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
