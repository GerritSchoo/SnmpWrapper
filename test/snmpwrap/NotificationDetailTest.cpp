// Unit tests of NotificationDetail.hpp / NotificationDetail.cpp (internal): NotificationDecoder turns received
// PDUs into Notifications - SNMPv2c traps and informs, SNMPv1 traps (RFC 3584), the community filter. The PDUs
// are built by hand; no socket.

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Convert.hpp"
#include "NotificationDetail.hpp"
#include "TestSupport.hpp"

using namespace snmpwrap;
using namespace snmpwrap::detail;

namespace {

const Oid kSysUpTime{1, 3, 6, 1, 2, 1, 1, 3, 0};
const Oid kSnmpTrapOid{1, 3, 6, 1, 6, 3, 1, 1, 4, 1, 0};
const Oid kAlarm{1, 3, 6, 1, 4, 1, 99999, 3, 0, 1};
const Oid kName{1, 3, 6, 1, 4, 1, 99999, 1, 1, 0};

/// A received PDU, freed at the end of the test.
class Pdu {
public:
    explicit Pdu(int command, const std::string& community = "public") : pdu_(snmp_pdu_create(command)) {
        pdu_->community = static_cast<u_char*>(malloc(community.size() + 1));
        std::memcpy(pdu_->community, community.c_str(), community.size() + 1);
        pdu_->community_len = community.size();
    }
    ~Pdu() { snmp_free_pdu(pdu_); }
    Pdu(const Pdu&) = delete;
    Pdu& operator=(const Pdu&) = delete;

