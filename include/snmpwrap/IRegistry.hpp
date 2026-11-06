/**
 * @file IRegistry.hpp
 * @brief The interface for registering OID subtrees - implemented by Agent; mock it in tests.
 */
#pragma once

#include <memory>

#include "snmpwrap/Oid.hpp"

namespace snmpwrap {

class IHandler;
class Mib;

/**
 * @brief Where OID subtrees are registered for SNMP access, independent of how requests reach them.
 *
 * Implemented by Agent. The generated DataAgent and registerMib() take it, so their tests can pass a mock:
 * @code
 * class MockRegistry : public snmpwrap::IRegistry {
 * public:
 *     MOCK_METHOD(void, addHandler, (const snmpwrap::Oid&, std::shared_ptr<snmpwrap::IHandler>), (override));
 *     MOCK_METHOD(std::shared_ptr<snmpwrap::Mib>, addMib, (const snmpwrap::Oid&), (override));
 *     MOCK_METHOD(void, removeHandler, (const snmpwrap::Oid&), (override));
 *     MOCK_METHOD(void, removeMib, (const snmpwrap::Mib&), (override));
 * };
 * @endcode
 */
class IRegistry {
public:
    virtual ~IRegistry() = default;

    /// @brief Registers @p handler for the subtree @p root. @throws Error (null handler, empty root, already registered).
    virtual void addHandler(const Oid& root, std::shared_ptr<IHandler> handler) = 0;
    /// @brief Creates and registers a Mib below @p root. @return The Mib (shared with the registry). @throws Error as addHandler().
    virtual std::shared_ptr<Mib> addMib(const Oid& root) = 0;
    /// @brief Removes the handler registered at @p root. @throws Error if none is registered there.
    virtual void removeHandler(const Oid& root) = 0;
    /// @brief Removes a Mib created by addMib(). @throws Error if @p mib was not created here or is removed already.
    virtual void removeMib(const Mib& mib) = 0;

protected:
    IRegistry() = default;
    IRegistry(const IRegistry&) = default;
    IRegistry& operator=(const IRegistry&) = default;
};

}  // namespace snmpwrap
