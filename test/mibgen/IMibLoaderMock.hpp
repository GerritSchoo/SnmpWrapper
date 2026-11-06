// gmock mock of IMibLoader (IMibLoader.hpp).
#pragma once

#include <gmock/gmock.h>

#include <memory>
#include <string>
#include <vector>

#include "IMibLoader.hpp"

namespace snmpwrap::mibgen::test {

class MockMibLoader : public IMibLoader {
public:
    MOCK_METHOD(std::unique_ptr<IMibInfo>, load, (const std::vector<std::string>&, const std::vector<std::string>&), (const, override));
};

}  // namespace snmpwrap::mibgen::test
