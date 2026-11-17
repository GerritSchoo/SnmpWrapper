#include "snmpwrap/Agent.hpp"

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

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

// Read end of the wake-up pipe became readable: empty it (the wake-up itself is the point).
void drainWakeup(int fd, void*) {
    char buf[64];
    while (::read(fd, buf, sizeof buf) > 0) {
    }
}

}  // namespace

struct Agent::Impl {
    AgentConfig config;
    std::atomic<bool> running{true};
    bool started = false;
    bool agentInitialized = false;
    std::vector<std::unique_ptr<Registration>> registrations;
    std::vector<std::shared_ptr<Mib>> mibs;

    // --- background loop (runInBackground) ------------------------------------------------------
    std::thread loopThread;
    std::atomic<bool> background{false};          ///< runInBackground() was called (stays true after the end)
    std::atomic<std::thread::id> loopId{};        ///< the background thread
    std::mutex queueMu;                           ///< guards tasks, loopAlive, loopDone
    std::condition_variable doneCv;               ///< signals loopDone
    std::deque<std::function<void()>> tasks;      ///< work handed to the loop by other threads
    bool loopAlive = false;                       ///< the loop still takes tasks
    bool loopDone = false;                        ///< the loop thread no longer touches Net-SNMP
    int wakeRead = -1;
    std::atomic<int> wakeWrite{-1};               ///< written by wake() - also from stop(), i.e. signal handlers

    bool onLoopThread() const noexcept { return std::this_thread::get_id() == loopId.load(); }

    /// Lets select() in the loop return now (async-signal-safe: an atomic load and write()).
    void wake() noexcept {
        const int fd = wakeWrite.load();
        if (fd >= 0) {
            const char c = 1;
            [[maybe_unused]] const auto n = ::write(fd, &c, 1);  // a full pipe means a wake-up is pending already
        }
    }

    /// Hands @p task to the loop. @return False if the loop takes no more tasks (the caller then does the work).
    bool post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> l(queueMu);
            if (!loopAlive) return false;
            tasks.push_back(std::move(task));
        }
        wake();
        return true;
    }

    /// After a failed post(): waits until the ending loop no longer uses Net-SNMP, so the caller may.
    void waitUntilLoopDone() {
        std::unique_lock<std::mutex> l(queueMu);
        doneCv.wait(l, [this] { return loopDone; });
    }

    /// Runs the queued tasks on the loop thread. Tasks report their own errors; nothing escapes from here.
    void runTasks() noexcept {
        std::deque<std::function<void()>> mine;
        {
            std::lock_guard<std::mutex> l(queueMu);
            mine.swap(tasks);
        }
        for (auto& task : mine) {
            try {
                task();
            } catch (const std::exception& e) {
                snmp_log(LOG_ERR, "snmpwrap: background task failed: %s\n", e.what());
            } catch (...) {
                snmp_log(LOG_ERR, "snmpwrap: background task failed\n");
            }
        }
    }

    /// Runs @p f on the loop thread when the Agent runs in the background and the caller is another thread; waits for
    /// the result and rethrows its exception. Otherwise (no background loop, or on it already) calls @p f directly.
    template <class F>
    auto onLoop(F&& f) -> decltype(f()) {
        if (!background.load() || onLoopThread()) return f();
        using Result = decltype(f());
        std::packaged_task<Result()> task(std::forward<F>(f));
        std::future<Result> result = task.get_future();
        if (!post([&task] { task(); })) {  // the loop is ending: do it here once it no longer uses Net-SNMP
            waitUntilLoopDone();
            task();
        }
        return result.get();
    }

    void backgroundLoop() noexcept {
        { std::lock_guard<std::mutex> l(queueMu); }  // runInBackground() has set loopId
        try {
            while (running) {
                runTasks();
                flushRemovals();
                agent_check_and_process(1);  // returns on a request, a wake-up or the 1 s heartbeat
            }
        } catch (const std::exception& e) {
            snmp_log(LOG_ERR, "snmpwrap: background loop ended: %s\n", e.what());
        } catch (...) {
            snmp_log(LOG_ERR, "snmpwrap: background loop ended\n");
        }
        {
            std::lock_guard<std::mutex> l(queueMu);
            loopAlive = false;  // from now on callers do their work themselves
        }
        runTasks();  // notifications queued before stop() and callers still waiting
        unregister_readfd(wakeRead);
        {
            std::lock_guard<std::mutex> l(queueMu);
            loopDone = true;
        }
        doneCv.notify_all();
    }

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
    stop();
    if (impl_->loopThread.joinable()) impl_->loopThread.join();
    for (int fd : {impl_->wakeRead, impl_->wakeWrite.exchange(-1)})
        if (fd >= 0) ::close(fd);
    // Also shuts down the library for Clients of this process (net-snmp state is process-wide).
    if (impl_->agentInitialized) detail::shutdownLibrary(impl_->config.name.c_str());
    SOCK_CLEANUP;
    g_agentExists = false;
}

