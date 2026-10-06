/**
 * @file session.hpp
 * @brief The interface of an SNMP session (manager side) - implemented by Client; your tests implement it with a mock.
 *
 * Program against Session& and the network becomes replaceable in tests. The generated typed client
 * (`<mib>::Remote`) takes a Session&, so it works with every implementation.
 */
#pragma once

#include <functional>
#include <vector>

#include "snmpwrap/oid.hpp"
#include "snmpwrap/value.hpp"

namespace snmpwrap {

/**
 * @brief SNMP operations of a manager, independent of how they reach an agent.
 *
 * Implemented by Client (network, v1/v2c/v3). For a mock implement the five pure virtual functions, e.g. with gmock:
 * @code
 * class MockSession : public snmpwrap::Session {
 * public:
 *     MOCK_METHOD(std::vector<snmpwrap::VarBind>, get, (const std::vector<snmpwrap::Oid>&), (override));
 *     MOCK_METHOD(snmpwrap::VarBind, getNext, (const snmpwrap::Oid&), (override));
 *     MOCK_METHOD(std::vector<snmpwrap::VarBind>, getBulk, (const std::vector<snmpwrap::Oid>&, int, int), (override));
 *     MOCK_METHOD(void, set, (const std::vector<snmpwrap::VarBind>&), (override));
 *     MOCK_METHOD(void, walk, (const snmpwrap::Oid&, const std::function<bool(const snmpwrap::VarBind&)>&), (override));
 * };
 * @endcode
 * Errors are reported as in Client: TransportError, ResponseError, Error.
 */
class Session {
public:
    virtual ~Session() = default;

    /// @brief GET of several instances in one request. @param[in] oids Instance OIDs. @return One varbind per OID, same order
    /// (missing instances as exception markers on v2c/v3).
    virtual std::vector<VarBind> get(const std::vector<Oid>& oids) = 0;
    /// @brief GETNEXT. @param[in] oid Any OID. @return The next instance, or Type::EndOfMibView.
    virtual VarBind getNext(const Oid& oid) = 0;
    /// @brief GETBULK. @param[in] oids Start OIDs. @param[in] nonRepeaters Leading OIDs with one step.
    /// @param[in] maxRepetitions Steps for the others. @return All returned varbinds.
    virtual std::vector<VarBind> getBulk(const std::vector<Oid>& oids, int nonRepeaters = 0, int maxRepetitions = 10) = 0;
    /// @brief SET of several instances in ONE atomic request. @param[in] varbinds Instances and values.
    /// @throws ResponseError (index() = failing varbind, 1-based) if the agent refuses.
    virtual void set(const std::vector<VarBind>& varbinds) = 0;
    /// @brief Walks a subtree. @param[in] root Subtree. @param[in] callback Called in OID order; return false to stop.
    virtual void walk(const Oid& root, const std::function<bool(const VarBind&)>& callback) = 0;

    /// @brief GET of one instance. @param[in] oid Instance OID. @return The varbind.
    VarBind get(const Oid& oid) { return get(std::vector<Oid>{oid}).front(); }
    /// @brief SET of one instance. @param[in] oid Instance OID. @param[in] value New value.
    void set(const Oid& oid, const Value& value) { set(std::vector<VarBind>{{oid, value}}); }
    /// @brief Walks a subtree and collects it. @param[in] root Subtree. @return All varbinds in OID order.
    std::vector<VarBind> walk(const Oid& root) {
        std::vector<VarBind> out;
        walk(root, [&out](const VarBind& vb) {
            out.push_back(vb);
            return true;
        });
        return out;
    }

protected:
    Session() = default;
    Session(const Session&) = default;
    Session& operator=(const Session&) = default;
    Session(Session&&) = default;
    Session& operator=(Session&&) = default;
};

}  // namespace snmpwrap
