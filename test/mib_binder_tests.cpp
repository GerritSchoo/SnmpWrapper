// Unit tests for MibBinder: bindings are checked against the MIB, MIB validation is applied to SETs,
// DEFVALs are filled in on row creation.

#include <iostream>
#include <map>
#include <string>

#include "snmpwrap/mib_binder.hpp"

using namespace snmpwrap;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(...)                                                                                  \
    do {                                                                                            \
        ++g_checks;                                                                                 \
        if (!(__VA_ARGS__)) {                                                                       \
            ++g_failures;                                                                           \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #__VA_ARGS__ "\n";      \
        }                                                                                           \
    } while (0)

/// Expects an Error whose message contains `needle`.
template <class F>
void checkError(F&& f, const std::string& needle, int line) {
    ++g_checks;
    try {
        f();
    } catch (const Error& e) {
        if (std::string(e.what()).find(needle) != std::string::npos) return;
        ++g_failures;
        std::cerr << __FILE__ << ":" << line << ": wrong message: " << e.what() << " (expected '" << needle << "')\n";
        return;
    }
    ++g_failures;
    std::cerr << __FILE__ << ":" << line << ": no Error thrown (expected '" << needle << "')\n";
}
#define CHECK_ERROR(expr, needle) checkError([&] { expr; }, needle, __LINE__)

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

const MibModel& model() {
    static const MibModel m = MibModel::load({SNMPWRAP_SOURCE_DIR "/mibs/SNMPWRAPPER-TEST-MIB.txt"});
    return m;
}

void testScalarChecks() {
    Mib mib(model().oid("snmpWrapperTestMIB"));
    MibBinder b(mib, model());
    auto get = [] { return Value::integer(1); };
    auto set = [](const Value&) {};

    CHECK_ERROR(b.scalar("noSuchObject", get), "unknown MIB object");
    CHECK_ERROR(b.scalar("swtTable", get), "is a Table in the MIB, not a scalar");
    CHECK_ERROR(b.scalar("swtLimit", get), "read-write in the MIB, but no set callback");
    CHECK_ERROR(b.scalar("swtCounter", [] { return Value::counter32(1); }, set), "read-only in the MIB, but a set");
    CHECK_ERROR(b.scalar("sysDescr", [] { return Value::string("x"); }), "is not below the Mib root");
    CHECK_ERROR(b.scalar("swtLimit", ScalarBinding{}), "get callback is required");

    // MIB range 1..100 is enforced without any code in the binding
    int limit = 50;
    b.scalar("swtLimit", [&] { return Value::integer(limit); }, [&](const Value& v) { limit = v.asInt(); });
    const Oid inst = model().oid("swtLimit") + SubId{0};
    CHECK(!trySet(mib, {{inst, Value::integer(100)}}) && limit == 100);
    auto e = trySet(mib, {{inst, Value::integer(101)}});
    CHECK(e && e->status() == ErrorStatus::WrongValue && limit == 100);
    e = trySet(mib, {{inst, Value::string("x")}});
    CHECK(e && e->status() == ErrorStatus::WrongType);

    // SIZE (0..64) of swtName -> wrongLength; user validator runs in addition
    std::string name = "n";
    b.scalar("swtName", ScalarBinding{[&] { return Value::string(name); }, [&](const Value& v) { name = v.asString(); },
                                      [](const Value& v) {
                                          if (v.asString() == "forbidden") throw SetError(ErrorStatus::InconsistentValue);
                                      }});
    const Oid nameInst = model().oid("swtName") + SubId{0};
    e = trySet(mib, {{nameInst, Value::string(std::string(65, 'x'))}});
    CHECK(e && e->status() == ErrorStatus::WrongLength);
    e = trySet(mib, {{nameInst, Value::string("forbidden")}});
    CHECK(e && e->status() == ErrorStatus::InconsistentValue);
    CHECK(!trySet(mib, {{nameInst, Value::string("ok")}}) && name == "ok");

    // read-write without setter is allowed when explicitly configured
    Mib mib2(model().oid("snmpWrapperTestMIB"));
    MibBinder relaxed(mib2, model(), MibBinder::Options{false});
    relaxed.scalar("swtLimit", get);
    e = trySet(mib2, {{inst, Value::integer(5)}});
    CHECK(e && e->status() == ErrorStatus::NotWritable);
}

