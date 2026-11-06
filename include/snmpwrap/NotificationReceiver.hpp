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
 * @warning Without acceptCommunities() every v1 / v2c notification reaches the handlers (and every inform is
 *          acknowledged), whoever sends it: anyone who can send UDP packets to the address can trigger them.
 * @note Like Client: use it from one thread. With an Agent in the same process, create the Agent first.
 */
class NotificationReceiver : public INotificationSource {
public:
    /**
     * @brief Opens the listening socket.
     * @param[in] address "udp:<ip>:<port>" (default port 162), "tcp:<ip>:<port>", ...
     * @throws TransportError if the address cannot be opened (in use, no permission for ports < 1024).
     */
    explicit NotificationReceiver(const std::string& address = "udp:0.0.0.0:162");
    ~NotificationReceiver();
    NotificationReceiver(const NotificationReceiver&) = delete;
    NotificationReceiver& operator=(const NotificationReceiver&) = delete;

    /// @brief Adds a handler; all handlers get every notification. Exceptions from handlers are ignored.
    /// @param[in] handler The handler.
    void onNotification(std::function<void(const Notification&)> handler) override;

    /**
     * @brief Accepts only notifications with one of these community strings.
     * @param[in] communities The accepted communities; empty = accept every notification (the default).
     * @note Other notifications are dropped before any handler runs, and informs among them are not acknowledged.
     *       Communities travel in clear text: this keeps out strangers, not attackers who can read the traffic.
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
    /// @brief Makes run() return within about one second. Thread- and signal-safe.
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace snmpwrap
