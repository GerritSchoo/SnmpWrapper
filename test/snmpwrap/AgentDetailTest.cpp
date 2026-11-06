// Unit tests of AgentDetail.hpp / AgentDetail.cpp (internal): how a Registration answers the phases of a
// request - GET, GETNEXT, the SET phases, a removed subtree - and the TxnHolder bookkeeping. The IHandler and its
// ISetTransaction are mocks; the requests are Net-SNMP structures built by hand (no Agent, no snmpd).

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <vector>

#include "AgentDetail.hpp"
#include "Convert.hpp"
#include "snmpwrap/IHandlerMock.hpp"
#include "snmpwrap/IMibBuilderMock.hpp"
#include "TestSupport.hpp"

using namespace snmpwrap;
using namespace snmpwrap::detail;
using namespace snmpwrap::test;
using ::testing::_;
using ::testing::ByMove;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::Return;
using ::testing::StrictMock;
using ::testing::Throw;

namespace {

const Oid kRoot{1, 3, 6, 1, 4, 1, 99999};
const Oid kA = kRoot + Oid({1, 1, 0});
const Oid kB = kRoot + Oid({1, 2, 0});

/// The request Net-SNMP hands to a handler: the request info (mode, attached data) and a list of varbinds.
class Request {
public:
    Request(int mode, const std::vector<VarBind>& vbs) {
        info_.mode = mode;
        for (const VarBind& vb : vbs) {
            auto r = std::make_unique<netsnmp_request_info>();
            const auto name = toNetOid(vb.oid);
            snmp_varlist_add_variable(&r->requestvb, name.data(), name.size(), ASN_NULL, nullptr, 0);
            assign(r->requestvb, vb.value);
            r->agent_req_info = &info_;
            if (!requests_.empty()) requests_.back()->next = r.get();
            requests_.push_back(std::move(r));
        }
    }
    ~Request() {
        netsnmp_free_all_list_data(info_.agent_data);  // what Net-SNMP does when the request is done
        for (auto& r : requests_) snmp_free_varbind(r->requestvb);
    }
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    /// The next phase of the same request (SET: RESERVE1 -> ACTION -> COMMIT / UNDO).
    Request& phase(int mode) {
        info_.mode = mode;
        return *this;
    }
    /// Frees what is attached to the request now (FREE: the end of the request).
    void release() {
        netsnmp_free_all_list_data(info_.agent_data);
        info_.agent_data = nullptr;
    }

    int handledBy(Registration& reg) { return reg.handle(&info_, requests_.empty() ? nullptr : requests_.front().get()); }

    netsnmp_request_info& at(std::size_t i) { return *requests_.at(i); }
    Oid name(std::size_t i) { return fromNetOid(at(i).requestvb->name, at(i).requestvb->name_length); }
    Value value(std::size_t i) { return fromVar(at(i).requestvb); }
    int status(std::size_t i) { return at(i).status; }
    bool processed(std::size_t i) { return at(i).processed != 0; }

private:
    netsnmp_agent_request_info info_{};
    std::vector<std::unique_ptr<netsnmp_request_info>> requests_;
};

class RegistrationTest : public ::testing::Test {
protected:
    /// A transaction the mocked handler returns from prepare(); the test keeps a pointer to set expectations.
    StrictMock<MockSetTransaction>* expectPrepare(const std::vector<VarBind>& sets) {
        auto txn = std::make_unique<StrictMock<MockSetTransaction>>();
        auto* raw = txn.get();
        EXPECT_CALL(*handler, prepare(::testing::SizeIs(sets.size())))
            .WillOnce(Return(ByMove(std::unique_ptr<ISetTransaction>(std::move(txn)))));
        return raw;
    }

