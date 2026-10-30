// Client (client.hpp): the own (source) address of requests, SessionConfig::localAddress.
// No snmpd: a plain UDP socket plays the device. It never answers (the request times out); the test looks at the
// address the request came from.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "snmpwrap/client.hpp"
#include "support/test_support.hpp"

using namespace snmpwrap;

namespace {

/// A bound UDP socket on 127.0.0.1 (or ::1): the "device", or something that occupies a port.
class UdpSocket {
public:
    explicit UdpSocket(bool v6 = false) : v6_(v6), fd_(::socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0)) {
        sockaddr_storage a{};
        socklen_t len = 0;
        if (v6) {
            auto& s6 = reinterpret_cast<sockaddr_in6&>(a);
            s6.sin6_family = AF_INET6;
            s6.sin6_addr = in6addr_loopback;
            len = sizeof s6;
        } else {
            auto& s4 = reinterpret_cast<sockaddr_in&>(a);
            s4.sin_family = AF_INET;
            s4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            len = sizeof s4;
        }
        ok_ = fd_ >= 0 && ::bind(fd_, reinterpret_cast<sockaddr*>(&a), len) == 0 &&
              ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len) == 0;
        port_ = ntohs(v6 ? reinterpret_cast<sockaddr_in6&>(a).sin6_port : reinterpret_cast<sockaddr_in&>(a).sin_port);
        timeval tv{1, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    ~UdpSocket() {
        if (fd_ >= 0) ::close(fd_);
    }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool ok() const { return ok_; }
    int port() const { return port_; }
    std::string address() const { return v6_ ? "[::1]:" + std::to_string(port_) : "127.0.0.1:" + std::to_string(port_); }

    /// Port a datagram was sent from, or -1 if none arrived within a second.
    int receiveFromPort() {
        char buf[1500];
        sockaddr_storage from{};
        socklen_t len = sizeof from;
        if (::recvfrom(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len) < 0) return -1;
        return ntohs(from.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6&>(from).sin6_port
                                                : reinterpret_cast<sockaddr_in&>(from).sin_port);
    }

private:
    bool v6_;
    int fd_;
    bool ok_ = false;
    int port_ = 0;
};

/// A port that was free a moment ago.
int freePort() {
    UdpSocket s;
    return s.port();
}

SessionConfig towards(const UdpSocket& device, const std::string& localAddress = "") {
    SessionConfig cfg;
    cfg.peer = device.address();
    cfg.localAddress = localAddress;
    cfg.timeout = std::chrono::milliseconds(100);
    cfg.retries = 0;
    return cfg;
}

/// Sends one GET (it times out: the device never answers).
void sendOneGet(Client& client) { EXPECT_THROW(client.get(Oid::parse("1.3.6.1.2.1.1.3.0")), TransportError); }

TEST(ClientLocalAddress, FixedIpAndPort) {
    UdpSocket device;
    ASSERT_TRUE(device.ok());
    const int port = freePort();
    Client client(towards(device, "127.0.0.1:" + std::to_string(port)));
    sendOneGet(client);
    EXPECT_EQ(device.receiveFromPort(), port);
}

TEST(ClientLocalAddress, FixedIpWithAnyPort) {
    UdpSocket device;
    ASSERT_TRUE(device.ok());
    Client client(towards(device, "127.0.0.1"));
    sendOneGet(client);
    EXPECT_GT(device.receiveFromPort(), 0);  // arrived, from some free port
}

TEST(ClientLocalAddress, SettingIsOnlyForThatClient) {
    UdpSocket device;
    ASSERT_TRUE(device.ok());
    const int port = freePort();
    Client fixed(towards(device, "127.0.0.1:" + std::to_string(port)));  // stays open: holds the port
    Client other(towards(device));                                         // no localAddress
    sendOneGet(other);
    const int from = device.receiveFromPort();
    EXPECT_GT(from, 0);
    EXPECT_NE(from, port);
}

TEST(ClientLocalAddress, Ipv6) {
    UdpSocket device(true);
    if (!device.ok()) GTEST_SKIP() << "no IPv6 loopback on this host";
    int port = 0;
    {
        UdpSocket probe(true);
        port = probe.port();
    }
    Client client(towards(device, "[::1]:" + std::to_string(port)));
    sendOneGet(client);
    EXPECT_EQ(device.receiveFromPort(), port);
}

TEST(ClientLocalAddress, AddressOfAnotherHostIsATransportError) {
    UdpSocket device;
    ASSERT_TRUE(device.ok());
    try {
        Client client(towards(device, "192.0.2.1"));  // TEST-NET-1: never an address of this host
        FAIL() << "no error for a foreign source address";
    } catch (const TransportError& e) {
        EXPECT_THAT(e.what(), ::testing::HasSubstr("localAddress 192.0.2.1"));
    }
}

TEST(ClientLocalAddress, PortInUseIsATransportError) {
    UdpSocket device, occupied;
    ASSERT_TRUE(device.ok());
    ASSERT_TRUE(occupied.ok());
    EXPECT_THROW(Client(towards(device, "127.0.0.1:" + std::to_string(occupied.port()))), TransportError);
}

TEST(ClientLocalAddress, TcpPeerIsRefused) {  // Net-SNMP has no source address for TCP: refused, not ignored
    SessionConfig cfg;
    cfg.peer = "tcp:127.0.0.1:1";
    cfg.localAddress = "127.0.0.1";
    try {
        Client client(cfg);
        FAIL() << "localAddress with a TCP peer accepted";
    } catch (const TransportError&) {
        FAIL() << "expected the configuration error, not a transport error";
    } catch (const Error& e) {
        EXPECT_THAT(e.what(), ::testing::HasSubstr("UDP peers only"));
    }
}

}  // namespace
