// Unit tests for the pure C++ parts (Oid, Value, index codec, Mib incl. RowStatus and SET transactions).
// No snmpd needed. Deliberately dependency-free: tiny CHECK macros instead of a test framework.

#include <iostream>
#include <map>
#include <string>

#include "snmpwrap/mib.hpp"

using namespace snmpwrap;

namespace {

int g_failures = 0;
int g_checks = 0;

// variadic so that braced Oid{1, 2} lists (commas outside parentheses) survive the preprocessor
#define CHECK(...)                                                               \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(__VA_ARGS__)) {                                                    \
            ++g_failures;                                                        \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #__VA_ARGS__ "\n"; \
        }                                                                        \
    } while (0)

#define CHECK_THROWS(expr, ExcType)                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        bool thrown = false;                                                     \
        try { expr; } catch (const ExcType&) { thrown = true; }                  \
        if (!thrown) {                                                           \
            ++g_failures;                                                        \
            std::cerr << __FILE__ << ":" << __LINE__ << ": expected " #ExcType " from " #expr "\n"; \
        }                                                                        \
    } while (0)

const Oid kRoot = Oid::parse("1.3.6.1.4.1.99999");

/// Runs a whole SET like the agent does: prepare -> apply -> commit, or undo after a failed apply.
/// Returns the SetError of whichever phase failed.
std::optional<SetError> trySet(Handler& h, const std::vector<VarBind>& sets) {
    std::unique_ptr<SetTransaction> t;
    try {
        t = h.prepare(sets);
    } catch (const SetError& e) {
        return e;
    }
    try {
        t->apply();
    } catch (const SetError& e) {
        t->undo();
        return e;
    }
    t->commit();
    return std::nullopt;
}

bool failsWith(const std::optional<SetError>& e, ErrorStatus status, std::size_t index) {
    return e && e->status() == status && e->index() == index;
}

// ---------------------------------------------------------------------------------------------
void testOid() {
    CHECK(Oid::parse("1.3.6.1").str() == "1.3.6.1");
    CHECK(Oid::parse(".1.3.6.1") == Oid({1, 3, 6, 1}));
    CHECK_THROWS(Oid::parse(""), Error);
    CHECK_THROWS(Oid::parse("1..3"), Error);
    CHECK_THROWS(Oid::parse("1.3."), Error);
    CHECK_THROWS(Oid::parse("1.a"), Error);
    CHECK_THROWS(Oid::parse("1.4294967296"), Error);
    CHECK(Oid::parse("1.4294967295")[1] == 4294967295u);

    // GETNEXT order is lexicographic by sub-id, not by string
    CHECK(Oid::parse("1.3.6.1.2") < Oid::parse("1.3.6.1.10"));
    CHECK(Oid::parse("1.3.6") < Oid::parse("1.3.6.0"));
    CHECK(Oid::parse("1.3.6.1").isPrefixOf(Oid::parse("1.3.6.1.4")));
    CHECK(Oid::parse("1.3.6.1").isPrefixOf(Oid::parse("1.3.6.1")));
    CHECK(!Oid::parse("1.3.6.2").isPrefixOf(Oid::parse("1.3.6.1.4")));
    CHECK(Oid::parse("1.3").suffixOf(Oid::parse("1.3.6.1")) == Oid({6, 1}));

    CHECK(indexString("ab") == Oid({2, 'a', 'b'}));
    CHECK(indexString("ab", true) == Oid({'a', 'b'}));
    CHECK(indexIp(10, 0, 0, 1) == Oid({10, 0, 0, 1}));
}

void testValue() {
    CHECK(Value::integer(-5).asInt() == -5);
    CHECK(Value::gauge(7).asUInt() == 7u);
    CHECK(Value::counter64(0x1122334455667788ULL).asUInt64() == 0x1122334455667788ULL);
    CHECK(Value::integer(3).asUInt() == 3u);
    CHECK_THROWS(Value::integer(-1).asUInt(), Error);
    CHECK_THROWS(Value::string("x").asInt(), Error);
    CHECK(Value::string("abc").asString() == "abc");
    CHECK(Value::integer(1) != Value::gauge(1));
    CHECK(Value::integer(1) == Value::integer(1));
    CHECK(Value::exception(Type::NoSuchInstance).isException());
    CHECK(!Value::null().isException());

    // Opaque and BITS keep their own type but share the string accessor
    CHECK(Value::opaque("ab").type() == Type::Opaque && Value::opaque("ab").asString() == "ab");
    CHECK(Value::bits(std::string("\xA0", 1)).type() == Type::Bits);
    CHECK(Value::opaque("x") != Value::string("x"));
    CHECK(Value::bits("x") != Value::opaque("x"));
    CHECK(std::string(toString(Type::Opaque)) == "Opaque" && std::string(toString(Type::Bits)) == "Bits");
    CHECK(Value::bits(std::string("\xA0", 1)).str() == "Bits (hex): A0");
}

