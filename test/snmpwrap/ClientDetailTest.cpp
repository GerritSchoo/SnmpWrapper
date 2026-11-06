// Unit tests of ClientDetail.hpp / ClientDetail.cpp (internal): SessionSetup (checking a SessionConfig and
// filling a Net-SNMP session), ClientAddrScope (the source address), ResponseReader and Walker - all without a
// socket: sessions and PDUs are Net-SNMP structures, the Walker gets a fake fetch function.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "ClientDetail.hpp"
#include "Convert.hpp"
#include "TestSupport.hpp"

using namespace snmpwrap;
using namespace snmpwrap::detail;
using ::testing::HasSubstr;

namespace {

using V = SessionConfig::Version;
using L = SessionConfig::SecurityLevel;
using A = SessionConfig::AuthProtocol;
using P = SessionConfig::PrivProtocol;

Oid protoOid(const ::oid* proto, std::size_t len) { return fromNetOid(proto, len); }

/// A session filled by SessionSetup from @p cfg (the setup must outlive the session's use).
class Configured {
public:
    explicit Configured(const SessionConfig& cfg) : setup_(cfg) {
        snmp_sess_init(&sess_);
        setup_.configure(sess_);
    }
    netsnmp_session& sess() { return sess_; }

private:
    SessionSetup setup_;
    netsnmp_session sess_{};
};

SessionConfig v3(L level) {
    SessionConfig cfg;
    cfg.version = V::V3;
    cfg.user = "admin";
    cfg.securityLevel = level;
    cfg.authPassphrase = "authpass123";
    cfg.privPassphrase = "privpass123";
    return cfg;
}

// =================================================================================================
// SessionSetup
// =================================================================================================

TEST(SessionSetup, KeepsACopyOfTheConfig) {
    SessionConfig cfg;
    cfg.peer = "10.0.0.1";
    const SessionSetup setup(cfg);
    cfg.peer = "changed";
    EXPECT_EQ(setup.config().peer, "10.0.0.1");
}

TEST(SessionSetup, LocalAddressWithATcpPeerIsRefused) {
    SessionConfig cfg;
    cfg.peer = "tcp:10.0.0.1:161";
    cfg.localAddress = "10.0.0.5";
    try {
        SessionSetup setup(cfg);
        FAIL() << "accepted";
    } catch (const Error& e) {
        EXPECT_THAT(e.what(), HasSubstr("UDP peers only"));
    }
}

TEST(SessionSetup, TcpPeerAloneAndLocalAddressWithUdpAreFine) {
    SessionConfig tcp;
    tcp.peer = "tcp:10.0.0.1:161";
    EXPECT_NO_THROW(SessionSetup{tcp});
    SessionConfig udp;
    udp.peer = "udp:10.0.0.1:161";
    udp.localAddress = "10.0.0.5";
    EXPECT_NO_THROW(SessionSetup{udp});
}

TEST(SessionSetup, PeerTimeoutAndRetries) {
    SessionConfig cfg;
    cfg.peer = "192.168.1.20:11161";
    cfg.timeout = std::chrono::milliseconds(250);
    cfg.retries = 3;
    Configured c(cfg);
    EXPECT_STREQ(c.sess().peername, "192.168.1.20:11161");
    EXPECT_EQ(c.sess().timeout, 250000);  // microseconds
    EXPECT_EQ(c.sess().retries, 3);
}

TEST(SessionSetup, V2cUsesTheCommunity) {
    SessionConfig cfg;
    cfg.community = "private";
    Configured c(cfg);
    EXPECT_EQ(c.sess().version, SNMP_VERSION_2c);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(c.sess().community), c.sess().community_len), "private");
}

TEST(SessionSetup, V1) {
    SessionConfig cfg;
    cfg.version = V::V1;
    Configured c(cfg);
    EXPECT_EQ(c.sess().version, SNMP_VERSION_1);
}

TEST(SessionSetup, V3UserAndContext) {
    SessionConfig cfg = v3(L::NoAuthNoPriv);
    cfg.contextName = "ctx";
    Configured c(cfg);
    EXPECT_EQ(c.sess().version, SNMP_VERSION_3);
    EXPECT_EQ(std::string(c.sess().securityName, c.sess().securityNameLen), "admin");
    EXPECT_EQ(std::string(c.sess().contextName, c.sess().contextNameLen), "ctx");
}

TEST(SessionSetup, V3WithoutContextLeavesTheDefault) {
    Configured c(v3(L::NoAuthNoPriv));
    EXPECT_EQ(c.sess().contextNameLen, 0u);
}

