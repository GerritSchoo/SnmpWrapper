// gmock mocks of the interfaces in IHandler.hpp: IHandler, ISetTransaction.
#pragma once

#include <gmock/gmock.h>

#include <memory>
#include <optional>
#include <vector>

#include "snmpwrap/IHandler.hpp"

namespace snmpwrap::test {

class MockHandler : public IHandler {
public:
    MOCK_METHOD(std::optional<Value>, get, (const Oid&), (override));
    MOCK_METHOD(std::optional<VarBind>, getNext, (const Oid&), (override));
    MOCK_METHOD(Type, missing, (const Oid&), (override));
    MOCK_METHOD(std::unique_ptr<ISetTransaction>, prepare, (const std::vector<VarBind>&), (override));
};

class MockSetTransaction : public ISetTransaction {
public:
    MOCK_METHOD(void, apply, (), (override));
    MOCK_METHOD(void, undo, (), (noexcept, override));
    MOCK_METHOD(void, commit, (), (noexcept, override));
};

}  // namespace snmpwrap::test
