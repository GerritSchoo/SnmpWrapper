/**
 * @file mib_model.hpp
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

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "snmpwrap/error.hpp"
#include "snmpwrap/index.hpp"
#include "snmpwrap/oid.hpp"
#include "snmpwrap/value.hpp"

namespace snmpwrap {

/// @brief MAX-ACCESS of a MIB object.
enum class MibAccess {
    NotAccessible,        ///< not-accessible (index columns, tables, entries).
    AccessibleForNotify,  ///< accessible-for-notify (only in notifications).
    ReadOnly,             ///< read-only.
    ReadWrite,            ///< read-write.
    ReadCreate,           ///< read-create (columns of tables with RowStatus).
};

/// @brief What a MIB node is.
enum class MibNodeKind {
    Other,         ///< OBJECT IDENTIFIER / MODULE-IDENTITY / groups / anything without data.
    Scalar,        ///< OBJECT-TYPE outside a table (instance `<oid>.0`).
    Table,         ///< SEQUENCE OF … (`xxxTable`).
    Entry,         ///< the row object of a table (`xxxEntry`), carries INDEX / AUGMENTS.
    Column,        ///< OBJECT-TYPE below an Entry.
    Notification,  ///< NOTIFICATION-TYPE (or SMIv1 TRAP-TYPE).
};

/// @brief One range of a SYNTAX restriction: value range for numbers, SIZE range for strings.
struct MibRange {
    std::int64_t low = 0;   ///< Lower bound (inclusive).
    std::int64_t high = 0;  ///< Upper bound (inclusive).
};

/// @brief One named number of an enumeration, e.g. `active(1)`.
struct MibEnum {
    std::int32_t value = 0;  ///< The number.
    std::string label;       ///< The name.
};

/// @brief One column of an INDEX clause.
struct MibIndexPart {
    std::string name;      ///< Name of the index object, e.g. "swtConnAddr".
    bool implied = false;  ///< Declared `IMPLIED`.
};

/**
 * @brief Everything the MIB says about one node.
 */
struct MibNode {
    std::string name;               ///< Descriptor, e.g. "swtRowStatus".
    std::string module;             ///< Defining module, e.g. "SNMPWRAPPER-TEST-MIB".
    Oid oid;                        ///< Object OID (without instance suffix).
    MibNodeKind kind = MibNodeKind::Other;  ///< Classification.
    Type type = Type::Null;         ///< Value type (Scalar / Column); Type::Null for other kinds.
    MibAccess access = MibAccess::NotAccessible;  ///< MAX-ACCESS.
    std::string textualConvention;  ///< TC of the SYNTAX, e.g. "RowStatus", "DisplayString"; empty if none.
    std::vector<MibRange> ranges;   ///< Value ranges (numbers) or SIZE ranges (strings); empty = unrestricted.
    std::vector<MibEnum> enums;     ///< Named numbers, sorted by value; empty if none.
    std::vector<MibIndexPart> index;   ///< INDEX clause (Entry only; resolved for AUGMENTS entries).
    std::string augments;           ///< AUGMENTS target entry (Entry only).
    std::vector<std::string> objects;  ///< OBJECTS of a notification.
    std::string displayHint;        ///< DISPLAY-HINT of the TC, if any.
    std::string units;              ///< UNITS clause.
    std::string description;        ///< DESCRIPTION text.
    std::string defaultValue;       ///< DEFVAL as written in the MIB (unparsed), empty if none.

    /// @brief Readable by managers. @return True for ReadOnly, ReadWrite and ReadCreate.
    bool readable() const noexcept {
        return access == MibAccess::ReadOnly || access == MibAccess::ReadWrite || access == MibAccess::ReadCreate;
    }
    /// @brief Writable by managers. @return True for ReadWrite and ReadCreate.
    bool writable() const noexcept { return access == MibAccess::ReadWrite || access == MibAccess::ReadCreate; }
};

/**
 * @brief Immutable C++ model of one or more loaded MIB modules (plus everything they import).
 */
class MibModel {
public:
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
     * @brief Loads MIB modules by module name from the search path.
     * @param[in] modules Module names, e.g. {"IF-MIB"}.
     * @param[in] mibDirs Additional directories to search.
     * @return The model.
     * @throws Error as load().
     */
    static MibModel loadModules(const std::vector<std::string>& modules, const std::vector<std::string>& mibDirs = {});

    /// @brief The modules that were explicitly loaded. @return Module names in load order.
    const std::vector<std::string>& modules() const noexcept;

    /// @brief All nodes. @return All known nodes, sorted by OID.
    const std::vector<MibNode>& nodes() const noexcept;

    /**
     * @brief Looks up a node by name.
     * @param[in] name "descriptor" or "MODULE::descriptor".
     * @return The node, or nullptr if unknown.
     * @throws Error if a plain descriptor is defined in several modules (use "MODULE::descriptor").
     */
    const MibNode* find(std::string_view name) const;

