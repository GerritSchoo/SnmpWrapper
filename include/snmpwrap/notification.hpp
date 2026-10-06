/**
 * @file notification.hpp
 * @brief Receiving notifications (traps and informs) sent by agents - the manager side of
 *        Agent::sendTrap(). MIB-independent; the code generator adds typed handlers on top.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "snmpwrap/oid.hpp"
#include "snmpwrap/value.hpp"

namespace snmpwrap {

/// @brief One received notification.
struct Notification {
    Oid trapOid;                ///< Which notification (snmpTrapOID.0; v1 traps are converted as in RFC 3584).
    std::uint32_t uptime = 0;   ///< sysUpTime.0 of the sender (hundredths of a second).
    std::vector<VarBind> vars;  ///< The varbinds (the OBJECTS of the notification), without sysUpTime.0 / snmpTrapOID.0.
    std::string source;         ///< Sender address, e.g. "UDP: [127.0.0.1]:48531->[127.0.0.1]:1162".
    std::string community;      ///< Community string of the message.
    int version = 2;            ///< 1 = SNMPv1 trap, 2 = SNMPv2c trap or inform.
    bool inform = false;        ///< True for an inform (acknowledged automatically).
};

/**
 * @brief Something that sends notifications - implemented by Agent; mock it in tests.
 * The generated `send<Name>()` functions take a NotificationSender&.
 */
class NotificationSender {
public:
    virtual ~NotificationSender() = default;
    /// @brief Sends a notification. @param[in] trapOid The NOTIFICATION-TYPE OID. @param[in] vars Its OBJECTS.
    virtual void sendTrap(const Oid& trapOid, const std::vector<VarBind>& vars) = 0;
};

/**
 * @brief Something that delivers received notifications - implemented by NotificationReceiver; mock it in tests. The generated `Notifications` class takes a NotificationSource&.
 */
class NotificationSource {
public:
    virtual ~NotificationSource() = default;
    /// @brief Adds a handler that gets every notification. @param[in] handler The handler.
    virtual void onNotification(std::function<void(const Notification&)> handler) = 0;
};

/**
 * @brief Listens for SNMPv1 / SNMPv2c traps and informs on a UDP (or TCP) address.
 *
 * Point snmpd's trap destinations at it (`trap2sink`, `trapsink`, `informsink` in snmpd.conf).
 * Informs are acknowledged automatically. SNMPv3 notifications are not supported.
 *
 * @code
 * NotificationReceiver receiver("udp:0.0.0.0:1162");     // port 162 needs root
 * receiver.onNotification([](const Notification& n) { std::cout << n.trapOid.str() << "\n"; });
 * while (running) receiver.poll();                         // or receiver.run() until stop()
 * @endcode
 *
 * @note Like Client: use it from one thread. With an Agent in the same process, create the Agent first.
 */
class NotificationReceiver : public NotificationSource {
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
