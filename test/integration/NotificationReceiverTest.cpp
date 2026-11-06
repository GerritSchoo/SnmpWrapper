// Integration: the real NotificationReceiver (NotificationReceiver.hpp / NotificationReceiver.cpp) on a UDP port of localhost,
// with SNMPv2c traps, SNMPv1 traps and informs that the test sends with Net-SNMP. No snmpd.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "snmpwrap/NotificationReceiver.hpp"
#include "TestSupport.hpp"

using namespace snmpwrap;
using ::testing::HasSubstr;

namespace {

const Oid kAlarm{1, 3, 6, 1, 4, 1, 99999, 3, 0, 1};
const Oid kName{1, 3, 6, 1, 4, 1, 99999, 1, 1, 0};

int freeUdpPort() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), len);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    ::close(fd);
    return ntohs(a.sin_port);
}

/// Sends notifications to 127.0.0.1:<port> through a Net-SNMP session of its own.
class Sender {
public:
    Sender(int port, long version, const std::string& community) : community_(community) {
        netsnmp_session s;
        snmp_sess_init(&s);
        peer_ = "udp:127.0.0.1:" + std::to_string(port);
        s.peername = const_cast<char*>(peer_.c_str());
        s.version = version;
        s.community = reinterpret_cast<u_char*>(const_cast<char*>(community_.c_str()));
        s.community_len = community_.size();
        s.timeout = 2000000;
        s.retries = 0;
        handle_ = snmp_sess_open(&s);
        if (!handle_) throw std::runtime_error("cannot open the sending session");
    }
    ~Sender() { snmp_sess_close(handle_); }
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    /// SNMPv2c trap (or inform, which waits for the acknowledgement). @return True if sent (inform: acknowledged).
    bool v2(const Oid& trapOid, std::uint32_t uptime, const std::string& name, bool inform = false) {
        netsnmp_pdu* pdu = snmp_pdu_create(inform ? SNMP_MSG_INFORM : SNMP_MSG_TRAP2);
        add(pdu, Oid({1, 3, 6, 1, 2, 1, 1, 3, 0}), ASN_TIMETICKS, &uptime, sizeof uptime);
        const auto trap = net(trapOid);
        add(pdu, Oid({1, 3, 6, 1, 6, 3, 1, 1, 4, 1, 0}), ASN_OBJECT_ID, trap.data(), trap.size() * sizeof(::oid));
        add(pdu, kName, ASN_OCTET_STR, name.data(), name.size());
        if (!inform) {
            if (snmp_sess_send(handle_, pdu)) return true;
            snmp_free_pdu(pdu);
            return false;
        }
        netsnmp_pdu* response = nullptr;
        const int status = snmp_sess_synch_response(handle_, pdu, &response);
        if (response) snmp_free_pdu(response);
        return status == STAT_SUCCESS;
    }

    /// SNMPv1 trap.
    bool v1(const Oid& enterprise, long generic, long specific, long time) {
        netsnmp_pdu* pdu = snmp_pdu_create(SNMP_MSG_TRAP);
        const auto ent = net(enterprise);
        pdu->enterprise = static_cast<::oid*>(malloc(ent.size() * sizeof(::oid)));
        std::copy(ent.begin(), ent.end(), pdu->enterprise);
        pdu->enterprise_length = ent.size();
        pdu->trap_type = generic;
        pdu->specific_type = specific;
        pdu->time = time;
        if (snmp_sess_send(handle_, pdu)) return true;
        snmp_free_pdu(pdu);
        return false;
    }

private:
    static std::vector<::oid> net(const Oid& o) { return std::vector<::oid>(o.ids().begin(), o.ids().end()); }
    static void add(netsnmp_pdu* pdu, const Oid& name, u_char type, const void* value, std::size_t len) {
        const auto n = net(name);
        snmp_pdu_add_variable(pdu, n.data(), n.size(), type, value, len);
    }

    std::string community_;
    std::string peer_;
    void* handle_ = nullptr;
};