struct LevelCase {
    L level;
    int netsnmp;
};

class SessionSetupLevel : public ::testing::TestWithParam<LevelCase> {};

TEST_P(SessionSetupLevel, MapsTheSecurityLevel) {
    Configured c(v3(GetParam().level));
    EXPECT_EQ(c.sess().securityLevel, GetParam().netsnmp);
}

INSTANTIATE_TEST_SUITE_P(Client, SessionSetupLevel,
                         ::testing::Values(LevelCase{L::NoAuthNoPriv, SNMP_SEC_LEVEL_NOAUTH},
                                           LevelCase{L::AuthNoPriv, SNMP_SEC_LEVEL_AUTHNOPRIV},
                                           LevelCase{L::AuthPriv, SNMP_SEC_LEVEL_AUTHPRIV}));

TEST(SessionSetup, NoAuthNoPrivDerivesNoKeys) {
    Configured c(v3(L::NoAuthNoPriv));
    EXPECT_EQ(c.sess().securityAuthKeyLen, 0u);
    EXPECT_EQ(c.sess().securityPrivKeyLen, 0u);
}

TEST(SessionSetup, AuthNoPrivDerivesOnlyTheAuthKey) {
    Configured c(v3(L::AuthNoPriv));
    EXPECT_GT(c.sess().securityAuthKeyLen, 0u);
    EXPECT_EQ(c.sess().securityPrivKeyLen, 0u);
}

TEST(SessionSetup, AuthPrivDerivesBothKeys) {
    Configured c(v3(L::AuthPriv));
    EXPECT_GT(c.sess().securityAuthKeyLen, 0u);
    EXPECT_GT(c.sess().securityPrivKeyLen, 0u);
}

TEST(SessionSetup, DifferentPassphrasesGiveDifferentKeys) {
    SessionConfig a = v3(L::AuthNoPriv), b = v3(L::AuthNoPriv);
    b.authPassphrase = "otherpass123";
    Configured ca(a), cb(b);
    ASSERT_EQ(ca.sess().securityAuthKeyLen, cb.sess().securityAuthKeyLen);
    EXPECT_NE(std::memcmp(ca.sess().securityAuthKey, cb.sess().securityAuthKey, ca.sess().securityAuthKeyLen), 0);
}

TEST(SessionSetup, PassphraseShorterThan8CharactersIsAnError) {
    SessionConfig cfg = v3(L::AuthNoPriv);
    cfg.authPassphrase = "short";
    const SessionSetup setup(cfg);
    netsnmp_session sess;
    snmp_sess_init(&sess);
    EXPECT_THROW(setup.configure(sess), Error);
}

struct AuthCase {
    A protocol;
    Oid oid;
};

class SessionSetupAuth : public ::testing::TestWithParam<AuthCase> {};

// The protocols this Net-SNMP build lacks (SHA-2 without OpenSSL) are an Error, never a wrong protocol.
TEST_P(SessionSetupAuth, ChoosesTheProtocolOrReportsItMissing) {
    SessionConfig cfg = v3(L::AuthNoPriv);
    cfg.authProtocol = GetParam().protocol;
    const SessionSetup setup(cfg);
    netsnmp_session sess;
    snmp_sess_init(&sess);
    try {
        setup.configure(sess);
    } catch (const Error& e) {
        EXPECT_THAT(e.what(), HasSubstr("authentication key"));
        return;
    }
    EXPECT_EQ(protoOid(sess.securityAuthProto, sess.securityAuthProtoLen), GetParam().oid);
}

INSTANTIATE_TEST_SUITE_P(Client, SessionSetupAuth,
                         ::testing::Values(AuthCase{A::MD5, Oid({1, 3, 6, 1, 6, 3, 10, 1, 1, 2})},
                                           AuthCase{A::SHA1, Oid({1, 3, 6, 1, 6, 3, 10, 1, 1, 3})},
                                           AuthCase{A::SHA224, Oid({1, 3, 6, 1, 6, 3, 10, 1, 1, 4})},
                                           AuthCase{A::SHA256, Oid({1, 3, 6, 1, 6, 3, 10, 1, 1, 5})},
                                           AuthCase{A::SHA384, Oid({1, 3, 6, 1, 6, 3, 10, 1, 1, 6})},
                                           AuthCase{A::SHA512, Oid({1, 3, 6, 1, 6, 3, 10, 1, 1, 7})}));

