/**
 * @file MibModel.hpp
 * @brief Reads MIB files (with Net-SNMP's MIB parser) into a C++ model of the interface description.
 *
 * The MibModel is the basis of the code generator `snmpwrap-mibgen`, which generates typed C++ code
 * from it at build time. Tools can also use it to resolve names, format values and parse values
 * (resolve(), format(), parseValue()), e.g. `client_cli -m`.
 *
 * @code
 * MibModel model = MibModel::load({"mibs/SNMPWRAPPER-TEST-MIB.txt"});
 * const MibNode& limit = model.node("swtLimit");
 * // limit.oid = 1.3.6.1.4.1.99999.1.4, limit.type = Type::Integer, limit.access = MibAccess::ReadWrite,
 * // limit.ranges = {{1, 100}}
 * Oid inst = model.resolve("SNMPWRAPPER-TEST-MIB::swtRowName.5");   // 1.3.6.1.4.1.99999.4.1.2.5
 * @endcode
 *
 * @note The model is an immutable copy; it does not depend on Net-SNMP data after load() returned and
 *       may be shared between threads. load() itself uses Net-SNMP's process-wide MIB tree and must not
 *       run concurrently with other Net-SNMP calls (load MIBs at program start).
 */
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "snmpwrap/Error.hpp"
#include "snmpwrap/IMibInfo.hpp"

namespace snmpwrap {

/**
 * @brief Immutable C++ model of one or more loaded MIB modules (plus everything they import).
 */
class MibModel final : public IMibInfo {
public:
    using IMibInfo::oid;

    /**
     * @brief Loads MIB files.
     * @param[in] files   MIB files to load. The directory of each file is added to the search path,
     *                    so imported modules lying next to it are found automatically.
     * @param[in] mibDirs Additional directories searched for imported modules. Net-SNMP's default MIB
     *                    directory (SNMPv2-SMI, SNMPv2-TC, …) is always searched.
     * @return The model, containing all nodes Net-SNMP knows after loading (incl. imported modules).
     * @throws Error if a file is missing, has syntax errors or imports a module that cannot be found;
     *         the message contains Net-SNMP's diagnostics.
     */
    static MibModel load(const std::vector<std::string>& files, const std::vector<std::string>& mibDirs = {});

    /**
     * @brief Looks up a node by name.
     * @param[in] name "descriptor" or "MODULE::descriptor".
     * @return The node, or nullptr if unknown.
     * @throws Error if a plain descriptor is defined in several modules (use "MODULE::descriptor").
     */
    const MibNode* find(std::string_view name) const override;

    /**
     * @brief Like find(), but the node must exist.
     * @param[in] name "descriptor" or "MODULE::descriptor".
     * @return The node.
     * @throws Error if unknown or ambiguous.
     */
    const MibNode& node(std::string_view name) const override;

    /**
     * @brief OID of a named node.
     * @param[in] name "descriptor" or "MODULE::descriptor".
     * @return Its object OID.
     * @throws Error if unknown or ambiguous.
     */

    /**
     * @brief Node with exactly this OID.
     * @param[in] oid Object OID.
     * @return The node or nullptr.
     */
    const MibNode* findByOid(const Oid& oid) const override;

    /**
     * @brief Deepest known node that is a prefix of @p oid (e.g. the column of a cell OID).
     * @param[in] oid Any OID, typically an instance OID.
     * @return The node or nullptr.
     */
    const MibNode* nodeFor(const Oid& oid) const override;

    /**
     * @brief Converts a symbolic instance name to an OID.
     * @param[in] text Numeric ("1.3.6.1.2.1.1.1.0"), "name.0", "MODULE::name.1.2" or with string index
     *                 parts: `"..."` (length-prefixed) and `'...'` (IMPLIED), e.g. `swtConnState.10.0.0.1.80.'web'`.
     * @return The OID.
     * @throws Error on unknown names or malformed text.
     */
    Oid resolve(std::string_view text) const override;

    /**
     * @brief Nodes of a module in OID order.
     * @param[in] module Module name.
     * @return The nodes defined by that module.
     */
    std::vector<const MibNode*> objects(std::string_view module) const override;

    /**
     * @brief The Entry node of a table.
     * @param[in] table A node of kind Table (or Entry, which is returned as is).
     * @return The entry node.
     * @throws Error if @p table is no table.
     */
    const MibNode& entryOf(const MibNode& table) const override;

    /**
     * @brief All columns of a table (including not-accessible index columns), ordered by column number.
     * @param[in] table Table or Entry node.
     * @return The column nodes.
     * @throws Error if @p table is no table.
     */
    std::vector<const MibNode*> columns(const MibNode& table) const override;

    /**
     * @brief Index layout of a table, derived from its INDEX / AUGMENTS clause.
     * @param[in] table Table or Entry node.
     * @return One IndexSpec per index column, ready for TableDef::indexes / encodeIndex().
     * @throws Error if the table or an index object is unknown or has an unsupported type.
     */
    std::vector<IndexSpec> indexSpecs(const MibNode& table) const override;

    /**
     * @brief Checks a value against the SYNTAX of a node (type, enumeration, ranges, SIZE).
     * @param[in] node  Scalar or Column node.
     * @param[in] value Value to check.
     * @throws SetError with WrongType, WrongValue (range / enum) or WrongLength (SIZE).
     */
    void validate(const MibNode& node, const Value& value) const override;

    /**
     * @brief Formats a varbind the way humans like to read it.
     * @param[in] vb The varbind.
     * @return E.g. `swtRowStatus.5 = active(1)` or `sysDescr.0 = "Linux host"`; unknown OIDs stay numeric.
     */
    std::string format(const VarBind& vb) const override;

    /**
     * @brief Formats a value using the node's enumeration, TC and UNITS.
     * @param[in] node  The object (may be nullptr: generic formatting).
     * @param[in] value The value.
     * @return E.g. `active(1)`, `"eth0"`, `42 seconds`.
     */
    std::string formatValue(const MibNode* node, const Value& value) const override;

    /**
     * @brief Parses text into a value of the node's type and validates it.
     * @param[in] name Object or instance name (see resolve()), e.g. "swtLimit" or "swtRowStatus.5".
     * @param[in] text Value text: number, enumeration label ("active" or "active(1)"), string,
     *                 dotted IpAddress or OID (numeric or symbolic).
     * @return The typed value.
     * @throws Error if the name is unknown or the text cannot be parsed; SetError if the value violates the SYNTAX.
     */
    Value parseValue(std::string_view name, std::string_view text) const override;

    /**
     * @brief Parses text into a value for a node (see parseValue(std::string_view, std::string_view)).
     * @param[in] node The object.
     * @param[in] text Value text.
     * @return The typed, validated value.
     * @throws Error / SetError as above.
     */
    Value parseValue(const MibNode& node, std::string_view text) const override;

private:
    struct Data;
    explicit MibModel(std::shared_ptr<const Data> d) : d_(std::move(d)) {}
    static std::shared_ptr<Data> collect(std::vector<std::string> modules);  // copies Net-SNMP's MIB tree
    std::shared_ptr<const Data> d_;
};

}  // namespace snmpwrap