// ---------------------------------------------------------------------------------------------
// Index codec
// ---------------------------------------------------------------------------------------------
void testIndexCodec() {
    const std::vector<IndexSpec> multi{IndexSpec::ipAddress(), IndexSpec::integer(), IndexSpec::impliedString()};
    const Oid enc = encodeIndex(multi, {Value::ipAddress(10, 0, 0, 1), Value::integer(80), Value::string("web")});
    CHECK(enc == Oid({10, 0, 0, 1, 80, 'w', 'e', 'b'}));
    auto dec = decodeIndex(multi, enc);
    CHECK(dec && dec->size() == 3);
    CHECK(dec && (*dec)[0] == Value::ipAddress(10, 0, 0, 1) && (*dec)[1] == Value::integer(80) && (*dec)[2] == Value::string("web"));

    // implied string may be empty; the empty tail is still a complete index
    CHECK(decodeIndex(multi, Oid({10, 0, 0, 1, 80}))->at(2) == Value::string(""));
    // malformed: too short, octet out of range, ...
    CHECK(!decodeIndex(multi, Oid({10, 0, 0})));
    CHECK(!decodeIndex(multi, Oid({10, 0, 256, 1, 80})));
    CHECK(!decodeIndex(multi, Oid({10, 0, 0, 1, 80, 300})));

    // length-prefixed string: exact consumption
    const std::vector<IndexSpec> str{IndexSpec::string(), IndexSpec::integer()};
    CHECK(encodeIndex(str, {Value::string("ab"), Value::integer(7)}) == Oid({2, 'a', 'b', 7}));
    CHECK(decodeIndex(str, Oid({2, 'a', 'b', 7}))->at(0) == Value::string("ab"));
    CHECK(decodeIndex(str, Oid({0, 7}))->at(0) == Value::string(""));  // empty string
    CHECK(!decodeIndex(str, Oid({3, 'a', 'b', 7})));                    // length runs past the end
    CHECK(!decodeIndex(str, Oid({2, 'a', 'b', 7, 9})));                 // trailing sub-id
    CHECK(!decodeIndex(str, Oid({2, 'a', 'b'})));                       // integer missing
    CHECK(!decodeIndex(str, Oid{}));

    // an Unsigned index accepts Gauge32, Counter32 and TimeTicks values
    CHECK(encodeIndex({IndexSpec::unsignedInt()}, {Value::timeTicks(5)}) == Oid({5}));
    CHECK(encodeIndex({IndexSpec::unsignedInt()}, {Value::counter32(6)}) == Oid({6}));
    CHECK_THROWS(encodeIndex({IndexSpec::unsignedInt()}, {Value::integer(1)}), Error);

    // 255 is the largest octet
    CHECK(decodeIndex({IndexSpec::string()}, Oid({1, 255})).has_value());
    CHECK(!decodeIndex({IndexSpec::string()}, Oid({1, 256})));

    // fixed size, object id, negative / unsigned integers
    CHECK(decodeIndex({IndexSpec::fixedString(2)}, Oid({'a', 'b'}))->at(0) == Value::string("ab"));
    CHECK(!decodeIndex({IndexSpec::fixedString(2)}, Oid({'a'})));
    CHECK_THROWS(encodeIndex({IndexSpec::fixedString(2)}, {Value::string("abc")}), Error);
    const std::vector<IndexSpec> oidSpec{IndexSpec::objectId(), IndexSpec::integer()};
    CHECK(encodeIndex(oidSpec, {Value::oid(Oid({1, 3, 6})), Value::integer(1)}) == Oid({3, 1, 3, 6, 1}));
    CHECK(decodeIndex(oidSpec, Oid({3, 1, 3, 6, 1}))->at(0) == Value::oid(Oid({1, 3, 6})));
    CHECK(decodeIndex({IndexSpec::integer()}, Oid({4294967295u}))->at(0) == Value::integer(-1));
    CHECK(encodeIndex({IndexSpec::integer()}, {Value::integer(-1)}) == Oid({4294967295u}));
    CHECK(decodeIndex({IndexSpec::unsignedInt()}, Oid({4294967295u}))->at(0) == Value::gauge(4294967295u));

    // IMPLIED is only legal as the last column
    CHECK_THROWS(decodeIndex({IndexSpec::impliedString(), IndexSpec::integer()}, Oid({1})), Error);
    CHECK_THROWS(encodeIndex({IndexSpec::integer()}, {}), Error);  // count mismatch
}

// ---------------------------------------------------------------------------------------------
// Mib: scalars + legacy-index table (no RowStatus)
// ---------------------------------------------------------------------------------------------
struct Fixture {
    std::string name = "n";
    int limit = 10;
    std::map<std::uint32_t, std::pair<std::string, std::uint32_t>> rows{{1, {"a", 10}}, {2, {"b", 20}}, {10, {"c", 30}}};

    Mib mib{kRoot};

    Fixture() {
        mib.scalar({1, 1}, {Type::OctetString, [this] { return Value::string(name); },
                            [this](const Value& v) { name = v.asString(); }});
        mib.scalar({1, 2}, {Type::Integer, [this] { return Value::integer(limit); },
                            [this](const Value& v) { limit = v.asInt(); },
                            [](const Value& v) {
                                if (v.asInt() > 100) throw SetError(ErrorStatus::WrongValue);
                            }});
        mib.scalar({1, 3}, {Type::Counter32, [] { return Value::counter32(1); }});  // read-only

        TableDef t;
        t.columns = {{2, Type::OctetString, Access::ReadWrite}, {3, Type::Gauge32, Access::ReadOnly}};
        t.rows = [this] {
            std::vector<Oid> out;
            for (auto& [i, r] : rows) out.push_back(indexInt(i));
            return out;
        };
        t.get = [this](const Oid& idx, SubId col) {
            auto& r = rows.at(idx[0]);
            return col == 2 ? Value::string(r.first) : Value::gauge(r.second);
        };
        t.set = [this](const Oid& idx, SubId, const Value& v) {
            if (v.asString() == "commitfail") throw SetError(ErrorStatus::CommitFailed, "injected");
            rows.at(idx[0]).first = v.asString();
        };
        mib.table(2, std::move(t));
    }
};

