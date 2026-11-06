// gmock mock of ICodeGenerator (ICodeGenerator.hpp).
#pragma once

#include <gmock/gmock.h>

#include "ICodeGenerator.hpp"

namespace snmpwrap::mibgen::test {

class MockCodeGenerator : public ICodeGenerator {
public:
    MOCK_METHOD(Output, generate, (const IMibInfo&, const Options&), (const, override));
};

}  // namespace snmpwrap::mibgen::test