    /**
     * @brief Like find(), but the node must exist.
     * @param[in] name "descriptor" or "MODULE::descriptor".
     * @return The node.
     * @throws Error if unknown or ambiguous.
     */
    const MibNode& node(std::string_view name) const;

    /**
     * @brief OID of a named node.
     * @param[in] name "descriptor" or "MODULE::descriptor".
     * @return Its object OID.
     * @throws Error if unknown or ambiguous.
     */
    Oid oid(std::string_view name) const { return node(name).oid; }

    /**
     * @brief Node with exactly this OID.
     * @param[in] oid Object OID.
     * @return The node or nullptr.
     */
    const MibNode* findByOid(const Oid& oid) const;

    /**
     * @brief Deepest known node that is a prefix of @p oid (e.g. the column of a cell OID).
     * @param[in] oid Any OID, typically an instance OID.
     * @return The node or nullptr.
     */
    const MibNode* nodeFor(const Oid& oid) const;

    /**
     * @brief Converts a symbolic instance name to an OID.
     * @param[in] text Numeric ("1.3.6.1.2.1.1.1.0"), "name.0", "MODULE::name.1.2" or with string index
     *                 parts: `"..."` (length-prefixed) and `'...'` (IMPLIED), e.g. `swtConnState.10.0.0.1.80.'web'`.
     * @return The OID.
     * @throws Error on unknown names or malformed text.
     */
    Oid resolve(std::string_view text) const;

    /**
     * @brief Nodes of a module in OID order.
     * @param[in] module Module name.
     * @return The nodes defined by that module.
     */
    std::vector<const MibNode*> objects(std::string_view module) const;

    /**
     * @brief The Entry node of a table.
     * @param[in] table A node of kind Table (or Entry, which is returned as is).
     * @return The entry node.
     * @throws Error if @p table is no table.
     */
    const MibNode& entryOf(const MibNode& table) const;

    /**
     * @brief All columns of a table (including not-accessible index columns), ordered by column number.
     * @param[in] table Table or Entry node.
     * @return The column nodes.
     * @throws Error if @p table is no table.
     */
    std::vector<const MibNode*> columns(const MibNode& table) const;

    /**
     * @brief Index layout of a table, derived from its INDEX / AUGMENTS clause.
     * @param[in] table Table or Entry node.
     * @return One IndexSpec per index column, ready for TableDef::indexes / encodeIndex().
     * @throws Error if the table or an index object is unknown or has an unsupported type.
     */
    std::vector<IndexSpec> indexSpecs(const MibNode& table) const;

    /**
     * @brief Checks a value against the SYNTAX of a node (type, enumeration, ranges, SIZE).
     * @param[in] node  Scalar or Column node.
     * @param[in] value Value to check.
     * @throws SetError with WrongType, WrongValue (range / enum) or WrongLength (SIZE).
     */
    void validate(const MibNode& node, const Value& value) const;

    /**
     * @brief Formats a varbind the way humans like to read it.
     * @param[in] vb The varbind.
     * @return E.g. `swtRowStatus.5 = active(1)` or `sysDescr.0 = "Linux host"`; unknown OIDs stay numeric.
     */
    std::string format(const VarBind& vb) const;

    /**
     * @brief Formats a value using the node's enumeration, TC and UNITS.
     * @param[in] node  The object (may be nullptr: generic formatting).
     * @param[in] value The value.
     * @return E.g. `active(1)`, `"eth0"`, `42 seconds`.
     */
    std::string formatValue(const MibNode* node, const Value& value) const;

    /**
     * @brief Parses text into a value of the node's type and validates it.
     * @param[in] name Object or instance name (see resolve()), e.g. "swtLimit" or "swtRowStatus.5".
     * @param[in] text Value text: number, enumeration label ("active" or "active(1)"), string,
     *                 dotted IpAddress or OID (numeric or symbolic).
     * @return The typed value.
     * @throws Error if the name is unknown or the text cannot be parsed; SetError if the value violates the SYNTAX.
     */
    Value parseValue(std::string_view name, std::string_view text) const;

    /**
     * @brief Parses text into a value for a node (see parseValue(std::string_view, std::string_view)).
     * @param[in] node The object.
     * @param[in] text Value text.
     * @return The typed, validated value.
     * @throws Error / SetError as above.
     */
    Value parseValue(const MibNode& node, std::string_view text) const;

private:
    struct Data;
    explicit MibModel(std::shared_ptr<const Data> d) : d_(std::move(d)) {}
    static std::shared_ptr<Data> collect(std::vector<std::string> modules);  // copies Net-SNMP's MIB tree
    std::shared_ptr<const Data> d_;
};

/// @brief Name of an access value, e.g. "read-write". @param[in] a The access. @return Static string.
const char* toString(MibAccess a) noexcept;
/// @brief Name of a node kind, e.g. "Column". @param[in] k The kind. @return Static string.
const char* toString(MibNodeKind k) noexcept;

}  // namespace snmpwrap