void testMibGet() {
    Fixture f;
    CHECK(f.mib.get(kRoot + Oid{1, 1, 0}) == Value::string("n"));
    CHECK(f.mib.get(kRoot + Oid{1, 2, 0}) == Value::integer(10));
    CHECK(!f.mib.get(kRoot + Oid{1, 1}));     // no .0 instance suffix
    CHECK(!f.mib.get(kRoot + Oid{1, 9, 0}));  // unknown object
    CHECK(f.mib.get(kRoot + Oid{2, 1, 2, 1}) == Value::string("a"));
    CHECK(f.mib.get(kRoot + Oid{2, 1, 3, 10}) == Value::gauge(30));
    CHECK(!f.mib.get(kRoot + Oid{2, 1, 2, 3}));  // row 3 does not exist
    CHECK(!f.mib.get(kRoot + Oid{2, 1, 9, 1}));  // column 9 does not exist
    CHECK(!f.mib.get(Oid::parse("1.3.6.1.2.1.1.1.0")));
}

void testMissingSemantics() {
    Fixture f;
    // object exists, instance does not -> noSuchInstance
    CHECK(f.mib.missing(kRoot + Oid{1, 1, 5}) == Type::NoSuchInstance);   // scalar with wrong suffix
    CHECK(f.mib.missing(kRoot + Oid{1, 1}) == Type::NoSuchInstance);      // the scalar object itself
    CHECK(f.mib.missing(kRoot + Oid{2, 1, 2, 99}) == Type::NoSuchInstance);  // column exists, row does not
    CHECK(f.mib.missing(kRoot + Oid{2, 1, 2}) == Type::NoSuchInstance);   // column without index
    // no object matches -> noSuchObject
    CHECK(f.mib.missing(kRoot + Oid{1, 9, 0}) == Type::NoSuchObject);
    CHECK(f.mib.missing(kRoot + Oid{2, 1, 9, 1}) == Type::NoSuchObject);  // unknown column
    CHECK(f.mib.missing(kRoot + Oid{2, 2, 2, 1}) == Type::NoSuchObject);  // not the entry (.1)
    CHECK(f.mib.missing(kRoot + Oid{7}) == Type::NoSuchObject);
    CHECK(f.mib.missing(kRoot) == Type::NoSuchObject);
    CHECK(f.mib.missing(Oid::parse("1.3.6.1.2.1")) == Type::NoSuchObject);
}

void testMibGetNext() {
    Fixture f;
    // Full walk from before the root must yield strictly increasing OIDs, 3 scalars + 3 rows * 2 columns
    std::vector<Oid> seen;
    Oid cur = Oid::parse("1.3.6");
    while (auto nx = f.mib.getNext(cur)) {
        CHECK(nx->oid > cur);
        seen.push_back(nx->oid);
        cur = nx->oid;
    }
    CHECK(seen.size() == 9);
    CHECK(seen.front() == kRoot + Oid{1, 1, 0});
    CHECK(seen.back() == kRoot + Oid{2, 1, 3, 10});
    // every walked OID is gettable
    for (const Oid& o : seen) CHECK(f.mib.get(o).has_value());

    CHECK(f.mib.getNext(kRoot)->oid == kRoot + Oid{1, 1, 0});                      // exactly the root
    CHECK(f.mib.getNext(kRoot + Oid{1})->oid == kRoot + Oid{1, 1, 0});             // inside, before first scalar
    CHECK(f.mib.getNext(kRoot + Oid{1, 1})->oid == kRoot + Oid{1, 1, 0});          // the object OID itself
    CHECK(f.mib.getNext(kRoot + Oid{1, 1, 0})->oid == kRoot + Oid{1, 2, 0});
    CHECK(f.mib.getNext(kRoot + Oid{1, 1, 0, 5})->oid == kRoot + Oid{1, 2, 0});    // past the instance
    CHECK(f.mib.getNext(kRoot + Oid{1, 3, 0})->oid == kRoot + Oid{2, 1, 2, 1});    // scalar -> table
    CHECK(f.mib.getNext(kRoot + Oid{2})->oid == kRoot + Oid{2, 1, 2, 1});
    CHECK(f.mib.getNext(kRoot + Oid{2, 1})->oid == kRoot + Oid{2, 1, 2, 1});
    CHECK(f.mib.getNext(kRoot + Oid{2, 1, 2})->oid == kRoot + Oid{2, 1, 2, 1});
    CHECK(f.mib.getNext(kRoot + Oid{2, 1, 1, 5})->oid == kRoot + Oid{2, 1, 2, 1});  // column 1 does not exist
    // index 10 must sort after index 2 (numeric, not textual)
    CHECK(f.mib.getNext(kRoot + Oid{2, 1, 2, 2})->oid == kRoot + Oid{2, 1, 2, 10});
    // GETNEXT from the last row of a column jumps to the first row of the next column
    CHECK(f.mib.getNext(kRoot + Oid{2, 1, 2, 10})->oid == kRoot + Oid{2, 1, 3, 1});
    // from a non-existing instance between existing ones
    CHECK(f.mib.getNext(kRoot + Oid{2, 1, 2, 5})->oid == kRoot + Oid{2, 1, 2, 10});
    CHECK(f.mib.getNext(kRoot + Oid{2, 1, 2, 1, 7})->oid == kRoot + Oid{2, 1, 2, 2});  // longer than any index
    // end of the subtree, and positions outside it
    CHECK(!f.mib.getNext(kRoot + Oid{2, 1, 3, 10}));
    CHECK(!f.mib.getNext(kRoot + Oid{2, 1, 4}));  // column 4 does not exist, nothing after
    CHECK(!f.mib.getNext(kRoot + Oid{9}));
    CHECK(!f.mib.getNext(Oid::parse("1.3.6.1.4.1.100000")));
    CHECK(f.mib.getNext(Oid::parse("1.3.6.1.2.1.1.1.0"))->oid == kRoot + Oid{1, 1, 0});  // before the root
}

