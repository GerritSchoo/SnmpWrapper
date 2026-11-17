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
#include <condition_variable>
#include <cstdlib>
#include <sys/wait.h>
#include <mutex>
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

TEST_F(ReceiverTest, StopEndsRunAtOnce) {
    std::thread stopper([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        receiver->stop();
    });
    const auto start = std::chrono::steady_clock::now();
    receiver->run();
    stopper.join();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(600));  // woken up, not after 1 s
}

// --- runInBackground() ------------------------------------------------------------------------

/// Counts what the background thread delivers; the test thread waits for it.
struct Counter {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<Notification> got;
    void add(const Notification& n) {
        std::lock_guard<std::mutex> l(mu);
        got.push_back(n);
        cv.notify_all();
    }
    bool waitFor(std::size_t count) {
        std::unique_lock<std::mutex> l(mu);
        return cv.wait_for(l, std::chrono::seconds(3), [&] { return got.size() >= count; });
    }
};

TEST(ReceiverBackground, ReceivesWithoutPollOnItsOwnThread) {
    const int port = freeUdpPort();
    auto receiver = std::make_shared<NotificationReceiver>("udp:127.0.0.1:" + std::to_string(port));
    Counter counter;
    std::atomic<bool> otherThread{false};
    const auto testThread = std::this_thread::get_id();
    receiver->onNotification([&](const Notification& n) {
        otherThread = std::this_thread::get_id() != testThread;
        counter.add(n);
    });
    receiver->runInBackground();
    Sender(port, SNMP_VERSION_2c, "public").v2(kAlarm, 1, "pump");
    ASSERT_TRUE(counter.waitFor(1));
    EXPECT_TRUE(otherThread);
    EXPECT_EQ(counter.got[0].trapOid, kAlarm);
    receiver->stop();
    receiver->wait();
}

TEST(ReceiverBackground, AcknowledgesAnInform) {
    const int port = freeUdpPort();
    auto receiver = std::make_shared<NotificationReceiver>("udp:127.0.0.1:" + std::to_string(port));
    receiver->runInBackground();
    EXPECT_TRUE(Sender(port, SNMP_VERSION_2c, "public").v2(kAlarm, 1, "x", true));  // waits for the acknowledgement
}

