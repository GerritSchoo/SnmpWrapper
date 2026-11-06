/**
 * @file IMibInfo.hpp
 * @brief Interface IMibInfo (what is known about loaded MIB modules) and the data it returns: MibNode, MibAccess,
 *        MibNodeKind, MibRange, MibEnum, MibIndexPart - implemented by MibModel; mock it in tests.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "snmpwrap/Index.hpp"
#include "snmpwrap/Oid.hpp"
#include "snmpwrap/Value.hpp"

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
 * @brief What is known about loaded MIB modules: nodes, tables, ranges, formatting - implemented by MibModel;
 *        mock it in tests. The code generator only uses this interface.
 */
class IMibInfo {
public:
    virtual ~IMibInfo() = default;

    /**
     * @brief Looks up a node by name.
     * @param[in] name "descriptor" or "MODULE::descriptor".
     * @return The node, or nullptr if unknown.
     * @throws Error if a plain descriptor is defined in several modules (use "MODULE::descriptor").
     */
    virtual const MibNode* find(std::string_view name) const = 0;

    /**
     * @brief Like find(), but the node must exist.
     * @param[in] name "descriptor" or "MODULE::descriptor".
     * @return The node.
     * @throws Error if unknown or ambiguous.
     */
    virtual const MibNode& node(std::string_view name) const = 0;

    /**
     * @brief OID of a named node.
     * @param[in] name "descriptor" or "MODULE::descriptor".
     * @return Its object OID.
     * @throws Error if unknown or ambiguous.
     */
    Oid oid(std::string_view name) const { return node(name).oid; }  // not virtual: always node(name).oid

    /**
     * @brief Node with exactly this OID.
     * @param[in] oid Object OID.
     * @return The node or nullptr.
     */
    virtual const MibNode* findByOid(const Oid& oid) const = 0;

    /**
     * @brief Deepest known node that is a prefix of @p oid (e.g. the column of a cell OID).
     * @param[in] oid Any OID, typically an instance OID.
     * @return The node or nullptr.
     */
    virtual const MibNode* nodeFor(const Oid& oid) const = 0;

    /**
     * @brief Converts a symbolic instance name to an OID.
     * @param[in] text Numeric ("1.3.6.1.2.1.1.1.0"), "name.0", "MODULE::name.1.2" or with string index
     *                 parts: `"..."` (length-prefixed) and `'...'` (IMPLIED), e.g. `swtConnState.10.0.0.1.80.'web'`.
     * @return The OID.
     * @throws Error on unknown names or malformed text.
     */
    virtual Oid resolve(std::string_view text) const = 0;

    /**
     * @brief Nodes of a module in OID order.
     * @param[in] module Module name.
     * @return The nodes defined by that module.
     */
    virtual std::vector<const MibNode*> objects(std::string_view module) const = 0;

    /**
     * @brief The Entry node of a table.
     * @param[in] table A node of kind Table (or Entry, which is returned as is).
     * @return The entry node.
     * @throws Error if @p table is no table.
     */
    virtual const MibNode& entryOf(const MibNode& table) const = 0;

    /**
     * @brief All columns of a table (including not-accessible index columns), ordered by column number.
     * @param[in] table Table or Entry node.
     * @return The column nodes.
     * @throws Error if @p table is no table.
     */
    virtual std::vector<const MibNode*> columns(const MibNode& table) const = 0;

    /**
     * @brief Index layout of a table, derived from its INDEX / AUGMENTS clause.
     * @param[in] table Table or Entry node.
     * @return One IndexSpec per index column, ready for TableDef::indexes / encodeIndex().
     * @throws Error if the table or an index object is unknown or has an unsupported type.
     */
    virtual std::vector<IndexSpec> indexSpecs(const MibNode& table) const = 0;

    /**
     * @brief Checks a value against the SYNTAX of a node (type, enumeration, ranges, SIZE).
     * @param[in] node  Scalar or Column node.
     * @param[in] value Value to check.
     * @throws SetError with WrongType, WrongValue (range / enum) or WrongLength (SIZE).
     */
    virtual void validate(const MibNode& node, const Value& value) const = 0;

    /**
     * @brief Formats a varbind the way humans like to read it.
     * @param[in] vb The varbind.
     * @return E.g. `swtRowStatus.5 = active(1)` or `sysDescr.0 = "Linux host"`; unknown OIDs stay numeric.
     */
    virtual std::string format(const VarBind& vb) const = 0;

    /**
     * @brief Formats a value using the node's enumeration, TC and UNITS.
     * @param[in] node  The object (may be nullptr: generic formatting).
     * @param[in] value The value.
     * @return E.g. `active(1)`, `"eth0"`, `42 seconds`.
     */
    virtual std::string formatValue(const MibNode* node, const Value& value) const = 0;

    /**
     * @brief Parses text into a value of the node's type and validates it.
     * @param[in] name Object or instance name (see resolve()), e.g. "swtLimit" or "swtRowStatus.5".
     * @param[in] text Value text: number, enumeration label ("active" or "active(1)"), string,
     *                 dotted IpAddress or OID (numeric or symbolic).
     * @return The typed value.
     * @throws Error if the name is unknown or the text cannot be parsed; SetError if the value violates the SYNTAX.
     */
    virtual Value parseValue(std::string_view name, std::string_view text) const = 0;

    /**
     * @brief Parses text into a value for a node (see parseValue(std::string_view, std::string_view)).
     * @param[in] node The object.
     * @param[in] text Value text.
     * @return The typed, validated value.
     * @throws Error / SetError as above.
     */
    virtual Value parseValue(const MibNode& node, std::string_view text) const = 0;

protected:
    IMibInfo() = default;
    IMibInfo(const IMibInfo&) = default;
    IMibInfo& operator=(const IMibInfo&) = default;
};


/// @brief Name of an access value, e.g. "read-write". @param[in] a The access. @return Static string.
const char* toString(MibAccess a) noexcept;
/// @brief Name of a node kind, e.g. "Column". @param[in] k The kind. @return Static string.
const char* toString(MibNodeKind k) noexcept;

}  // namespace snmpwrap