void testMibSet() {
    Fixture f;
    CHECK(!trySet(f.mib, {{kRoot + Oid{1, 1, 0}, Value::string("x")}}));
    CHECK(f.name == "x");

    CHECK(failsWith(trySet(f.mib, {{kRoot + Oid{1, 1, 0}, Value::integer(1)}}), ErrorStatus::WrongType, 0));
    CHECK(failsWith(trySet(f.mib, {{kRoot + Oid{1, 2, 0}, Value::integer(101)}}), ErrorStatus::WrongValue, 0));  // validator
    CHECK(failsWith(trySet(f.mib, {{kRoot + Oid{1, 3, 0}, Value::counter32(1)}}), ErrorStatus::NotWritable, 0));
    CHECK(failsWith(trySet(f.mib, {{kRoot + Oid{2, 1, 3, 1}, Value::gauge(1)}}), ErrorStatus::NotWritable, 0));
    CHECK(failsWith(trySet(f.mib, {{kRoot + Oid{2, 1, 2, 99}, Value::string("z")}}), ErrorStatus::NoCreation, 0));
    CHECK(failsWith(trySet(f.mib, {{kRoot + Oid{9, 9, 0}, Value::integer(1)}}), ErrorStatus::NoCreation, 0));

    CHECK(!trySet(f.mib, {{kRoot + Oid{2, 1, 2, 2}, Value::string("renamed")}}));
    CHECK(f.rows.at(2).first == "renamed");
}

void testSetErrorIndexAndAtomicity() {
    Fixture f;
    // second varbind invalid -> error attributed to index 1, first one NOT applied (validated before apply)
    auto e = trySet(f.mib, {{kRoot + Oid{1, 2, 0}, Value::integer(60)}, {kRoot + Oid{1, 1, 0}, Value::integer(5)}});
    CHECK(failsWith(e, ErrorStatus::WrongType, 1));
    CHECK(f.limit == 10);

    // validator failure on the third varbind
    e = trySet(f.mib, {{kRoot + Oid{1, 1, 0}, Value::string("q")},
                       {kRoot + Oid{2, 1, 2, 1}, Value::string("w")},
                       {kRoot + Oid{1, 2, 0}, Value::integer(500)}});
    CHECK(failsWith(e, ErrorStatus::WrongValue, 2));
    CHECK(f.name == "n" && f.rows.at(1).first == "a");

    // failure in the apply phase (injected): everything written before is rolled back, index points at the culprit
    e = trySet(f.mib, {{kRoot + Oid{1, 2, 0}, Value::integer(60)},
                       {kRoot + Oid{1, 1, 0}, Value::string("changed")},
                       {kRoot + Oid{2, 1, 2, 1}, Value::string("commitfail")}});
    CHECK(failsWith(e, ErrorStatus::CommitFailed, 2));
    CHECK(f.limit == 10);
    CHECK(f.name == "n");
    CHECK(f.rows.at(1).first == "a");

    // a failed apply of the very first op leaves nothing behind either
    e = trySet(f.mib, {{kRoot + Oid{2, 1, 2, 1}, Value::string("commitfail")}, {kRoot + Oid{1, 2, 0}, Value::integer(70)}});
    CHECK(failsWith(e, ErrorStatus::CommitFailed, 0));
    CHECK(f.limit == 10);

    // the same PDU without the bad value succeeds completely
    CHECK(!trySet(f.mib, {{kRoot + Oid{1, 2, 0}, Value::integer(60)}, {kRoot + Oid{2, 1, 2, 1}, Value::string("ok")}}));
    CHECK(f.limit == 60 && f.rows.at(1).first == "ok");

    // exceptions that are not SetError become genErr(prepare) / commitFailed(apply) and name the varbind
    Mib m(kRoot);
    int boom = 0;
    m.scalar({1}, {Type::Integer, [&] { return Value::integer(boom); }, [&](const Value&) { throw std::runtime_error("disk"); },
                   [](const Value&) {}});
    e = trySet(m, {{kRoot + Oid{1, 0}, Value::integer(1)}});
    CHECK(failsWith(e, ErrorStatus::CommitFailed, 0));
}

void testMibDefinitions() {
    Mib m(kRoot);
    ScalarDef s{Type::Integer, [] { return Value::integer(1); }};
    m.scalar({1, 1}, s);
    CHECK_THROWS(m.scalar({1, 1}, s), Error);            // duplicate
    CHECK_THROWS(m.scalar({1, 1, 5}, s), Error);         // below an existing scalar
    CHECK_THROWS(m.scalar({1}, s), Error);               // above an existing scalar
    CHECK_THROWS(m.scalar({7}, ScalarDef{}), Error);     // getter missing
    CHECK_THROWS(m.table(5, TableDef{}), Error);         // no callbacks / columns

    TableDef noRows;
    noRows.columns = {{2, Type::Integer, Access::ReadOnly}};
    noRows.get = [](const Oid&, SubId) { return Value::integer(1); };
    CHECK_THROWS(m.table(6, noRows), Error);  // neither rows nor nextRow+hasRow
    noRows.nextRow = [](const Oid*) { return std::optional<Oid>(); };
    CHECK_THROWS(m.table(6, noRows), Error);  // nextRow alone is not enough (no hasRow)
    noRows.hasRow = [](const Oid&) { return false; };
    m.table(6, noRows);                       // ok

    TableDef badIdx = noRows;
    badIdx.indexes = {IndexSpec::impliedString(), IndexSpec::integer()};
    CHECK_THROWS(m.table(8, badIdx), Error);  // IMPLIED not last

    // getter returning the wrong type is a programming error reported as Error
    m.scalar({2, 1}, ScalarDef{Type::Integer, [] { return Value::string("oops"); }});
    CHECK_THROWS(m.get(kRoot + Oid{2, 1, 0}), Error);
}