    Pdu& add(const Oid& oid, const Value& value) {
        const auto name = toNetOid(oid);
        assign(snmp_pdu_add_variable(pdu_, name.data(), name.size(), ASN_NULL, nullptr, 0), value);
        return *this;
    }
    /// The two varbinds every SNMPv2 notification starts with.
    Pdu& v2Header(std::uint32_t uptime, const Oid& trapOid) {
        return add(kSysUpTime, Value::timeTicks(uptime)).add(kSnmpTrapOid, Value::oid(trapOid));
    }
    /// SNMPv1 trap fields.
    Pdu& v1(const Oid& enterprise, long generic, long specific, long time) {
        const auto ent = toNetOid(enterprise);
        pdu_->enterprise = static_cast<::oid*>(malloc(ent.size() * sizeof(::oid)));
        std::memcpy(pdu_->enterprise, ent.data(), ent.size() * sizeof(::oid));
        pdu_->enterprise_length = ent.size();
        pdu_->trap_type = generic;
        pdu_->specific_type = specific;
        pdu_->time = time;
        return *this;
    }
    netsnmp_pdu& pdu() { return *pdu_; }

private:
    netsnmp_pdu* pdu_;
};

// --- SNMPv2c --------------------------------------------------------------------------------------

TEST(NotificationDecoder, V2TrapTakesTrapOidAndUptimeFromTheirVarbinds) {
    Pdu p(SNMP_MSG_TRAP2);
    p.v2Header(1234, kAlarm).add(kName, Value::string("pump"));
    const auto n = NotificationDecoder().decode(p.pdu(), "UDP: [127.0.0.1]:5000");
    ASSERT_TRUE(n);
    EXPECT_EQ(n->version, 2);
    EXPECT_FALSE(n->inform);
    EXPECT_EQ(n->trapOid, kAlarm);
    EXPECT_EQ(n->uptime, 1234u);
    EXPECT_EQ(n->community, "public");
    EXPECT_EQ(n->source, "UDP: [127.0.0.1]:5000");
}

TEST(NotificationDecoder, V2VarsAreTheObjectsWithoutTheHeader) {
    Pdu p(SNMP_MSG_TRAP2);
    p.v2Header(1, kAlarm).add(kName, Value::string("pump")).add(kName + SubId{9}, Value::integer(42));
    const auto n = NotificationDecoder().decode(p.pdu(), "");
    ASSERT_TRUE(n);
    ASSERT_EQ(n->vars.size(), 2u);
    EXPECT_EQ(n->vars[0].oid, kName);
    EXPECT_EQ(n->vars[0].value, Value::string("pump"));
    EXPECT_EQ(n->vars[1].value, Value::integer(42));
}

TEST(NotificationDecoder, InformIsMarked) {
    Pdu p(SNMP_MSG_INFORM);
    p.v2Header(1, kAlarm);
    const auto n = NotificationDecoder().decode(p.pdu(), "");
    ASSERT_TRUE(n);
    EXPECT_TRUE(n->inform);
    EXPECT_EQ(n->version, 2);
}

TEST(NotificationDecoder, HeaderVarbindsOfTheWrongTypeAreOrdinaryVars) {
    Pdu p(SNMP_MSG_TRAP2);
    p.add(kSysUpTime, Value::integer(5)).add(kSnmpTrapOid, Value::string("not an OID"));
    const auto n = NotificationDecoder().decode(p.pdu(), "");
    ASSERT_TRUE(n);
    EXPECT_EQ(n->uptime, 0u);
    EXPECT_EQ(n->trapOid, Oid{});
    EXPECT_EQ(n->vars.size(), 2u);
}

TEST(NotificationDecoder, VarbindsSnmpwrapCannotRepresentAreSkipped) {
    Pdu p(SNMP_MSG_TRAP2);
    p.v2Header(1, kAlarm).add(kName, Value::integer(1)).add(kName + SubId{1}, Value::integer(2));
    const u_char nsap[] = {1, 2};
    snmp_set_var_typed_value(p.pdu().variables->next_variable->next_variable, ASN_NSAP, nsap, sizeof nsap);
    const auto n = NotificationDecoder().decode(p.pdu(), "");
    ASSERT_TRUE(n);
    ASSERT_EQ(n->vars.size(), 1u);
    EXPECT_EQ(n->vars[0].value, Value::integer(2));
}

// --- SNMPv1 (RFC 3584 3.1) ------------------------------------------------------------------------

TEST(NotificationDecoder, V1GenericTrapBecomesSnmpTrapsGenericPlusOne) {
    Pdu p(SNMP_MSG_TRAP);
    p.v1(Oid({1, 3, 6, 1, 4, 1, 99999}), SNMP_TRAP_COLDSTART, 0, 77);
    const auto n = NotificationDecoder().decode(p.pdu(), "");
    ASSERT_TRUE(n);
    EXPECT_EQ(n->version, 1);
    EXPECT_EQ(n->trapOid, Oid({1, 3, 6, 1, 6, 3, 1, 1, 5, 1}));  // coldStart
    EXPECT_EQ(n->uptime, 77u);
}

TEST(NotificationDecoder, V1LinkDown) {
    Pdu p(SNMP_MSG_TRAP);
    p.v1(Oid({1, 3, 6, 1, 4, 1, 99999}), SNMP_TRAP_LINKDOWN, 0, 0);
    EXPECT_EQ(NotificationDecoder().decode(p.pdu(), "")->trapOid, Oid({1, 3, 6, 1, 6, 3, 1, 1, 5, 3}));
}

TEST(NotificationDecoder, V1EnterpriseSpecificTrapIsEnterpriseZeroSpecific) {
    Pdu p(SNMP_MSG_TRAP);
    p.v1(Oid({1, 3, 6, 1, 4, 1, 99999, 3}), SNMP_TRAP_ENTERPRISESPECIFIC, 1, 0).add(kName, Value::string("pump"));
    const auto n = NotificationDecoder().decode(p.pdu(), "");
    ASSERT_TRUE(n);
    EXPECT_EQ(n->trapOid, kAlarm);  // 1.3.6.1.4.1.99999.3 . 0 . 1
    ASSERT_EQ(n->vars.size(), 1u);  // v1 has no header varbinds
}

TEST(NotificationDecoder, V1KeepsVarbindsNamedLikeTheV2Header) {
    Pdu p(SNMP_MSG_TRAP);
    p.v1(Oid({1, 3, 6, 1, 4, 1, 99999}), SNMP_TRAP_COLDSTART, 0, 5).add(kSysUpTime, Value::timeTicks(9));
    const auto n = NotificationDecoder().decode(p.pdu(), "");
    ASSERT_TRUE(n);
    EXPECT_EQ(n->uptime, 5u);  // from the PDU, not from the varbind
    EXPECT_EQ(n->vars.size(), 1u);
}

// --- what is not decoded --------------------------------------------------------------------------

class NotificationDecoderIgnores : public ::testing::TestWithParam<int> {};

TEST_P(NotificationDecoderIgnores, OtherPdus) {
    Pdu p(GetParam());
    p.add(kName, Value::null());
    EXPECT_FALSE(NotificationDecoder().decode(p.pdu(), ""));
}

INSTANTIATE_TEST_SUITE_P(Notification, NotificationDecoderIgnores,
                         ::testing::Values(SNMP_MSG_GET, SNMP_MSG_GETNEXT, SNMP_MSG_SET, SNMP_MSG_RESPONSE, SNMP_MSG_GETBULK));

// --- the community filter -------------------------------------------------------------------------

TEST(NotificationDecoder, AcceptsEveryCommunityByDefault) {
    Pdu p(SNMP_MSG_TRAP2, "anything");
    p.v2Header(1, kAlarm);
    EXPECT_TRUE(NotificationDecoder().decode(p.pdu(), ""));
}

TEST(NotificationDecoder, AcceptsOnlyTheGivenCommunities) {
    NotificationDecoder d;
    d.acceptCommunities({"public", "monitor"});
    Pdu ok(SNMP_MSG_TRAP2, "monitor"), other(SNMP_MSG_TRAP2, "private");
    ok.v2Header(1, kAlarm);
    other.v2Header(1, kAlarm);
    EXPECT_TRUE(d.decode(ok.pdu(), ""));
    EXPECT_FALSE(d.decode(other.pdu(), ""));
}

TEST(NotificationDecoder, AnEmptyListAcceptsEveryCommunityAgain) {
    NotificationDecoder d;
    d.acceptCommunities({"public"});
    d.acceptCommunities({});
    Pdu p(SNMP_MSG_TRAP2, "private");
    p.v2Header(1, kAlarm);
    EXPECT_TRUE(d.decode(p.pdu(), ""));
}

TEST(NotificationDecoder, InformsOfOtherCommunitiesAreDropped) {  // and so not acknowledged by the receiver
    NotificationDecoder d;
    d.acceptCommunities({"public"});
    Pdu p(SNMP_MSG_INFORM, "private");
    p.v2Header(1, kAlarm);
    EXPECT_FALSE(d.decode(p.pdu(), ""));
}

}  // namespace