TEST(ReceiverBackground, HandlersAndCommunitiesCanBeChangedWhileItRuns) {
    const int port = freeUdpPort();
    auto receiver = std::make_shared<NotificationReceiver>("udp:127.0.0.1:" + std::to_string(port));
    receiver->runInBackground();
    Counter counter;
    receiver->onNotification([&](const Notification& n) { counter.add(n); });  // added while the loop runs
    receiver->acceptCommunities({"monitor"});
    Sender(port, SNMP_VERSION_2c, "public").v2(kAlarm, 1, "dropped");
    Sender(port, SNMP_VERSION_2c, "monitor").v2(kAlarm, 2, "accepted");
    ASSERT_TRUE(counter.waitFor(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::lock_guard<std::mutex> l(counter.mu);
    ASSERT_EQ(counter.got.size(), 1u);
    EXPECT_EQ(counter.got[0].community, "monitor");
}

TEST(ReceiverBackground, AHandlerMayAddAHandler) {
    const int port = freeUdpPort();
    auto receiver = std::make_shared<NotificationReceiver>("udp:127.0.0.1:" + std::to_string(port));
    Counter counter;
    std::atomic<bool> added{false};
    receiver->onNotification([&](const Notification&) {
        if (!added.exchange(true)) receiver->onNotification([&](const Notification& n) { counter.add(n); });  // no deadlock
    });
    receiver->runInBackground();
    Sender(port, SNMP_VERSION_2c, "public").v2(kAlarm, 1, "first");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    Sender(port, SNMP_VERSION_2c, "public").v2(kAlarm, 2, "second");
    ASSERT_TRUE(counter.waitFor(1));  // the new handler got the next notification
}

TEST(ReceiverBackground, PollRunAndASecondLoopAreRefused) {
    auto receiver = std::make_shared<NotificationReceiver>("udp:127.0.0.1:" + std::to_string(freeUdpPort()));
    receiver->runInBackground();
    EXPECT_THROW(receiver->poll(std::chrono::milliseconds(1)), Error);
    EXPECT_THROW(receiver->run(), Error);
    EXPECT_THROW(receiver->runInBackground(), Error);
}

TEST(ReceiverBackground, StopAndWaitEndItAtOnceAndAStoppedReceiverDoesNotStartAgain) {
    auto receiver = std::make_shared<NotificationReceiver>("udp:127.0.0.1:" + std::to_string(freeUdpPort()));
    receiver->runInBackground();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto start = std::chrono::steady_clock::now();
    receiver->stop();
    receiver->wait();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(500));
    EXPECT_THROW(receiver->runInBackground(), Error);
}

TEST(ReceiverBackground, TheDestructorEndsTheLoop) {
    auto receiver = std::make_shared<NotificationReceiver>("udp:127.0.0.1:" + std::to_string(freeUdpPort()));
    receiver->runInBackground();
    receiver.reset();  // must neither hang nor crash
    SUCCEED();
}

// --- SNMPv3 -------------------------------------------------------------------------------------
// Sent by the Net-SNMP tools in a process of their own: a sender in this process would share Net-SNMP's user table
// with the receiver, and a wrong passphrase could not be told apart.

const std::string kAuth = "authpass123";
const std::string kPriv = "privpass123";
const std::string kSenderEngine = "0x8000000001020304";

/// Runs a Net-SNMP tool without any user configuration. @return Its exit code, or -1 if the tools are missing.
int tool(const std::string& args) {
    static const bool available = std::system("command -v snmptrap >/dev/null 2>&1 && command -v snmpinform >/dev/null 2>&1") == 0;
    if (!available) return -1;
    const std::string env = "SNMPCONFPATH=/nonexistent SNMP_PERSISTENT_DIR=/tmp/snmpwrap-receiver-v3-$$ ";
    const int rc = std::system((env + args + " >/dev/null 2>&1").c_str());
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
}

class ReceiverV3 : public ::testing::Test {
protected:
    void SetUp() override {
        if (tool("true") < 0) GTEST_SKIP() << "snmptrap / snmpinform not found";
        receiver = std::make_shared<NotificationReceiver>("udp:127.0.0.1:" + std::to_string(port));
        receiver->onNotification([this](const Notification& n) { counter.add(n); });
    }
    static NotificationUser user(const std::string& name, SecurityLevel level) {
        NotificationUser u;
        u.name = name;
        u.securityLevel = level;
        u.authPassphrase = kAuth;
        u.privPassphrase = kPriv;
        return u;
    }
    /// snmpinform -v3: waits for the acknowledgement. @return True if acknowledged.
    bool inform(const std::string& security) {
        return tool("snmpinform -v3 " + security + " -t 1 -r 0 127.0.0.1:" + std::to_string(port) +
                    " '' 1.3.6.1.4.1.99999.3.0.1 1.3.6.1.4.1.99999.1.1.0 s pump") == 0;
    }
    /// snmptrap -v3 with the sender's engine ID.
    void trap(const std::string& security) {
        tool("snmptrap -v3 -e " + kSenderEngine + " " + security + " 127.0.0.1:" + std::to_string(port) +
             " '' 1.3.6.1.4.1.99999.3.0.1 1.3.6.1.4.1.99999.1.1.0 s pump");
    }
    /// Nothing arrives within half a second.
    bool nothing() {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::lock_guard<std::mutex> l(counter.mu);
        return counter.got.empty();
    }

    int port = freeUdpPort();
    std::shared_ptr<NotificationReceiver> receiver;
    Counter counter;
};

TEST_F(ReceiverV3, AnInformIsAcknowledgedAndCarriesUserAndLevel) {
    receiver->addUser(user("ops", SecurityLevel::AuthNoPriv));
    receiver->runInBackground();
    EXPECT_TRUE(inform("-u ops -l authNoPriv -a SHA -A " + kAuth));
    ASSERT_TRUE(counter.waitFor(1));
    const Notification& n = counter.got[0];
    EXPECT_EQ(n.version, 3);
    EXPECT_TRUE(n.inform);
    EXPECT_EQ(n.user, "ops");
    EXPECT_EQ(n.securityLevel, SecurityLevel::AuthNoPriv);
    EXPECT_EQ(n.trapOid, kAlarm);
    ASSERT_EQ(n.vars.size(), 1u);
    EXPECT_EQ(n.vars[0].value, Value::string("pump"));
}

TEST_F(ReceiverV3, ATrapNeedsTheEngineIdOfTheSender) {
    NotificationUser u = user("trapper", SecurityLevel::AuthNoPriv);
    u.engineId = kSenderEngine;
    receiver->addUser(u);
    receiver->runInBackground();
    trap("-u trapper -l authNoPriv -a SHA -A " + kAuth);
    ASSERT_TRUE(counter.waitFor(1));
    EXPECT_EQ(counter.got[0].user, "trapper");
    EXPECT_FALSE(counter.got[0].inform);
}

TEST_F(ReceiverV3, AWrongPassphraseIsNotHeard) {
    receiver->addUser(user("ops", SecurityLevel::AuthNoPriv));
    receiver->runInBackground();
    EXPECT_FALSE(inform("-u ops -l authNoPriv -a SHA -A wrongpass99"));
    EXPECT_TRUE(nothing());
}

TEST_F(ReceiverV3, ATrapWithAWrongPassphraseIsNotHeard) {
    NotificationUser u = user("trapper", SecurityLevel::AuthNoPriv);
    u.engineId = kSenderEngine;
    receiver->addUser(u);
    receiver->runInBackground();
    trap("-u trapper -l authNoPriv -a SHA -A wrongpass99");
    EXPECT_TRUE(nothing());
}

TEST_F(ReceiverV3, AnUnknownUserIsNotHeard) {
    receiver->addUser(user("ops", SecurityLevel::AuthNoPriv));
    receiver->runInBackground();
    EXPECT_FALSE(inform("-u stranger -l authNoPriv -a SHA -A " + kAuth));
    EXPECT_TRUE(nothing());
}

TEST_F(ReceiverV3, ALevelBelowTheUsersIsNotHeard) {  // the user's name alone, without its keys
    receiver->addUser(user("ops", SecurityLevel::AuthNoPriv));
    receiver->runInBackground();
    EXPECT_FALSE(inform("-u ops -l noAuthNoPriv"));
    EXPECT_TRUE(nothing());
}

TEST_F(ReceiverV3, AcceptOnlyV3DropsV2c) {
    receiver->addUser(user("ops", SecurityLevel::AuthNoPriv));
    receiver->acceptOnlyV3();
    receiver->runInBackground();
    Sender(port, SNMP_VERSION_2c, "public").v2(kAlarm, 1, "dropped");
    EXPECT_TRUE(inform("-u ops -l authNoPriv -a SHA -A " + kAuth));
    ASSERT_TRUE(counter.waitFor(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::lock_guard<std::mutex> l(counter.mu);
    ASSERT_EQ(counter.got.size(), 1u);
    EXPECT_EQ(counter.got[0].version, 3);
}

TEST_F(ReceiverV3, AuthPrivWithAes) {
    // like the integration suite: AES needs a Net-SNMP built with OpenSSL; the internal crypto here cannot do it
    if (std::system("net-snmp-config --configure-options 2>/dev/null | grep -q -e 'openssl=internal' -e 'without-openssl'") == 0)
        GTEST_SKIP() << "this Net-SNMP is built without OpenSSL: no AES";
    receiver->addUser(user("secret", SecurityLevel::AuthPriv));
    receiver->runInBackground();
    EXPECT_TRUE(inform("-u secret -l authPriv -a SHA -A " + kAuth + " -x AES -X " + kPriv));
    ASSERT_TRUE(counter.waitFor(1));
    EXPECT_EQ(counter.got[0].securityLevel, SecurityLevel::AuthPriv);
}

TEST_F(ReceiverV3, UsersAreAddedBeforeTheBackgroundLoopStarts) {
    receiver->runInBackground();
    EXPECT_THROW(receiver->addUser(user("late", SecurityLevel::AuthNoPriv)), Error);
}

TEST_F(ReceiverTest, AnAddressInUseIsATransportError) {
    EXPECT_THROW(NotificationReceiver("udp:127.0.0.1:" + std::to_string(port)), TransportError);
}

}  // namespace