// ---------------------------------------------------------------------------------------------
// Multi-index table (ipAddress, integer, IMPLIED string)
// ---------------------------------------------------------------------------------------------
void testMultiIndexTable() {
    const std::vector<IndexSpec> spec{IndexSpec::ipAddress(), IndexSpec::integer(), IndexSpec::impliedString()};
    std::map<Oid, std::int32_t> state;
    auto key = [&](std::uint8_t d, int port, const char* tag) {
        return encodeIndex(spec, {Value::ipAddress(10, 0, 0, d), Value::integer(port), Value::string(tag)});
    };
    state[key(1, 80, "web")] = 1;
    state[key(1, 80, "web2")] = 2;
    state[key(1, 443, "tls")] = 3;
    state[key(2, 22, "ssh")] = 4;
    state[key(1, 80, "")] = 5;  // empty implied string

    Mib m(kRoot);
    TableDef t;
    t.indexes = spec;
    t.columns = {{5, Type::Integer, Access::ReadWrite}};
    t.rows = [&] {
        std::vector<Oid> r;
        for (auto& [k, v] : state) r.push_back(k);
        return r;
    };
    t.get = [&](const Oid& i, SubId) { return Value::integer(state.at(i)); };
    t.set = [&](const Oid& i, SubId, const Value& v) { state.at(i) = v.asInt(); };
    // a malformed row that the application (wrongly) reports must be ignored by the agent
    state[Oid({9, 9})] = 99;
    m.table(3, std::move(t));

    CHECK(m.get(kRoot + Oid{3, 1, 5} + key(1, 80, "web")) == Value::integer(1));
    CHECK(m.get(kRoot + Oid{3, 1, 5} + key(1, 80, "")) == Value::integer(5));
    CHECK(!m.get(kRoot + Oid{3, 1, 5} + key(1, 81, "web")));
    CHECK(!m.get(kRoot + Oid{3, 1, 5, 9, 9}));  // malformed index is not an instance
    CHECK(m.missing(kRoot + Oid{3, 1, 5, 9, 9}) == Type::NoSuchInstance);

    // OID order: 10.0.0.1:80 "" < "web" < "web2", then :443, then 10.0.0.2
    std::vector<Oid> order;
    Oid cur = kRoot;
    while (auto nx = m.getNext(cur)) {
        order.push_back(nx->oid);
        cur = nx->oid;
    }
    CHECK(order.size() == 5);
    CHECK(order.size() == 5 && order[0] == kRoot + Oid{3, 1, 5} + key(1, 80, ""));
    CHECK(order.size() == 5 && order[1] == kRoot + Oid{3, 1, 5} + key(1, 80, "web"));
    CHECK(order.size() == 5 && order[2] == kRoot + Oid{3, 1, 5} + key(1, 80, "web2"));
    CHECK(order.size() == 5 && order[3] == kRoot + Oid{3, 1, 5} + key(1, 443, "tls"));
    CHECK(order.size() == 5 && order[4] == kRoot + Oid{3, 1, 5} + key(2, 22, "ssh"));

    CHECK(!trySet(m, {{kRoot + Oid{3, 1, 5} + key(2, 22, "ssh"), Value::integer(40)}}));
    CHECK(state.at(key(2, 22, "ssh")) == 40);
    CHECK(failsWith(trySet(m, {{kRoot + Oid{3, 1, 5, 9, 9}, Value::integer(1)}}), ErrorStatus::NoCreation, 0));
}

// ---------------------------------------------------------------------------------------------
// Large table via nextRow/hasRow: GETNEXT must never enumerate all rows
// ---------------------------------------------------------------------------------------------
void testLargeTableScales() {
    constexpr std::uint32_t N = 100000;
    std::map<std::uint32_t, std::uint32_t> data;
    for (std::uint32_t i = 1; i <= N; ++i) data[i * 2] = i;  // even indexes only

    int rowsCalls = 0;
    Mib m(kRoot);
    TableDef t;
    t.indexes = {IndexSpec::unsignedInt()};
    t.columns = {{2, Type::Gauge32, Access::ReadOnly}, {3, Type::Gauge32, Access::ReadOnly}};
    t.rows = [&] {
        ++rowsCalls;
        std::vector<Oid> r;
        for (auto& [k, v] : data) r.push_back(Oid{k});
        return r;
    };
    t.hasRow = [&](const Oid& i) { return i.size() == 1 && data.count(i[0]) > 0; };
    t.nextRow = [&](const Oid* after) -> std::optional<Oid> {
        auto it = after ? data.upper_bound((*after)[0]) : data.begin();  // single-sub-id indexes only here
        if (after && after->size() != 1) it = after->empty() ? data.begin() : data.upper_bound((*after)[0]);
        if (it == data.end()) return std::nullopt;
        return Oid{it->first};
    };
    t.get = [&](const Oid& i, SubId c) { return Value::gauge(data.at(i[0]) + c); };
    m.table(1, std::move(t));

    std::size_t count = 0;
    Oid cur = kRoot;
    bool ascending = true;
    while (auto nx = m.getNext(cur)) {
        ascending = ascending && nx->oid > cur;
        cur = nx->oid;
        ++count;
    }
    CHECK(ascending);
    CHECK(count == 2u * N);
    CHECK(rowsCalls == 0);  // never enumerated the whole table

    CHECK(m.getNext(kRoot + Oid{1, 1, 2, 4})->oid == kRoot + Oid{1, 1, 2, 6});
    CHECK(m.getNext(kRoot + Oid{1, 1, 2, 5})->oid == kRoot + Oid{1, 1, 2, 6});  // between rows
    CHECK(m.getNext(kRoot + Oid{1, 1, 2, 2 * N})->oid == kRoot + Oid{1, 1, 3, 2});  // column boundary
    CHECK(!m.getNext(kRoot + Oid{1, 1, 3, 2 * N}));
    CHECK(m.get(kRoot + Oid{1, 1, 3, 200}) == Value::gauge(103));
    CHECK(!m.get(kRoot + Oid{1, 1, 3, 201}));
    CHECK(rowsCalls == 0);  // get() used hasRow as well
}

