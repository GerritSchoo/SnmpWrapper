// gmock mock of IRegistry (IRegistry.hpp).
#pragma once

#include <gmock/gmock.h>

#include <memory>

#include "snmpwrap/IRegistry.hpp"
#include "snmpwrap/Mib.hpp"

namespace snmpwrap::test {

class MockRegistry : public IRegistry {
public:
    MOCK_METHOD(void, addHandler, (const Oid&, std::shared_ptr<IHandler>), (override));
    MOCK_METHOD(std::shared_ptr<Mib>, addMib, (const Oid&), (override));
    MOCK_METHOD(void, removeHandler, (const Oid&), (override));
    MOCK_METHOD(void, removeMib, (const Mib&), (override));
};

}  // namespace snmpwrap::test