void Agent::addHandler(const Oid& root, std::shared_ptr<IHandler> handler) {
    if (!handler) throw Error("null handler");
    if (root.empty()) throw Error("empty root OID");
    impl_->onLoop([&] { addHandlerOnLoop(root, std::move(handler)); });
}

void Agent::addHandlerOnLoop(const Oid& root, std::shared_ptr<IHandler> handler) {
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
    impl_->onLoop([&] {
        auto& regs = impl_->registrations;
        auto it = std::find_if(regs.begin(), regs.end(), [&](const auto& r) { return !r->removed() && r->root() == root; });
        if (it == regs.end()) throw Error("no handler registered at " + root.str());
        (*it)->markRemoved();
        impl_->flushRemovals();
    });
}

void Agent::removeMib(const Mib& mib) {
    impl_->onLoop([&] {
        auto& regs = impl_->registrations;
        auto it = std::find_if(regs.begin(), regs.end(), [&](const auto& r) { return !r->removed() && r->handler().get() == &mib; });
        if (it == regs.end()) throw Error("Mib at " + mib.root().str() + " was not created by this Agent (or is removed already)");
        (*it)->markRemoved();
        impl_->flushRemovals();
    });
}

std::shared_ptr<Mib> Agent::addMib(const Oid& root) {
    auto mib = std::make_shared<Mib>(root);
    impl_->onLoop([&] {
        addHandlerOnLoop(root, mib);
        impl_->mibs.push_back(mib);
    });
    return mib;
}

void Agent::start() {
    if (impl_->started) return;
    detail::initLibrary(impl_->config.name.c_str());  // fires the AgentX connect (POST_READ_CONFIG)
    snmp_alarm_register(1, SA_REPEAT, heartbeat, nullptr);
    impl_->started = true;
}

bool Agent::poll(bool block) {
    if (impl_->background) throw Error("the Agent runs in the background (runInBackground): do not call poll() as well");
    start();
    if (!impl_->running) return false;
    impl_->flushRemovals();
    // blocking waits at most until the next alarm: the 1 s heartbeat (or an earlier net-snmp timer)
    agent_check_and_process(block ? 1 : 0);
    return impl_->running;
}

void Agent::run() {
    if (impl_->background) throw Error("the Agent runs in the background (runInBackground): do not call run() as well");
    start();
    while (impl_->running) {
        impl_->flushRemovals();
        agent_check_and_process(1);
    }
}

void Agent::runInBackground() {
    Impl& im = *impl_;
    if (im.background) throw Error("the Agent runs in the background already");
    if (!im.running) throw Error("the Agent was stopped; a stopped Agent does not start again");
    start();

    int fds[2];
    if (::pipe(fds) != 0) throw Error("runInBackground: cannot create the wake-up pipe");
    for (int fd : fds) {
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    if (register_readfd(fds[0], drainWakeup, nullptr) != FD_REGISTERED_OK) {
        ::close(fds[0]);
        ::close(fds[1]);
        throw Error("runInBackground: Net-SNMP refused the wake-up pipe");
    }
    im.wakeRead = fds[0];
    im.wakeWrite = fds[1];

    std::lock_guard<std::mutex> l(im.queueMu);  // the thread waits for it before its first step
    im.loopAlive = true;
    try {
        im.loopThread = std::thread([&im] { im.backgroundLoop(); });
    } catch (...) {
        im.loopAlive = false;
        unregister_readfd(fds[0]);
        throw;
    }
    im.loopId = im.loopThread.get_id();
    im.background = true;
}

void Agent::stop() noexcept {
    impl_->running = false;
    impl_->wake();  // the background loop notices it at once, not only at the next heartbeat
}

void Agent::wait() {
    if (!impl_->loopThread.joinable()) return;
    if (impl_->onLoopThread()) throw Error("Agent::wait() called from the background thread itself");
    impl_->loopThread.join();
}

void Agent::sendTrap(const Oid& trapOid, const std::vector<VarBind>& vars) {
    const bool foreignThread = impl_->background && !impl_->onLoopThread();
    if (!foreignThread) start();  // in the background it is started already (and start() belongs to the loop thread)
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
    // The list is plain memory, built above in the calling thread (encoding errors are thrown there); sending it
    // uses Net-SNMP's sessions and belongs to the loop thread.
    auto send = [list] {
        send_v2trap(list);
        snmp_free_varbind(list);
    };
    if (foreignThread && impl_->post(send)) return;  // queued, the loop sends it right away
    if (foreignThread) impl_->waitUntilLoopDone();   // the loop is ending: send it here once it is done
    send();
}

}  // namespace snmpwrap