void testFallbackLoadsRowsOncePerRequest() {
    std::map<std::uint32_t, int> data;
    for (std::uint32_t i = 1; i <= 500; ++i) data[i] = static_cast<int>(i);
    int rowsCalls = 0;
    Mib m(kRoot);
    TableDef t;
    t.columns = {{2, Type::Integer, Access::ReadOnly}, {3, Type::Integer, Access::ReadOnly}, {4, Type::Integer, Access::ReadOnly}};
    t.rows = [&] {
        ++rowsCalls;
        std::vector<Oid> r;
        for (auto it = data.rbegin(); it != data.rend(); ++it) r.push_back(Oid{it->first});  // deliberately unsorted
        return r;
    };
    t.get = [&](const Oid& i, SubId c) { return Value::integer(data.at(i[0]) + static_cast<int>(c)); };
    m.table(1, std::move(t));

    Oid cur = kRoot;
    std::size_t count = 0;
    bool ok = true;
    for (;;) {
        const int before = rowsCalls;
        auto nx = m.getNext(cur);
        ok = ok && rowsCalls - before <= 1;  // incl. column transitions
        if (!nx) break;
        ok = ok && nx->oid > cur;
        cur = nx->oid;
        ++count;
    }
    CHECK(ok);
    CHECK(count == 1500);
}

// ---------------------------------------------------------------------------------------------
// RowStatus
// ---------------------------------------------------------------------------------------------
struct RowTable {
    struct Row {
        std::string name;
        std::uint32_t value = 0;
        RowStatus status = RowStatus::NotReady;
    };
    std::map<std::uint32_t, Row> rows;
    int destroyCalls = 0;
    Mib mib{kRoot};

    static constexpr SubId kName = 2, kValue = 3, kStatus = 4;

    explicit RowTable(bool rejectEditWhileActive = false) {
        TableDef t;
        t.indexes = {IndexSpec::integer()};
        t.columns = {{kName, Type::OctetString, Access::ReadWrite}, {kValue, Type::Gauge32, Access::ReadWrite}};
        t.rows = [this] {
            std::vector<Oid> r;
            for (auto& [k, v] : rows) r.push_back(Oid{k});
            return r;
        };
        t.get = [this](const Oid& i, SubId c) {
            auto& r = rows.at(i[0]);
            return c == kName ? Value::string(r.name) : Value::gauge(r.value);
        };
        t.set = [this](const Oid& i, SubId c, const Value& v) {
            auto& r = rows.at(i[0]);
            if (c == kName) {
                if (v.asString() == "commitfail") throw SetError(ErrorStatus::CommitFailed, "injected");
                r.name = v.asString();
            } else {
                r.value = v.asUInt();
            }
        };
        t.validate = [](const Oid&, SubId c, const Value& v) {
            if (c == kValue && v.asUInt() > 1000) throw SetError(ErrorStatus::WrongValue, "too big");
        };
        RowStatusSpec rs;
        rs.column = kStatus;
        rs.requiredColumns = {kName};
        rs.create = [this](const Oid& i, const std::map<SubId, Value>& cols) {
            Row r;
            if (auto n = cols.find(kName); n != cols.end()) r.name = n->second.asString();
            if (auto v = cols.find(kValue); v != cols.end()) r.value = v->second.asUInt();
            rows[i[0]] = r;
        };
        rs.destroy = [this](const Oid& i) { ++destroyCalls; rows.erase(i[0]); };
        rs.setState = [this](const Oid& i, RowStatus s) { rows.at(i[0]).status = s; };
        rs.state = [this](const Oid& i) { return rows.at(i[0]).status; };
        rs.complete = [this](const Oid& i) { return !rows.at(i[0]).name.empty(); };
        rs.rejectEditWhileActive = rejectEditWhileActive;
        t.rowStatus = rs;
        mib.table(4, std::move(t));
    }

    Oid cell(SubId col, std::uint32_t idx) const { return kRoot + Oid{4, 1, col, idx}; }
    static Value st(RowStatus s) { return Value::integer(static_cast<int>(s)); }
};

void testRowStatusCreateAndGo() {
    RowTable t;
    // missing required column
    auto e = trySet(t.mib, {{t.cell(RowTable::kValue, 5), Value::gauge(1)}, {t.cell(RowTable::kStatus, 5), RowTable::st(RowStatus::CreateAndGo)}});
    CHECK(failsWith(e, ErrorStatus::InconsistentValue, 1));
    CHECK(t.rows.empty());

    // proper create
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kName, 5), Value::string("eth5")}, {t.cell(RowTable::kStatus, 5), RowTable::st(RowStatus::CreateAndGo)}}));
    CHECK(t.rows.size() == 1 && t.rows.at(5).name == "eth5" && t.rows.at(5).status == RowStatus::Active);
    CHECK(t.mib.get(t.cell(RowTable::kStatus, 5)) == RowTable::st(RowStatus::Active));
    CHECK(t.mib.get(t.cell(RowTable::kName, 5)) == Value::string("eth5"));
    // the status column takes part in walks, sorted after the other columns
    CHECK(t.mib.getNext(t.cell(RowTable::kValue, 5))->oid == t.cell(RowTable::kStatus, 5));
    CHECK(!t.mib.getNext(t.cell(RowTable::kStatus, 5)));

    // create on an existing row
    e = trySet(t.mib, {{t.cell(RowTable::kStatus, 5), RowTable::st(RowStatus::CreateAndGo)}});
    CHECK(failsWith(e, ErrorStatus::InconsistentValue, 0));
    e = trySet(t.mib, {{t.cell(RowTable::kStatus, 5), RowTable::st(RowStatus::CreateAndWait)}});
    CHECK(failsWith(e, ErrorStatus::InconsistentValue, 0));
}

