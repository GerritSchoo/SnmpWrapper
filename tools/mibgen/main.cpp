// snmpwrap-mibgen – generates typed C++17 code (agent interface, registration, notifications,
// typed client) for one MIB module. The command line is described and implemented in Cli.hpp.

#include <iostream>
#include <string>
#include <vector>

#include "Cli.hpp"

int main(int argc, char** argv) {
    snmpwrap::mibgen::Cli cli(std::cerr);
    return cli.run(std::vector<std::string>(argv + 1, argv + argc));
}