struct PrivCase {
    P protocol;
    Oid oid;
};

class SessionSetupPriv : public ::testing::TestWithParam<PrivCase> {};

TEST_P(SessionSetupPriv, ChoosesTheProtocolOrReportsItMissing) {
    SessionConfig cfg = v3(L::AuthPriv);
    cfg.privProtocol = GetParam().protocol;
    const SessionSetup setup(cfg);
    netsnmp_session sess;
    snmp_sess_init(&sess);
    try {
        setup.configure(sess);
    } catch (const Error& e) {
        EXPECT_THAT(e.what(), HasSubstr("privacy key"));
        return;
    }
    EXPECT_EQ(protoOid(sess.securityPrivProto, sess.securityPrivProtoLen), GetParam().oid);
}

INSTANTIATE_TEST_SUITE_P(Client, SessionSetupPriv,
                         ::testing::Values(PrivCase{P::DES, Oid({1, 3, 6, 1, 6, 3, 10, 1, 2, 2})},
                                           PrivCase{P::AES128, Oid({1, 3, 6, 1, 6, 3, 10, 1, 2, 4})},
                                           PrivCase{P::AES192, Oid({1, 3, 6, 1, 4, 1, 14832, 1, 3})},
                                           PrivCase{P::AES256, Oid({1, 3, 6, 1, 4, 1, 14832, 1, 4})}));

// =================================================================================================
// ClientAddrScope
// =================================================================================================

struct PortCase {
    const char* address;
    bool namesPort;
};

class ClientAddrNamesPort : public ::testing::TestWithParam<PortCase> {};

TEST_P(ClientAddrNamesPort, DetectsAPort) { EXPECT_EQ(ClientAddrScope::namesPort(GetParam().address), GetParam().namesPort); }

INSTANTIATE_TEST_SUITE_P(Client, ClientAddrNamesPort,
                         ::testing::Values(PortCase{"192.168.1.5", false}, PortCase{"192.168.1.5:5000", true},
                                           PortCase{"fe80::1", false}, PortCase{"[fe80::1]", false},
                                           PortCase{"[fe80::1]:5000", true}, PortCase{"", false}));

const char* clientAddr() { return netsnmp_ds_get_string(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_CLIENT_ADDR); }
bool clientAddrUsesPort() { return netsnmp_ds_get_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_CLIENT_ADDR_USES_PORT) != 0; }

class ClientAddrScopeTest : public ::testing::Test {
protected:
    void TearDown() override {
        netsnmp_ds_set_string(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_CLIENT_ADDR, nullptr);
        netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_CLIENT_ADDR_USES_PORT, 0);
    }
};

TEST_F(ClientAddrScopeTest, SetsTheAddressWhileItLives) {
    {
        ClientAddrScope scope("10.0.0.5:5000");
        ASSERT_NE(clientAddr(), nullptr);
        EXPECT_STREQ(clientAddr(), "10.0.0.5:5000");
        EXPECT_TRUE(clientAddrUsesPort());
    }
    EXPECT_EQ(clientAddr(), nullptr);
    EXPECT_FALSE(clientAddrUsesPort());
}

TEST_F(ClientAddrScopeTest, WithoutPortTheSystemPicksOne) {
    ClientAddrScope scope("10.0.0.5");
    EXPECT_FALSE(clientAddrUsesPort());
}

TEST_F(ClientAddrScopeTest, RestoresWhatWasSetBefore) {  // e.g. clientaddr from snmp.conf
    netsnmp_ds_set_string(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_CLIENT_ADDR, "10.9.9.9");
    { ClientAddrScope scope("10.0.0.5:5000"); }
    ASSERT_NE(clientAddr(), nullptr);
    EXPECT_STREQ(clientAddr(), "10.9.9.9");
    EXPECT_FALSE(clientAddrUsesPort());
}

TEST_F(ClientAddrScopeTest, EmptyAddressChangesNothing) {
    netsnmp_ds_set_string(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_CLIENT_ADDR, "10.9.9.9");
    {
        ClientAddrScope scope("");
        EXPECT_STREQ(clientAddr(), "10.9.9.9");
    }
    EXPECT_STREQ(clientAddr(), "10.9.9.9");
}

// =================================================================================================
// ResponseReader
// =================================================================================================