void testRowStatusCreateAndWait() {
    RowTable t;
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kStatus, 7), RowTable::st(RowStatus::CreateAndWait)}}));
    CHECK(t.rows.at(7).status == RowStatus::NotReady);  // name missing -> incomplete
    CHECK(t.mib.get(t.cell(RowTable::kStatus, 7)) == RowTable::st(RowStatus::NotReady));

    // activating an incomplete row fails in the apply phase and changes nothing
    auto e = trySet(t.mib, {{t.cell(RowTable::kStatus, 7), RowTable::st(RowStatus::Active)}});
    CHECK(failsWith(e, ErrorStatus::InconsistentValue, 0));
    CHECK(t.rows.at(7).status == RowStatus::NotReady);

    // supplying the name promotes notReady -> notInService
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kName, 7), Value::string("x")}}));
    CHECK(t.rows.at(7).status == RowStatus::NotInService);

    // name and active in ONE request also works for a notReady row
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kStatus, 8), RowTable::st(RowStatus::CreateAndWait)}}));
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kName, 8), Value::string("y")}, {t.cell(RowTable::kStatus, 8), RowTable::st(RowStatus::Active)}}));
    CHECK(t.rows.at(8).status == RowStatus::Active);

    CHECK(!trySet(t.mib, {{t.cell(RowTable::kStatus, 7), RowTable::st(RowStatus::Active)}}));
    CHECK(t.rows.at(7).status == RowStatus::Active);
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kStatus, 7), RowTable::st(RowStatus::NotInService)}}));
    CHECK(t.rows.at(7).status == RowStatus::NotInService);

    // createAndWait with the name already present is complete right away
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kName, 9), Value::string("z")}, {t.cell(RowTable::kStatus, 9), RowTable::st(RowStatus::CreateAndWait)}}));
    CHECK(t.rows.at(9).status == RowStatus::NotInService);
}

void testRowStatusDestroy() {
    RowTable t;
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kName, 1), Value::string("a")}, {t.cell(RowTable::kStatus, 1), RowTable::st(RowStatus::CreateAndGo)}}));

    // destroy of a non-existing row is a no-op without error
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kStatus, 99), RowTable::st(RowStatus::Destroy)}}));
    CHECK(t.destroyCalls == 0);

    // destroy is irreversible, so it only happens in COMMIT
    auto txn = t.mib.prepare({{t.cell(RowTable::kStatus, 1), RowTable::st(RowStatus::Destroy)}});
    txn->apply();
    CHECK(t.rows.count(1) == 1);  // still there after apply
    txn->commit();
    CHECK(t.rows.count(1) == 0 && t.destroyCalls == 1);

    // destroy is NOT executed when another part of the request fails
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kName, 1), Value::string("a")}, {t.cell(RowTable::kStatus, 1), RowTable::st(RowStatus::CreateAndGo)}}));
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kName, 2), Value::string("b")}, {t.cell(RowTable::kStatus, 2), RowTable::st(RowStatus::CreateAndGo)}}));
    auto e = trySet(t.mib, {{t.cell(RowTable::kStatus, 1), RowTable::st(RowStatus::Destroy)}, {t.cell(RowTable::kName, 2), Value::string("commitfail")}});
    CHECK(failsWith(e, ErrorStatus::CommitFailed, 1));
    CHECK(t.rows.count(1) == 1 && t.rows.count(2) == 1);
}

void testRowStatusErrors() {
    RowTable t;
    // cell of a non-existing row without create -> inconsistentName
    CHECK(failsWith(trySet(t.mib, {{t.cell(RowTable::kName, 3), Value::string("x")}}), ErrorStatus::InconsistentName, 0));
    // active / notInService on a non-existing row
    CHECK(failsWith(trySet(t.mib, {{t.cell(RowTable::kStatus, 3), RowTable::st(RowStatus::Active)}}), ErrorStatus::InconsistentValue, 0));
    // invalid RowStatus values
    CHECK(failsWith(trySet(t.mib, {{t.cell(RowTable::kStatus, 3), Value::integer(9)}}), ErrorStatus::WrongValue, 0));
    CHECK(failsWith(trySet(t.mib, {{t.cell(RowTable::kStatus, 3), Value::integer(0)}}), ErrorStatus::WrongValue, 0));
    CHECK(failsWith(trySet(t.mib, {{t.cell(RowTable::kStatus, 3), RowTable::st(RowStatus::NotReady)}}), ErrorStatus::WrongValue, 0));
    CHECK(failsWith(trySet(t.mib, {{t.cell(RowTable::kStatus, 3), Value::string("x")}}), ErrorStatus::WrongType, 0));
    // column validator applies during creation as well
    CHECK(failsWith(trySet(t.mib, {{t.cell(RowTable::kName, 3), Value::string("x")}, {t.cell(RowTable::kValue, 3), Value::gauge(5000)},
                                   {t.cell(RowTable::kStatus, 3), RowTable::st(RowStatus::CreateAndGo)}}), ErrorStatus::WrongValue, 1));
    CHECK(t.rows.empty());
    // unknown column / wrong type for a column
    CHECK(failsWith(trySet(t.mib, {{kRoot + Oid{4, 1, 9, 3}, Value::integer(1)}}), ErrorStatus::NoCreation, 0));
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kName, 3), Value::string("x")}, {t.cell(RowTable::kStatus, 3), RowTable::st(RowStatus::CreateAndGo)}}));
    CHECK(failsWith(trySet(t.mib, {{t.cell(RowTable::kName, 3), Value::integer(1)}}), ErrorStatus::WrongType, 0));
    // malformed index: a table with a 2-column index refuses creation with inconsistentName
}