void testTableChecks() {
    Mib mib(model().oid("snmpWrapperTestMIB"));
    MibBinder b(mib, model());

    TableBinding noGet;
    noGet.rows = [] { return std::vector<Oid>{}; };
    CHECK_ERROR(b.table("swtTable", noGet), "get callback is required");

    TableBinding ro;
    ro.rows = [] { return std::vector<Oid>{Oid{1}}; };
    ro.get = [](const Oid&, SubId) { return Value::gauge(1); };
    CHECK_ERROR(b.table("swtTable", ro), "writable columns in the MIB, but no set");
    ro.set = [](const Oid&, SubId, const Value&) {};
    CHECK_ERROR(b.table("swtBigTable", ro), "no writable columns in the MIB");

    TableBinding rsMissing = ro;
    CHECK_ERROR(b.table("swtRowTable", rsMissing), "missing callbacks: create destroy setState state");

    TableBinding notRs = ro;
    notRs.create = [](const Oid&, const std::map<SubId, Value>&) {};
    notRs.destroy = [](const Oid&) {};
    notRs.setState = [](const Oid&, RowStatus) {};
    notRs.state = [](const Oid&) { return RowStatus::Active; };
    CHECK_ERROR(b.table("swtTable", notRs), "has no RowStatus column");

    CHECK(b.column("swtRowName") == 2 && b.column("swtRowValue") == 3);
    CHECK_ERROR(b.column("swtLimit"), "is not a table column");

    // the entry name works as well as the table name
    TableBinding conn;
    conn.rows = [] { return std::vector<Oid>{}; };
    conn.get = [](const Oid&, SubId) { return Value::integer(1); };
    conn.set = [](const Oid&, SubId, const Value&) {};
    b.table("swtConnEntry", conn);
    CHECK(b.unbound().size() == 9);  // 6 scalars + swtTable, swtRowTable, swtBigTable
    CHECK_ERROR(b.finish(), "MIB objects without binding");
    b.finish(true);  // explicitly allowed
}

void testRowStatusFromMib() {
    struct Entry {
        std::string name;
        std::uint32_t value = 99;
        RowStatus status = RowStatus::NotReady;
    };
    std::map<std::uint32_t, Entry> rows;
    std::map<SubId, Value> created;

    Mib mib(model().oid("snmpWrapperTestMIB"));
    MibBinder b(mib, model());
    const SubId nameCol = b.column("swtRowName"), valueCol = b.column("swtRowValue");
    TableBinding t;
    t.rows = [&] {
        std::vector<Oid> r;
        for (auto& [k, v] : rows) r.push_back(Oid{k});
        return r;
    };
    t.get = [&](const Oid& i, SubId c) { return c == nameCol ? Value::string(rows.at(i[0]).name) : Value::gauge(rows.at(i[0]).value); };
    t.set = [&](const Oid& i, SubId c, const Value& v) {
        if (c == nameCol) rows.at(i[0]).name = v.asString();
        else rows.at(i[0]).value = v.asUInt();
    };
    t.create = [&](const Oid& i, const std::map<SubId, Value>& cols) {
        created = cols;
        Entry e;
        e.name = cols.at(nameCol).asString();
        e.value = cols.at(valueCol).asUInt();
        rows[i[0]] = e;
    };
    t.destroy = [&](const Oid& i) { rows.erase(i[0]); };
    t.setState = [&](const Oid& i, RowStatus s) { rows.at(i[0]).status = s; };
    t.state = [&](const Oid& i) { return rows.at(i[0]).status; };
    b.table("swtRowTable", t);

    const Oid entry = model().oid("swtRowEntry");
    auto cell = [&](SubId col, std::uint32_t idx) { return entry + col + SubId{idx}; };
    const SubId statusCol = b.column("swtRowStatus");

    // required = writable columns without DEFVAL = swtRowName only (swtRowValue has DEFVAL { 0 })
    auto e = trySet(mib, {{cell(valueCol, 5), Value::gauge(1)}, {cell(statusCol, 5), Value::integer(4)}});
    CHECK(e && e->status() == ErrorStatus::InconsistentValue);
    CHECK(!trySet(mib, {{cell(nameCol, 5), Value::string("five")}, {cell(statusCol, 5), Value::integer(4)}}));
    // DEFVAL 0 was filled in for the missing swtRowValue
    CHECK(created.count(valueCol) == 1 && created.at(valueCol) == Value::gauge(0));
    CHECK(rows.at(5).value == 0 && rows.at(5).status == RowStatus::Active);
    // a supplied value wins over the DEFVAL
    CHECK(!trySet(mib, {{cell(nameCol, 6), Value::string("six")}, {cell(valueCol, 6), Value::gauge(7)},
                        {cell(statusCol, 6), Value::integer(4)}}));
    CHECK(rows.at(6).value == 7);
    // MIB range of swtRowValue (0..1000) applies on creation and on later SETs
    e = trySet(mib, {{cell(nameCol, 7), Value::string("x")}, {cell(valueCol, 7), Value::gauge(5000)},
                     {cell(statusCol, 7), Value::integer(4)}});
    CHECK(e && e->status() == ErrorStatus::WrongValue && e->index() == 1);
    e = trySet(mib, {{cell(valueCol, 5), Value::gauge(1001)}});
    CHECK(e && e->status() == ErrorStatus::WrongValue);
    // SIZE (0..32) of swtRowName
    e = trySet(mib, {{cell(nameCol, 5), Value::string(std::string(33, 'n'))}});
    CHECK(e && e->status() == ErrorStatus::WrongLength);
}

}  // namespace

int main() {
    try {
        testScalarChecks();
        testTableChecks();
        testRowStatusFromMib();
    } catch (const std::exception& e) {
        std::cerr << "unexpected exception: " << e.what() << "\n";
        return 1;
    }
    std::cout << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures ? 1 : 0;
}
