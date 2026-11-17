#include "snmpwrap/NotificationReceiver.hpp"

#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "Convert.hpp"
#include "NotificationDetail.hpp"
#include "snmpwrap/Error.hpp"

namespace snmpwrap {


struct NotificationReceiver::Impl {
    void* handle = nullptr;  // from snmp_sess_add()
    using Handler = std::function<void(const Notification&)>;
    std::mutex mu;  ///< guards handlers and decoder: onNotification() / acceptCommunities() come from any thread
    std::shared_ptr<const std::vector<Handler>> handlers = std::make_shared<std::vector<Handler>>();
    detail::NotificationDecoder decoder;
    std::atomic<bool> running{true};
    int wakeRead = -1, wakeWrite = -1;  ///< stop() writes, select() in poll() wakes up
    std::thread loopThread;
    std::atomic<std::thread::id> loopId{};
    std::atomic<bool> background{false};

    ~Impl() {
        if (handle) snmp_sess_close(handle);
        for (int fd : {wakeRead, wakeWrite})
            if (fd >= 0) ::close(fd);
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
        std::optional<Notification> received;
        std::shared_ptr<const std::vector<Handler>> current;
        {
            std::lock_guard<std::mutex> l(mu);
            received = decoder.decode(*pdu, std::move(source));
            current = handlers;  // a snapshot: handlers may add handlers without a deadlock
        }
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

        for (const auto& h : *current) {
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

    int fds[2];
    if (::pipe(fds) != 0) throw Error("NotificationReceiver: cannot create the wake-up pipe");
    for (int fd : fds) {
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    impl_->wakeRead = fds[0];
    impl_->wakeWrite = fds[1];
}

NotificationReceiver::~NotificationReceiver() {
    stop();
    if (impl_->loopThread.joinable()) impl_->loopThread.join();
}

void NotificationReceiver::onNotification(std::function<void(const Notification&)> handler) {
    if (!handler) return;
    std::lock_guard<std::mutex> l(impl_->mu);
    auto next = std::make_shared<std::vector<Impl::Handler>>(*impl_->handlers);
    next->push_back(std::move(handler));
    impl_->handlers = std::move(next);
}

void NotificationReceiver::acceptCommunities(std::vector<std::string> communities) {
    std::lock_guard<std::mutex> l(impl_->mu);
    impl_->decoder.acceptCommunities(std::move(communities));
}

bool NotificationReceiver::poll(std::chrono::milliseconds timeout) {
    if (impl_->background && std::this_thread::get_id() != impl_->loopId.load())
        throw Error("the NotificationReceiver runs in the background (runInBackground): do not call poll() as well");
    int numfds = 0;
    fd_set fds;
    FD_ZERO(&fds);
    struct timeval tv {};
    int block = 1;
    snmp_sess_select_info(impl_->handle, &numfds, &fds, &tv, &block);
    FD_SET(impl_->wakeRead, &fds);
    numfds = std::max(numfds, impl_->wakeRead + 1);
    struct timeval wait {};
    wait.tv_sec = static_cast<long>(timeout.count() / 1000);
    wait.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    const int n = select(numfds, &fds, nullptr, nullptr, &wait);
    if (n > 0 && FD_ISSET(impl_->wakeRead, &fds)) {  // stop(): empty the pipe, report no message
        char buf[64];
        while (::read(impl_->wakeRead, buf, sizeof buf) > 0) {
        }
        FD_CLR(impl_->wakeRead, &fds);
        if (n == 1) return false;
    }
    if (n > 0) {
        snmp_sess_read(impl_->handle, &fds);
        return true;
    }
    if (n == 0) snmp_sess_timeout(impl_->handle);
    return false;
}

void NotificationReceiver::run() {
    if (impl_->background) throw Error("the NotificationReceiver runs in the background (runInBackground): do not call run() as well");
    while (impl_->running) poll(std::chrono::milliseconds(1000));
}

void NotificationReceiver::runInBackground() {
    Impl& im = *impl_;
    if (im.background) throw Error("the NotificationReceiver runs in the background already");
    if (!im.running) throw Error("the NotificationReceiver was stopped; a stopped receiver does not start again");
    im.background = true;  // before the thread: its first poll() must see it
    try {
        im.loopThread = std::thread([this, &im] {
            im.loopId = std::this_thread::get_id();
            while (im.running) {
                try {
                    poll(std::chrono::milliseconds(1000));
                } catch (...) {  // nothing may end the thread except stop()
                }
            }
        });
    } catch (...) {
        im.background = false;
        throw;
    }
}

void NotificationReceiver::stop() noexcept {
    impl_->running = false;
    const char c = 1;
    [[maybe_unused]] const auto n = ::write(impl_->wakeWrite, &c, 1);  // async-signal-safe; full pipe = wake-up pending
}

void NotificationReceiver::wait() {
    if (!impl_->loopThread.joinable()) return;
    if (std::this_thread::get_id() == impl_->loopId.load())
        throw Error("NotificationReceiver::wait() called from the receiver's own thread (inside a handler)");
    impl_->loopThread.join();
}

}  // namespace snmpwrap
