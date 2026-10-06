// NAAb SQL executor -- see include/naab/sql_executor.h for the containment model.

#include "naab/sql_executor.h"
#include "naab/resource_limits.h"

#include <sqlite3.h>

#include <climits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace naab {
namespace runtime {

namespace {

thread_local SqlBindings t_pending_bindings;

// Refuses every action that reaches outside the in-memory database.
int authorizer(void*, int action, const char*, const char*, const char*, const char*) {
    switch (action) {
        case SQLITE_ATTACH:   // ATTACH 'file' -- and VACUUM INTO 'file' attaches its target
        case SQLITE_DETACH:
            return SQLITE_DENY;
        default:
            return SQLITE_OK;
    }
}

// Lets --timeout stop a running statement. Non-zero aborts with SQLITE_INTERRUPT.
int progressHandler(void*) {
    return security::ResourceLimiter::isTimeoutTriggered() ? 1 : 0;
}

std::string errorText(sqlite3* db, const std::string& what) {
    return "SQL error: " + what + ": " + (db ? sqlite3_errmsg(db) : "no database") + "\n";
}

interpreter::NaabVal columnValue(sqlite3_stmt* stmt, int col) {
    switch (sqlite3_column_type(stmt, col)) {
        case SQLITE_INTEGER: {
            const sqlite3_int64 v = sqlite3_column_int64(stmt, col);
            if (v >= INT_MIN && v <= INT_MAX) return interpreter::NaabVal::makeInt(static_cast<int>(v));
            return interpreter::NaabVal::makeDouble(static_cast<double>(v));
        }
        case SQLITE_FLOAT:
            return interpreter::NaabVal::makeDouble(sqlite3_column_double(stmt, col));
        case SQLITE_TEXT: {
            const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return interpreter::NaabVal::makeString(std::string(p ? p : "",
                static_cast<size_t>(sqlite3_column_bytes(stmt, col))));
        }
        case SQLITE_BLOB: {
            const auto* p = static_cast<const char*>(sqlite3_column_blob(stmt, col));
            return interpreter::NaabVal::makeString(std::string(p ? p : "",
                static_cast<size_t>(sqlite3_column_bytes(stmt, col))));
        }
        default:
            return interpreter::NaabVal::makeNull();
    }
}

void bindParameters(sqlite3* db, sqlite3_stmt* stmt, const SqlBindings& bindings) {
    const int n = sqlite3_bind_parameter_count(stmt);
    for (int i = 1; i <= n; i++) {
        const char* raw = sqlite3_bind_parameter_name(stmt, i);
        if (!raw || raw[0] == '?') {
            throw std::runtime_error(
                "SQL error: positional parameters ('?') cannot be bound\n\n"
                "  Help:\n  - Name the parameter after a bound variable: <<sql[x]>> ... WHERE id = :x\n");
        }
        const std::string name(raw + 1);  // strip the ':' / '@' / '$' prefix
        const interpreter::NaabVal* value = nullptr;
        for (const auto& [k, v] : bindings) {
            if (k == name) { value = &v; break; }
        }
        if (!value) {
            throw std::runtime_error(
                "SQL error: parameter " + std::string(raw) + " has no bound variable\n\n"
                "  Help:\n  - Bind it in the block header: <<sql[" + name + "]>>\n");
        }
        int rc;
        if (value->isNull())        rc = sqlite3_bind_null(stmt, i);
        else if (value->isBool())   rc = sqlite3_bind_int(stmt, i, value->asBool() ? 1 : 0);
        else if (value->isInt())    rc = sqlite3_bind_int64(stmt, i, value->asInt());
        else if (value->isDouble()) rc = sqlite3_bind_double(stmt, i, value->asDouble());
        else if (value->isString()) {
            const std::string& s = value->asString();
            rc = sqlite3_bind_text(stmt, i, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
        } else {
            throw std::runtime_error(
                "SQL error: variable '" + name + "' is not a scalar\n\n"
                "  Help:\n  - Only null, bool, int, float and string bind to SQL parameters\n"
                "  - Bind the fields you need one at a time, or pass a JSON string\n");
        }
        if (rc != SQLITE_OK) throw std::runtime_error(errorText(db, "binding :" + name));
    }
}

struct SharedDb {
    sqlite3* db = nullptr;
    std::mutex mutex;  // one connection; blocks may arrive from worker threads
    SharedDb() {
        if (sqlite3_open_v2(":memory:", &db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MEMORY |
                            SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            db = nullptr;
            return;
        }
        sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr);
        sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
        sqlite3_limit(db, SQLITE_LIMIT_ATTACHED, 0);
        sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 64 * 1024 * 1024);
        sqlite3_set_authorizer(db, authorizer, nullptr);
        sqlite3_progress_handler(db, 1000, progressHandler, nullptr);
        sqlite3_exec(db, "PRAGMA temp_store = MEMORY", nullptr, nullptr, nullptr);
    }
    ~SharedDb() { if (db) sqlite3_close(db); }
};

SharedDb& sharedDb() {
    static SharedDb instance;
    return instance;
}

} // namespace

void setPendingSqlBindings(SqlBindings bindings) {
    t_pending_bindings = std::move(bindings);
}

SqlExecutor::SqlExecutor() = default;
SqlExecutor::~SqlExecutor() = default;

bool SqlExecutor::isInitialized() const { return sharedDb().db != nullptr; }

std::string SqlExecutor::getRuntimeVersion() const {
    return std::string("SQLite ") + sqlite3_libversion();
}

interpreter::NaabVal SqlExecutor::run(const std::string& code, const SqlBindings& bindings) {
    SharedDb& shared = sharedDb();
    sqlite3* db_ = shared.db;
    if (!db_) throw std::runtime_error("SQL error: the in-memory database could not be opened\n");
    std::lock_guard<std::mutex> lock(shared.mutex);

    interpreter::NaabVal last_rows = interpreter::NaabVal::makeNull();
    const char* tail = code.c_str();
    const char* end = tail + code.size();
    while (tail < end) {
        sqlite3_stmt* stmt = nullptr;
        const char* next = nullptr;
        if (sqlite3_prepare_v2(db_, tail, static_cast<int>(end - tail), &stmt, &next) != SQLITE_OK) {
            if (stmt) sqlite3_finalize(stmt);
            throw std::runtime_error(errorText(db_, "preparing statement"));
        }
        tail = next;
        if (!stmt) continue;  // whitespace or a comment

        try {
            bindParameters(db_, stmt, bindings);
            const int cols = sqlite3_column_count(stmt);
            std::vector<interpreter::NaabVal> rows;
            int rc;
            while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
                if (rows.size() >= kMaxResultRows) {
                    throw std::runtime_error(
                        "SQL error: a statement returned more than " +
                        std::to_string(kMaxResultRows) + " rows\n\n"
                        "  Help:\n  - Add a LIMIT, or aggregate in SQL\n");
                }
                std::unordered_map<std::string, interpreter::NaabVal> row;
                for (int c = 0; c < cols; c++) {
                    const char* cname = sqlite3_column_name(stmt, c);
                    row[cname ? cname : std::to_string(c)] = columnValue(stmt, c);
                }
                rows.push_back(interpreter::NaabVal::makeDict(std::move(row)));
            }
            if (rc == SQLITE_INTERRUPT) {
                throw std::runtime_error("Execution timeout: SQL statement stopped at the time limit\n");
            }
            if (rc != SQLITE_DONE) throw std::runtime_error(errorText(db_, "executing statement"));
            if (cols > 0) last_rows = interpreter::NaabVal::makeList(std::move(rows));
        } catch (...) {
            sqlite3_finalize(stmt);
            throw;
        }
        sqlite3_finalize(stmt);
    }
    return last_rows;
}

interpreter::NaabVal SqlExecutor::executeWithReturn(const std::string& code) {
    SqlBindings bindings = std::move(t_pending_bindings);
    t_pending_bindings.clear();
    return run(code, bindings);
}

bool SqlExecutor::execute(const std::string& code) {
    executeWithReturn(code);
    return true;
}

interpreter::NaabVal SqlExecutor::callFunction(const std::string& function_name,
                                               const std::vector<interpreter::NaabVal>&) {
    throw std::runtime_error("SQL error: '" + function_name + "' -- SQL blocks have no callable functions\n");
}

} // namespace runtime
} // namespace naab