/// A response PDU, freed at the end of the test.
class Response {
public:
    Response() : pdu_(snmp_pdu_create(SNMP_MSG_RESPONSE)) {
        pdu_->errstat = SNMP_ERR_NOERROR;  // as in a received answer (snmp_pdu_create puts a placeholder there)
        pdu_->errindex = 0;
    }
    ~Response() { snmp_free_pdu(pdu_); }
    Response(const Response&) = delete;
    Response& operator=(const Response&) = delete;
    Response& add(const Oid& oid, const Value& value) {
        const auto name = toNetOid(oid);
        assign(snmp_pdu_add_variable(pdu_, name.data(), name.size(), ASN_NULL, nullptr, 0), value);
        return *this;
    }
    Response& error(int status, int index) {
        pdu_->errstat = status;
        pdu_->errindex = index;
        return *this;
    }
    netsnmp_pdu& pdu() { return *pdu_; }

private:
    netsnmp_pdu* pdu_;
};

const Oid kA{1, 3, 6, 1, 2, 1, 1, 1, 0};
const Oid kB{1, 3, 6, 1, 2, 1, 1, 3, 0};

TEST(ResponseReader, ReadsTheVarbindsInOrder) {
    Response r;
    r.add(kA, Value::string("x")).add(kB, Value::timeTicks(5));
    const auto vbs = ResponseReader(SNMP_MSG_GET, 2, "peer").read(r.pdu());
    ASSERT_EQ(vbs.size(), 2u);
    EXPECT_EQ(vbs[0].oid, kA);
    EXPECT_EQ(vbs[0].value, Value::string("x"));
    EXPECT_EQ(vbs[1].value, Value::timeTicks(5));
}

TEST(ResponseReader, ExceptionMarkersAreValues) {
    Response r;
    r.add(kA, Value::exception(Type::NoSuchInstance));
    EXPECT_EQ(ResponseReader(SNMP_MSG_GET, 1, "peer").read(r.pdu()).front().value, Value::exception(Type::NoSuchInstance));
}

TEST(ResponseReader, ErrorStatusIsAResponseErrorWithItsIndex) {
    Response r;
    r.add(kA, Value::integer(1)).add(kB, Value::integer(2)).error(SNMP_ERR_NOTWRITABLE, 2);
    try {
        ResponseReader(SNMP_MSG_SET, 2, "peer").read(r.pdu());
        FAIL() << "no ResponseError";
    } catch (const ResponseError& e) {
        EXPECT_EQ(e.status(), ErrorStatus::NotWritable);
        EXPECT_EQ(e.index(), 2);
    }
}

class ResponseReaderCount : public ::testing::TestWithParam<int> {};

TEST_P(ResponseReaderCount, GetGetNextAndSetMustAnswerEveryVarbind) {  // RFC 3416 4.2
    Response r;
    r.add(kA, Value::integer(1));
    try {
        ResponseReader(GetParam(), 2, "10.0.0.1").read(r.pdu());
        FAIL() << "a short answer was accepted";
    } catch (const ResponseError&) {
        FAIL() << "not a ResponseError";
    } catch (const Error& e) {
        EXPECT_THAT(e.what(), HasSubstr("answered 1 varbinds to a request with 2"));
        EXPECT_THAT(e.what(), HasSubstr("10.0.0.1"));
    }
}

INSTANTIATE_TEST_SUITE_P(Client, ResponseReaderCount, ::testing::Values(SNMP_MSG_GET, SNMP_MSG_GETNEXT, SNMP_MSG_SET));

TEST(ResponseReader, GetBulkMayAnswerAnyNumber) {
    Response r;
    r.add(kA, Value::integer(1)).add(kB, Value::integer(2));
    EXPECT_EQ(ResponseReader(SNMP_MSG_GETBULK, 1, "peer").read(r.pdu()).size(), 2u);
    Response empty;
    EXPECT_TRUE(ResponseReader(SNMP_MSG_GETBULK, 1, "peer").read(empty.pdu()).empty());
}

TEST(ResponseReader, ValueSnmpwrapCannotRepresentIsAnError) {
    Response r;
    r.add(kA, Value::integer(1));
    const u_char nsap[] = {1, 2};
    snmp_set_var_typed_value(r.pdu().variables, ASN_NSAP, nsap, sizeof nsap);
    EXPECT_THROW(ResponseReader(SNMP_MSG_GET, 1, "peer").read(r.pdu()), Error);
}

// =================================================================================================
// Walker
// =================================================================================================

const Oid kRoot{1, 3, 6, 1, 2, 1, 1};