void testRowStatusRollback() {
    RowTable t;
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kName, 1), Value::string("keep")}, {t.cell(RowTable::kStatus, 1), RowTable::st(RowStatus::CreateAndGo)}}));
    // create row 5 and modify row 1 in one request; the modification fails in apply -> row 5 must vanish again
    auto e = trySet(t.mib, {{t.cell(RowTable::kName, 5), Value::string("new")},
                            {t.cell(RowTable::kStatus, 5), RowTable::st(RowStatus::CreateAndGo)},
                            {t.cell(RowTable::kValue, 1), Value::gauge(77)},
                            {t.cell(RowTable::kName, 1), Value::string("commitfail")}});
    CHECK(failsWith(e, ErrorStatus::CommitFailed, 3));
    CHECK(t.rows.count(5) == 0);
    CHECK(t.rows.at(1).value == 0 && t.rows.at(1).name == "keep");

    // a state change is rolled back as well
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kStatus, 6), RowTable::st(RowStatus::CreateAndWait)}}));
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kName, 1), Value::string("keep")}}));
    e = trySet(t.mib, {{t.cell(RowTable::kName, 6), Value::string("six")}, {t.cell(RowTable::kName, 1), Value::string("commitfail")}});
    CHECK(failsWith(e, ErrorStatus::CommitFailed, 1));
    CHECK(t.rows.at(6).name.empty() && t.rows.at(6).status == RowStatus::NotReady);  // promotion undone too
}

void testRowStatusActiveEdit() {
    // default: active rows stay editable
    {
        RowTable t;
        CHECK(!trySet(t.mib, {{t.cell(RowTable::kName, 1), Value::string("a")}, {t.cell(RowTable::kStatus, 1), RowTable::st(RowStatus::CreateAndGo)}}));
        CHECK(!trySet(t.mib, {{t.cell(RowTable::kName, 1), Value::string("b")}}));
    }
    // rejectEditWhileActive: RFC 2579 behaviour
    RowTable t(true);
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kName, 1), Value::string("a")}, {t.cell(RowTable::kStatus, 1), RowTable::st(RowStatus::CreateAndGo)}}));
    auto e = trySet(t.mib, {{t.cell(RowTable::kName, 1), Value::string("b")}});
    CHECK(failsWith(e, ErrorStatus::InconsistentValue, 0));
    CHECK(t.rows.at(1).name == "a");
    // together with notInService it is allowed
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kName, 1), Value::string("b")}, {t.cell(RowTable::kStatus, 1), RowTable::st(RowStatus::NotInService)}}));
    CHECK(t.rows.at(1).name == "b" && t.rows.at(1).status == RowStatus::NotInService);
    // a not-in-service row can be edited on its own
    CHECK(!trySet(t.mib, {{t.cell(RowTable::kName, 1), Value::string("c")}}));
    CHECK(t.rows.at(1).name == "c");
}

void testRowStatusMalformedIndex() {
    std::map<Oid, int> rows;
    const std::vector<IndexSpec> spec{IndexSpec::integer(), IndexSpec::string()};
    Mib m(kRoot);
    TableDef t;
    t.indexes = spec;
    t.columns = {{2, Type::Integer, Access::ReadWrite}};
    t.rows = [&] {
        std::vector<Oid> r;
        for (auto& [k, v] : rows) r.push_back(k);
        return r;
    };
    t.get = [&](const Oid& i, SubId) { return Value::integer(rows.at(i)); };
    t.set = [&](const Oid& i, SubId, const Value& v) { rows.at(i) = v.asInt(); };
    RowStatusSpec rs;
    rs.column = 3;
    rs.create = [&](const Oid& i, const std::map<SubId, Value>&) { rows[i] = 0; };
    rs.destroy = [&](const Oid& i) { rows.erase(i); };
    rs.setState = [](const Oid&, RowStatus) {};
    rs.state = [](const Oid&) { return RowStatus::Active; };
    t.rowStatus = rs;
    m.table(1, std::move(t));

    const Oid good = encodeIndex(spec, {Value::integer(1), Value::string("ab")});
    CHECK(!trySet(m, {{kRoot + Oid{1, 1, 3} + good, Value::integer(4)}}));
    CHECK(rows.count(good) == 1);
    // trailing garbage / truncated string -> not a valid index
    CHECK(failsWith(trySet(m, {{kRoot + Oid{1, 1, 3, 1, 2, 'a', 'b', 9}, Value::integer(4)}}), ErrorStatus::InconsistentName, 0));
    CHECK(failsWith(trySet(m, {{kRoot + Oid{1, 1, 3, 1, 5, 'a'}, Value::integer(4)}}), ErrorStatus::InconsistentName, 0));
    CHECK(failsWith(trySet(m, {{kRoot + Oid{1, 1, 3}, Value::integer(4)}}), ErrorStatus::NoCreation, 0));  // no index at all
    CHECK(rows.size() == 1);
}

}  // namespace

int main() {
    testOid();
    testValue();
    testIndexCodec();
    testMibGet();
    testMissingSemantics();
    testMibGetNext();
    testMibSet();
    testSetErrorIndexAndAtomicity();
    testMibDefinitions();
    testMultiIndexTable();
    testLargeTableScales();
    testFallbackLoadsRowsOncePerRequest();
    testRowStatusCreateAndGo();
    testRowStatusCreateAndWait();
    testRowStatusDestroy();
    testRowStatusErrors();
    testRowStatusRollback();
    testRowStatusActiveEdit();
    testRowStatusMalformedIndex();
    std::cout << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures ? 1 : 0;
}
