// gmock mock of IFileWriter (IFileWriter.hpp).
#pragma once

#include <gmock/gmock.h>

#include <filesystem>
#include <string>

#include "IFileWriter.hpp"

namespace snmpwrap::mibgen::test {

class MockFileWriter : public IFileWriter {
public:
    MOCK_METHOD(bool, write, (const std::filesystem::path&, const std::string&), (override));
};

}  // namespace snmpwrap::mibgen::test