/// A receiver on a free port of localhost that remembers what it received.
class ReceiverTest : public ::testing::Test {
protected:
    ReceiverTest() {
        receiver = std::make_shared<NotificationReceiver>("udp:127.0.0.1:" + std::to_string(port));
        receiver->onNotification([this](const Notification& n) { received.push_back(n); });
    }
    /// Polls until @p count notifications arrived or 2 s passed.
    void waitFor(std::size_t count) {
        for (int i = 0; i < 40 && received.size() < count; ++i) receiver->poll(std::chrono::milliseconds(50));
    }

    int port = freeUdpPort();
    std::shared_ptr<NotificationReceiver> receiver;
    std::vector<Notification> received;
};

TEST_F(ReceiverTest, ReceivesAV2cTrap) {
    Sender(port, SNMP_VERSION_2c, "public").v2(kAlarm, 1234, "pump");
    waitFor(1);
    ASSERT_EQ(received.size(), 1u);
    const Notification& n = received.front();
    EXPECT_EQ(n.version, 2);
    EXPECT_FALSE(n.inform);
    EXPECT_EQ(n.trapOid, kAlarm);
    EXPECT_EQ(n.uptime, 1234u);
    EXPECT_EQ(n.community, "public");
    EXPECT_THAT(n.source, HasSubstr("127.0.0.1"));
    ASSERT_EQ(n.vars.size(), 1u);
    EXPECT_EQ(n.vars[0].value, Value::string("pump"));
}

TEST_F(ReceiverTest, ReceivesAV1TrapAsRfc3584Oid) {
    Sender(port, SNMP_VERSION_1, "public").v1(Oid({1, 3, 6, 1, 4, 1, 99999}), SNMP_TRAP_COLDSTART, 0, 77);
    waitFor(1);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].version, 1);
    EXPECT_EQ(received[0].trapOid, Oid({1, 3, 6, 1, 6, 3, 1, 1, 5, 1}));
    EXPECT_EQ(received[0].uptime, 77u);
}

TEST_F(ReceiverTest, AcknowledgesAnInform) {
    auto acknowledged = std::async(std::launch::async, [this] { return Sender(port, SNMP_VERSION_2c, "public").v2(kAlarm, 1, "x", true); });
    while (acknowledged.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        receiver->poll(std::chrono::milliseconds(50));
    EXPECT_TRUE(acknowledged.get());
    ASSERT_GE(received.size(), 1u);
    EXPECT_TRUE(received[0].inform);
}

TEST_F(ReceiverTest, OtherCommunitiesAreDropped) {
    receiver->acceptCommunities({"monitor"});
    Sender(port, SNMP_VERSION_2c, "public").v2(kAlarm, 1, "dropped");
    Sender(port, SNMP_VERSION_2c, "monitor").v2(kAlarm, 2, "accepted");
    waitFor(2);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].community, "monitor");
}

TEST_F(ReceiverTest, EveryHandlerIsCalledEvenAfterOneThrows) {
    int second = 0;
    receiver->onNotification([](const Notification&) { throw std::runtime_error("handler"); });
    receiver->onNotification([&](const Notification&) { ++second; });
    Sender(port, SNMP_VERSION_2c, "public").v2(kAlarm, 1, "x");
    waitFor(1);
    EXPECT_EQ(received.size(), 1u);
    EXPECT_EQ(second, 1);
}

TEST_F(ReceiverTest, PollWithoutTrafficReturnsFalse) { EXPECT_FALSE(receiver->poll(std::chrono::milliseconds(20))); }

TEST_F(ReceiverTest, PollWithTrafficReturnsTrue) {
    Sender(port, SNMP_VERSION_2c, "public").v2(kAlarm, 1, "x");
    bool any = false;
    for (int i = 0; i < 40 && !any; ++i) any = receiver->poll(std::chrono::milliseconds(50));
    EXPECT_TRUE(any);
}

TEST_F(ReceiverTest, RunEndsAfterStop) {
    std::thread stopper([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        receiver->stop();
    });
    const auto start = std::chrono::steady_clock::now();
    receiver->run();  // returns within about a second after stop()
    stopper.join();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
}

TEST_F(ReceiverTest, AnAddressInUseIsATransportError) {
    EXPECT_THROW(NotificationReceiver("udp:127.0.0.1:" + std::to_string(port)), TransportError);
}

}  // namespace
