// Test doubles of ISession (ISession.hpp).
//   MockSession - gmock; by default answers like a FakeSession, so EXPECT_CALL can be added on top
//   FakeSession - a working in-memory agent: answers GET / GETNEXT / walk from a map, records SETs
#pragma once

#include <gmock/gmock.h>

#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "snmpwrap/ISession.hpp"

namespace snmpwrap::test {

class FakeSession : public ISession {
public:
    std::map<Oid, Value> values;
    std::vector<std::vector<VarBind>> sets;
    bool answerNothing = false;  // a broken implementation / agent: no varbinds at all

    using ISession::get;
    using ISession::set;
    using ISession::walk;

    std::vector<VarBind> get(const std::vector<Oid>& oids) override {
        std::vector<VarBind> out;
        if (answerNothing) return out;
        for (const Oid& o : oids) out.push_back({o, values.count(o) ? values.at(o) : Value::exception(Type::NoSuchInstance)});
        return out;
    }
    VarBind getNext(const Oid& oid) override {
        auto it = values.upper_bound(oid);
        return it == values.end() ? VarBind{oid, Value::exception(Type::EndOfMibView)} : VarBind{it->first, it->second};
    }
    std::vector<VarBind> getBulk(const std::vector<Oid>& oids, int, int) override { return get(oids); }
    void set(const std::vector<VarBind>& varbinds) override {
        sets.push_back(varbinds);
        for (const auto& vb : varbinds) values[vb.oid] = vb.value;
    }
    void walk(const Oid& root, const std::function<bool(const VarBind&)>& callback) override {
        for (auto it = values.upper_bound(root); it != values.end() && root.isPrefixOf(it->first); ++it)
            if (!callback({it->first, it->second})) return;
    }
};

class MockSession : public ISession {
public:
    MOCK_METHOD(std::vector<VarBind>, get, (const std::vector<Oid>&), (override));
    MOCK_METHOD(VarBind, getNext, (const Oid&), (override));
    MOCK_METHOD(std::vector<VarBind>, getBulk, (const std::vector<Oid>&, int, int), (override));
    MOCK_METHOD(void, set, (const std::vector<VarBind>&), (override));
    MOCK_METHOD(void, walk, (const Oid&, const std::function<bool(const VarBind&)>&), (override));

    using ISession::get;
    using ISession::set;
    using ISession::walk;

    /// Lets every call without an EXPECT_CALL behave like @p fake (which must outlive this mock).
    void delegateTo(FakeSession& fake) {
        using ::testing::_;
        ON_CALL(*this, get(_)).WillByDefault([&fake](const std::vector<Oid>& o) { return fake.get(o); });
        ON_CALL(*this, getNext(_)).WillByDefault([&fake](const Oid& o) { return fake.getNext(o); });
        ON_CALL(*this, getBulk(_, _, _)).WillByDefault([&fake](const std::vector<Oid>& o, int n, int r) { return fake.getBulk(o, n, r); });
        ON_CALL(*this, set(_)).WillByDefault([&fake](const std::vector<VarBind>& v) { fake.set(v); });
        ON_CALL(*this, walk(_, _)).WillByDefault([&fake](const Oid& r, const std::function<bool(const VarBind&)>& cb) { fake.walk(r, cb); });
    }
};

}  // namespace snmpwrap::test