    std::shared_ptr<StrictMock<MockHandler>> handler = std::make_shared<StrictMock<MockHandler>>();
    Registration reg{kRoot, handler};
};

// --- accessors ------------------------------------------------------------------------------------

TEST_F(RegistrationTest, KeepsRootAndHandler) {
    EXPECT_EQ(reg.root(), kRoot);
    EXPECT_EQ(reg.handler(), handler);
    EXPECT_EQ(reg.netsnmpRegistration(), nullptr);
}

TEST_F(RegistrationTest, TxnKeyNamesTheRoot) { EXPECT_EQ(reg.txnKey(), "snmpwrap.txn." + kRoot.str()); }

TEST_F(RegistrationTest, StartsActiveWithoutPendingSets) {
    EXPECT_FALSE(reg.removed());
    EXPECT_EQ(reg.pendingSets(), 0);
    EXPECT_FALSE(reg.canBeUnregistered());
}

TEST_F(RegistrationTest, RemovedWithoutPendingSetsCanBeUnregistered) {
    reg.markRemoved();
    EXPECT_TRUE(reg.removed());
    EXPECT_TRUE(reg.canBeUnregistered());
}

// --- GET ------------------------------------------------------------------------------------------

TEST_F(RegistrationTest, GetAnswersWithTheHandlersValue) {
    EXPECT_CALL(*handler, get(kA)).WillOnce(Return(Value::integer(42)));
    Request req(MODE_GET, {{kA, Value::null()}});
    EXPECT_EQ(req.handledBy(reg), SNMP_ERR_NOERROR);
    EXPECT_EQ(req.value(0), Value::integer(42));
}

TEST_F(RegistrationTest, GetAnswersEveryVarbind) {
    EXPECT_CALL(*handler, get(kA)).WillOnce(Return(Value::integer(1)));
    EXPECT_CALL(*handler, get(kB)).WillOnce(Return(Value::string("b")));
    Request req(MODE_GET, {{kA, Value::null()}, {kB, Value::null()}});
    req.handledBy(reg);
    EXPECT_EQ(req.value(0), Value::integer(1));
    EXPECT_EQ(req.value(1), Value::string("b"));
}

TEST_F(RegistrationTest, GetOfAMissingInstanceIsNoSuchInstance) {
    EXPECT_CALL(*handler, get(kA)).WillOnce(Return(std::nullopt));
    EXPECT_CALL(*handler, missing(kA)).WillOnce(Return(Type::NoSuchInstance));
    Request req(MODE_GET, {{kA, Value::null()}});
    req.handledBy(reg);
    EXPECT_EQ(req.value(0), Value::exception(Type::NoSuchInstance));
}

TEST_F(RegistrationTest, GetOfAMissingObjectIsNoSuchObject) {
    EXPECT_CALL(*handler, get(kA)).WillOnce(Return(std::nullopt));
    EXPECT_CALL(*handler, missing(kA)).WillOnce(Return(Type::NoSuchObject));
    Request req(MODE_GET, {{kA, Value::null()}});
    req.handledBy(reg);
    EXPECT_EQ(req.value(0), Value::exception(Type::NoSuchObject));
}

TEST_F(RegistrationTest, GetThatThrowsIsGenErr) {
    EXPECT_CALL(*handler, get(kA)).WillOnce(Throw(std::runtime_error("broken")));
    Request req(MODE_GET, {{kA, Value::null()}});
    EXPECT_EQ(req.handledBy(reg), SNMP_ERR_NOERROR);  // the error is in the varbind, not in the return value
    EXPECT_EQ(req.status(0), SNMP_ERR_GENERR);
}

TEST_F(RegistrationTest, GetSkipsVarbindsAlreadyProcessed) {
    Request req(MODE_GET, {{kA, Value::null()}});
    req.at(0).processed = 1;  // answered by another handler before
    req.handledBy(reg);       // StrictMock: no call to get()
}

// --- GETNEXT --------------------------------------------------------------------------------------

TEST_F(RegistrationTest, GetNextAnswersWithTheNextInstance) {
    EXPECT_CALL(*handler, getNext(kA)).WillOnce(Return(VarBind{kB, Value::gauge(7)}));
    Request req(MODE_GETNEXT, {{kA, Value::null()}});
    req.handledBy(reg);
    EXPECT_EQ(req.name(0), kB);
    EXPECT_EQ(req.value(0), Value::gauge(7));
}

TEST_F(RegistrationTest, GetNextWithNothingMoreLeavesTheVarbindForTheNextSubtree) {
    EXPECT_CALL(*handler, getNext(kA)).WillOnce(Return(std::nullopt));
    Request req(MODE_GETNEXT, {{kA, Value::null()}});
    req.handledBy(reg);
    EXPECT_EQ(req.name(0), kA);
    EXPECT_FALSE(req.processed(0));
}

TEST_F(RegistrationTest, GetNextOutsideTheRootIsIgnored) {  // a handler must not answer for other subtrees
    EXPECT_CALL(*handler, getNext(kA)).WillOnce(Return(VarBind{Oid({1, 3, 6, 1, 4, 1, 99998, 1}), Value::integer(1)}));
    Request req(MODE_GETNEXT, {{kA, Value::null()}});
    req.handledBy(reg);
    EXPECT_EQ(req.name(0), kA);
}

TEST_F(RegistrationTest, InclusiveGetNextTriesTheStartFirst) {  // GETBULK continuation in Net-SNMP
    EXPECT_CALL(*handler, get(kA)).WillOnce(Return(Value::integer(5)));
    Request req(MODE_GETNEXT, {{kA, Value::null()}});
    req.at(0).inclusive = 1;
    req.handledBy(reg);
    EXPECT_EQ(req.name(0), kA);
    EXPECT_EQ(req.value(0), Value::integer(5));
}

TEST_F(RegistrationTest, InclusiveGetNextFallsBackToTheNext) {
    EXPECT_CALL(*handler, get(kA)).WillOnce(Return(std::nullopt));
    EXPECT_CALL(*handler, getNext(kA)).WillOnce(Return(VarBind{kB, Value::integer(6)}));
    Request req(MODE_GETNEXT, {{kA, Value::null()}});
    req.at(0).inclusive = 1;
    req.handledBy(reg);
    EXPECT_EQ(req.name(0), kB);
}

TEST_F(RegistrationTest, GetNextThatThrowsIsGenErr) {
    EXPECT_CALL(*handler, getNext(kA)).WillOnce(Throw(std::runtime_error("broken")));
    Request req(MODE_GETNEXT, {{kA, Value::null()}});
    req.handledBy(reg);
    EXPECT_EQ(req.status(0), SNMP_ERR_GENERR);
}

// --- SET phases -----------------------------------------------------------------------------------

TEST_F(RegistrationTest, Reserve1PreparesWithTheRequestedValues) {
    auto txn = std::make_unique<StrictMock<MockSetTransaction>>();
    EXPECT_CALL(*handler, prepare(ElementsAre(::testing::AllOf(Field(&VarBind::oid, kA), Field(&VarBind::value, Value::integer(3))),
                                              ::testing::AllOf(Field(&VarBind::oid, kB), Field(&VarBind::value, Value::string("x"))))))
        .WillOnce(Return(ByMove(std::unique_ptr<ISetTransaction>(std::move(txn)))));
    Request req(MODE_SET_RESERVE1, {{kA, Value::integer(3)}, {kB, Value::string("x")}});
    EXPECT_EQ(req.handledBy(reg), SNMP_ERR_NOERROR);
    EXPECT_EQ(req.status(0), SNMP_ERR_NOERROR);
}

TEST_F(RegistrationTest, Reserve1KeepsTheTransactionPendingUntilTheRequestEnds) {
    expectPrepare({{kA, Value::integer(3)}});
    Request req(MODE_SET_RESERVE1, {{kA, Value::integer(3)}});
    req.handledBy(reg);
    EXPECT_EQ(reg.pendingSets(), 1);
    req.release();
    EXPECT_EQ(reg.pendingSets(), 0);
}

TEST_F(RegistrationTest, ACommittedSetRunsApplyThenCommit) {
    auto* txn = expectPrepare({{kA, Value::integer(3)}});
    ::testing::InSequence order;
    EXPECT_CALL(*txn, apply());
    EXPECT_CALL(*txn, commit());
    Request req(MODE_SET_RESERVE1, {{kA, Value::integer(3)}});
    req.handledBy(reg);
    req.phase(MODE_SET_RESERVE2).handledBy(reg);  // nothing to do
    req.phase(MODE_SET_ACTION).handledBy(reg);
    req.phase(MODE_SET_COMMIT).handledBy(reg);
    req.phase(MODE_SET_FREE).handledBy(reg);  // nothing to do
}

TEST_F(RegistrationTest, AFailedSetRunsUndo) {
    auto* txn = expectPrepare({{kA, Value::integer(3)}});
    EXPECT_CALL(*txn, apply());
    EXPECT_CALL(*txn, undo());
    Request req(MODE_SET_RESERVE1, {{kA, Value::integer(3)}});
    req.handledBy(reg);
    req.phase(MODE_SET_ACTION).handledBy(reg);
    req.phase(MODE_SET_UNDO).handledBy(reg);  // another subtree of the request failed
}

TEST_F(RegistrationTest, Reserve1RefusedWithSetErrorMarksTheNamedVarbind) {
    EXPECT_CALL(*handler, prepare(_)).WillOnce(Throw(SetError(ErrorStatus::WrongValue, "no", 1)));
    Request req(MODE_SET_RESERVE1, {{kA, Value::integer(3)}, {kB, Value::integer(4)}});
    req.handledBy(reg);
    EXPECT_EQ(req.status(0), SNMP_ERR_NOERROR);
    EXPECT_EQ(req.status(1), SNMP_ERR_WRONGVALUE);
    EXPECT_EQ(reg.pendingSets(), 0);
}

TEST_F(RegistrationTest, Reserve1RefusedWithoutIndexMarksTheFirstVarbind) {
    EXPECT_CALL(*handler, prepare(_)).WillOnce(Throw(SetError(ErrorStatus::NotWritable)));
    Request req(MODE_SET_RESERVE1, {{kA, Value::integer(3)}, {kB, Value::integer(4)}});
    req.handledBy(reg);
    EXPECT_EQ(req.status(0), SNMP_ERR_NOTWRITABLE);
}

TEST_F(RegistrationTest, Reserve1WithAnUnexpectedExceptionIsGenErr) {
    EXPECT_CALL(*handler, prepare(_)).WillOnce(Throw(std::runtime_error("broken")));
    Request req(MODE_SET_RESERVE1, {{kA, Value::integer(3)}});
    req.handledBy(reg);
    EXPECT_EQ(req.status(0), SNMP_ERR_GENERR);
}

TEST_F(RegistrationTest, Reserve1WithoutTransactionIsGenErr) {
    EXPECT_CALL(*handler, prepare(_)).WillOnce(Return(ByMove(std::unique_ptr<ISetTransaction>())));
    Request req(MODE_SET_RESERVE1, {{kA, Value::integer(3)}});
    req.handledBy(reg);
    EXPECT_EQ(req.status(0), SNMP_ERR_GENERR);
    EXPECT_EQ(reg.pendingSets(), 0);
}

TEST_F(RegistrationTest, Reserve1WithAValueSnmpwrapCannotRepresentIsWrongType) {
    Request req(MODE_SET_RESERVE1, {{kA, Value::integer(3)}});
    const u_char nsap[] = {1, 2};
    snmp_set_var_typed_value(req.at(0).requestvb, ASN_NSAP, nsap, sizeof nsap);
    req.handledBy(reg);  // StrictMock: prepare() is not called
    EXPECT_EQ(req.status(0), SNMP_ERR_WRONGTYPE);
}

TEST_F(RegistrationTest, ApplyRefusedWithSetErrorMarksTheNamedVarbind) {
    auto* txn = expectPrepare({{kA, Value::integer(3)}, {kB, Value::integer(4)}});
    EXPECT_CALL(*txn, apply()).WillOnce(Throw(SetError(ErrorStatus::CommitFailed, "disk", 1)));
    Request req(MODE_SET_RESERVE1, {{kA, Value::integer(3)}, {kB, Value::integer(4)}});
    req.handledBy(reg);
    req.phase(MODE_SET_ACTION).handledBy(reg);
    EXPECT_EQ(req.status(1), SNMP_ERR_COMMITFAILED);
}

TEST_F(RegistrationTest, ApplyWithAnUnexpectedExceptionIsCommitFailed) {
    auto* txn = expectPrepare({{kA, Value::integer(3)}});
    EXPECT_CALL(*txn, apply()).WillOnce(Throw(std::runtime_error("disk")));
    Request req(MODE_SET_RESERVE1, {{kA, Value::integer(3)}});
    req.handledBy(reg);
    req.phase(MODE_SET_ACTION).handledBy(reg);
    EXPECT_EQ(req.status(0), SNMP_ERR_COMMITFAILED);
}

TEST_F(RegistrationTest, LaterPhasesWithoutReserve1DoNothing) {  // RESERVE1 failed elsewhere: no transaction here
    Request req(MODE_SET_ACTION, {{kA, Value::integer(3)}});
    EXPECT_EQ(req.handledBy(reg), SNMP_ERR_NOERROR);
    EXPECT_EQ(req.phase(MODE_SET_COMMIT).handledBy(reg), SNMP_ERR_NOERROR);
    EXPECT_EQ(req.phase(MODE_SET_UNDO).handledBy(reg), SNMP_ERR_NOERROR);
}

// --- unknown modes --------------------------------------------------------------------------------

TEST_F(RegistrationTest, UnknownModeIsGenErr) {
    Request req(12345, {{kA, Value::null()}});
    EXPECT_EQ(req.handledBy(reg), SNMP_ERR_GENERR);
}

// --- a removed registration -----------------------------------------------------------------------

TEST_F(RegistrationTest, RemovedGetIsNoSuchObjectWithoutAskingTheHandler) {
    reg.markRemoved();
    Request req(MODE_GET, {{kA, Value::null()}});
    req.handledBy(reg);  // StrictMock: no call
    EXPECT_EQ(req.value(0), Value::exception(Type::NoSuchObject));
}

TEST_F(RegistrationTest, RemovedGetNextLeavesTheVarbindForTheNextSubtree) {
    reg.markRemoved();
    Request req(MODE_GETNEXT, {{kA, Value::null()}});
    req.handledBy(reg);
    EXPECT_EQ(req.name(0), kA);
    EXPECT_FALSE(req.processed(0));
}

TEST_F(RegistrationTest, RemovedSetIsNoCreation) {
    reg.markRemoved();
    Request req(MODE_SET_RESERVE1, {{kA, Value::integer(1)}});
    req.handledBy(reg);
    EXPECT_EQ(req.status(0), SNMP_ERR_NOCREATION);
}

TEST_F(RegistrationTest, ASetInProgressFinishesAfterTheRemoval) {
    auto* txn = expectPrepare({{kA, Value::integer(3)}});
    EXPECT_CALL(*txn, apply());
    EXPECT_CALL(*txn, commit());
    Request req(MODE_SET_RESERVE1, {{kA, Value::integer(3)}});
    req.handledBy(reg);
    reg.markRemoved();
    EXPECT_FALSE(reg.canBeUnregistered());  // the transaction still refers to the handler
    req.phase(MODE_SET_ACTION).handledBy(reg);
    req.phase(MODE_SET_COMMIT).handledBy(reg);
    req.release();
    EXPECT_TRUE(reg.canBeUnregistered());
}

// --- TxnHolder ------------------------------------------------------------------------------------

TEST_F(RegistrationTest, TxnHolderCountsWhileItLives) {
    auto txn = std::make_unique<StrictMock<MockSetTransaction>>();
    auto* raw = txn.get();
    auto* holder = new TxnHolder(reg, std::move(txn));
    EXPECT_EQ(reg.pendingSets(), 1);
    EXPECT_EQ(&holder->transaction(), raw);
    TxnHolder::free(holder);  // the destructor callback Net-SNMP calls
    EXPECT_EQ(reg.pendingSets(), 0);
}

}  // namespace
