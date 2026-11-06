// gmock mock of IMibInfo (IMibInfo.hpp).
#pragma once

#include <gmock/gmock.h>

#include <string>
#include <string_view>
#include <vector>

#include "snmpwrap/IMibInfo.hpp"

namespace snmpwrap::test {

class MockMibInfo : public IMibInfo {
public:
    MOCK_METHOD(const MibNode*, find, (std::string_view), (const, override));
    MOCK_METHOD(const MibNode&, node, (std::string_view), (const, override));
    MOCK_METHOD(const MibNode*, findByOid, (const Oid&), (const, override));
    MOCK_METHOD(const MibNode*, nodeFor, (const Oid&), (const, override));
    MOCK_METHOD(Oid, resolve, (std::string_view), (const, override));
    MOCK_METHOD(std::vector<const MibNode*>, objects, (std::string_view), (const, override));
    MOCK_METHOD(const MibNode&, entryOf, (const MibNode&), (const, override));
    MOCK_METHOD(std::vector<const MibNode*>, columns, (const MibNode&), (const, override));
    MOCK_METHOD(std::vector<IndexSpec>, indexSpecs, (const MibNode&), (const, override));
    MOCK_METHOD(void, validate, (const MibNode&, const Value&), (const, override));
    MOCK_METHOD(std::string, format, (const VarBind&), (const, override));
    MOCK_METHOD(std::string, formatValue, (const MibNode*, const Value&), (const, override));
    MOCK_METHOD(Value, parseValue, (std::string_view, std::string_view), (const, override));
    MOCK_METHOD(Value, parseValue, (const MibNode&, std::string_view), (const, override));
};

}  // namespace snmpwrap::test