/// A fetch function that answers with prepared batches and records what it was asked for.
struct FakeFetch {
    std::vector<std::vector<VarBind>> batches;
    std::vector<Oid> asked;
    std::size_t next = 0;
    Walker::Fetch fn() {
        return [this](const Oid& after) {
            asked.push_back(after);
            return next < batches.size() ? batches[next++] : std::vector<VarBind>{};
        };
    }
};

std::vector<Oid> walked(const Walker& w) {
    std::vector<Oid> out;
    w.walk([&](const VarBind& vb) {
        out.push_back(vb.oid);
        return true;
    });
    return out;
}

TEST(Walker, StartsAtTheRootAndContinuesAfterTheLastOid) {
    FakeFetch f;
    f.batches = {{{kRoot + Oid({1, 0}), Value::integer(1)}, {kRoot + Oid({2, 0}), Value::integer(2)}},
                 {{kRoot + Oid({3, 0}), Value::integer(3)}}};
    const Walker w(kRoot, false, f.fn());
    EXPECT_EQ(walked(w), (std::vector<Oid>{kRoot + Oid({1, 0}), kRoot + Oid({2, 0}), kRoot + Oid({3, 0})}));
    ASSERT_GE(f.asked.size(), 2u);
    EXPECT_EQ(f.asked[0], kRoot);
    EXPECT_EQ(f.asked[1], kRoot + Oid({2, 0}));
}

TEST(Walker, StopsAtTheEndOfTheSubtree) {
    FakeFetch f;
    f.batches = {{{kRoot + Oid({1, 0}), Value::integer(1)}, {Oid({1, 3, 6, 1, 2, 1, 2, 1, 0}), Value::integer(2)}}};
    EXPECT_EQ(walked(Walker(kRoot, false, f.fn())), (std::vector<Oid>{kRoot + Oid({1, 0})}));
    EXPECT_EQ(f.asked.size(), 1u);
}

TEST(Walker, StopsAtEndOfMibView) {
    FakeFetch f;
    f.batches = {{{kRoot + Oid({1, 0}), Value::integer(1)}, {kRoot + Oid({2, 0}), Value::exception(Type::EndOfMibView)}}};
    EXPECT_EQ(walked(Walker(kRoot, false, f.fn())).size(), 1u);
}

TEST(Walker, StopsAtAnEmptyAnswer) {
    FakeFetch f;
    EXPECT_TRUE(walked(Walker(kRoot, false, f.fn())).empty());
    EXPECT_EQ(f.asked.size(), 1u);
}

TEST(Walker, StopsWhenTheCallbackSaysSo) {
    FakeFetch f;
    f.batches = {{{kRoot + Oid({1, 0}), Value::integer(1)}, {kRoot + Oid({2, 0}), Value::integer(2)}}};
    int calls = 0;
    Walker(kRoot, false, f.fn()).walk([&](const VarBind&) {
        ++calls;
        return false;
    });
    EXPECT_EQ(calls, 1);
}

TEST(Walker, NonIncreasingOidIsAnError) {  // the walk would never end
    FakeFetch f;
    f.batches = {{{kRoot + Oid({2, 0}), Value::integer(1)}, {kRoot + Oid({1, 0}), Value::integer(2)}}};
    EXPECT_THROW(walked(Walker(kRoot, false, f.fn())), Error);
}

TEST(Walker, SameOidTwiceIsAnError) {
    FakeFetch f;
    f.batches = {{{kRoot + Oid({1, 0}), Value::integer(1)}}, {{kRoot + Oid({1, 0}), Value::integer(1)}}};
    EXPECT_THROW(walked(Walker(kRoot, false, f.fn())), Error);
}

TEST(Walker, OnV1NoSuchNameIsTheEnd) {
    int calls = 0;
    const Walker w(kRoot, true, [&](const Oid&) -> std::vector<VarBind> {
        if (calls++ == 0) return {{kRoot + Oid({1, 0}), Value::integer(1)}};
        throw ResponseError(ErrorStatus::NoSuchName, 1, "end of MIB");
    });
    EXPECT_EQ(walked(w).size(), 1u);
}

TEST(Walker, OnV2NoSuchNameIsAnError) {
    const Walker w(kRoot, false, [](const Oid&) -> std::vector<VarBind> { throw ResponseError(ErrorStatus::NoSuchName, 1, "x"); });
    EXPECT_THROW(walked(w), ResponseError);
}

TEST(Walker, OtherErrorsArePassedOn) {
    const Walker w(kRoot, true, [](const Oid&) -> std::vector<VarBind> { throw TransportError("timeout"); });
    EXPECT_THROW(walked(w), TransportError);
}

}  // namespace
