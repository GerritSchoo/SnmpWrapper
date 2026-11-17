/**
 * @file NotificationReceiver.hpp
 * @brief Receiving notifications (traps and informs) sent by agents - the manager side of
 *        Agent::sendTrap(). MIB-independent; the code generator adds typed handlers on top.
 */
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "snmpwrap/INotificationSender.hpp"
#include "snmpwrap/INotificationSource.hpp"

namespace snmpwrap {

/**
 * @brief Listens for SNMPv1 / SNMPv2c traps and informs on a UDP (or TCP) address.
 *
 * Point snmpd's trap destinations at it (`trap2sink`, `trapsink`, `informsink` in snmpd.conf).
 * Informs are acknowledged automatically. SNMPv3 notifications are not supported.
 *
 * @code
 * NotificationReceiver receiver("udp:0.0.0.0:1162");     // port 162 needs root
 * receiver.onNotification([](const Notification& n) { std::cout << n.trapOid.str() << "\n"; });
 * receiver.acceptCommunities({"public"});                 // drop everything else
 * while (running) receiver.poll();                         // or receiver.run() until stop()
 * @endcode
 *
 * The handlers are callbacks: they run on the thread that calls poll() / run() - or, after runInBackground(), on the
 * receiver's own thread. The generated `Notifications` class registers itself as such a handler and calls the typed
 * on<Notification>() hooks.
 *
 * @warning Without acceptCommunities() every v1 / v2c notification reaches the handlers (and every inform is
 *          acknowledged), whoever sends it: anyone who can send UDP packets to the address can trigger them.
 * @note Like Client: use it from one thread - except onNotification(), acceptCommunities(), stop() and wait(), which
 *       may be called from any thread. With an Agent in the same process, create the Agent first.
 */
class NotificationReceiver : public INotificationSource {
public:
    /**
     * @brief Opens the listening socket.
     * @param[in] address "udp:<ip>:<port>" (default port 162), "tcp:<ip>:<port>", ...
     * @throws TransportError if the address cannot be opened (in use, no permission for ports < 1024).
     */
    explicit NotificationReceiver(const std::string& address = "udp:0.0.0.0:162");
    /// @brief Ends a background loop (stop() + wait()) and closes the socket.
    ~NotificationReceiver();
    NotificationReceiver(const NotificationReceiver&) = delete;
    NotificationReceiver& operator=(const NotificationReceiver&) = delete;

    /// @brief Adds a handler; all handlers get every notification. Exceptions from handlers are ignored.
    /// @param[in] handler The handler. Thread-safe; a handler may add further handlers (they get the next notification).
    void onNotification(std::function<void(const Notification&)> handler) override;

    /**
     * @brief Accepts only notifications with one of these community strings.
     * @param[in] communities The accepted communities; empty = accept every notification (the default).
     * @note Other notifications are dropped before any handler runs, and informs among them are not acknowledged.
     *       Communities travel in clear text: this keeps out strangers, not attackers who can read the traffic.
     *       Thread-safe: applies from the next notification on.
     */
    void acceptCommunities(std::vector<std::string> communities);

    /**
     * @brief Waits for and processes incoming messages.
     * @param[in] timeout Maximum time to wait.
     * @return True if a message was processed.
     */
    bool poll(std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));

    /// @brief Processes messages until stop() is called.
    void run();

    /**
     * @brief Receives on a thread of the receiver's own and returns at once: the handlers run on that thread.
     * @code
     * auto receiver = std::make_shared<snmpwrap::NotificationReceiver>("udp:0.0.0.0:1162");
     * receiver->acceptCommunities({"public"});
     * mib::Notifications notifications(receiver);
     * notifications.onSomeTrap([](const mib::SomeTrap& t) { ... });   // runs on the receiver's thread
     * receiver->runInBackground();
     * ...
     * receiver->stop();
     * receiver->wait();                                              // no handler runs after this
     * @endcode
     * poll() and run() must not be used as well. What the handlers touch must be thread-safe.
     * @throws Error if it runs in the background already or was stopped (a stopped receiver does not start again).
     */
    void runInBackground();

    /// @brief Makes run() / the background loop return at once. Thread- and signal-safe.
    void stop() noexcept;

    /// @brief Waits until the background loop has ended (call stop() first); returns at once without one.
    /// @throws Error if called from the receiver's own thread (inside a handler).
    void wait();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace snmpwrap
