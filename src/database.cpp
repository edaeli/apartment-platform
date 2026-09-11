#include "database.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

Statement::Statement(sqlite3* db, const std::string& sql) {
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement_, nullptr) != SQLITE_OK) {
        const std::string message = sqlite3_errmsg(db);
        sqlite3_finalize(statement_);
        throw std::runtime_error(message);
    }
}

Statement::~Statement() { sqlite3_finalize(statement_); }

void Statement::check(int result) const {
    if (result != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(statement_)));
    }
}

void Statement::bind(int index, const std::string& value) {
    check(sqlite3_bind_text(statement_, index, value.c_str(),
                           static_cast<int>(value.size()), SQLITE_TRANSIENT));
}
void Statement::bind(int index, std::int64_t value) {
    check(sqlite3_bind_int64(statement_, index, value));
}
void Statement::bind(int index, double value) {
    check(sqlite3_bind_double(statement_, index, value));
}

bool Statement::step() {
    const int result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) return true;
    if (result == SQLITE_DONE) return false;
    throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(statement_)));
}

std::int64_t Statement::integer(int column) const {
    return sqlite3_column_int64(statement_, column);
}
double Statement::real(int column) const {
    return sqlite3_column_double(statement_, column);
}
std::string Statement::text(int column) const {
    const auto* value = sqlite3_column_text(statement_, column);
    return value ? reinterpret_cast<const char*>(value) : "";
}
bool Statement::isNull(int column) const {
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
}

Database::Database(const std::string& path, bool create) {
    const int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
    if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "Cannot open SQLite";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(message);
    }
    try {
        if (sqlite3_busy_timeout(db_, 5000) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        execute("PRAGMA foreign_keys = ON");
        if (scalar("PRAGMA foreign_keys") != 1) {
            throw std::runtime_error("SQLite foreign keys are disabled");
        }
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

Database::~Database() { sqlite3_close(db_); }

void Database::execute(const std::string& sql) {
    char* error = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(db_);
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

std::int64_t Database::scalar(const std::string& sql) {
    Statement query(db_, sql);
    if (!query.step()) throw std::runtime_error("Expected a row from SQLite");
    return query.integer(0);
}

void Database::migrate(const std::string& schemaPath) {
    execute("BEGIN IMMEDIATE");
    try {
        const auto version = scalar("PRAGMA user_version");
        if (version == 0) {
            std::ifstream file(schemaPath);
            if (!file) throw std::runtime_error("Cannot read schema: " + schemaPath);
            std::ostringstream sql;
            sql << file.rdbuf();
            execute(sql.str());
            if (scalar("PRAGMA user_version") != 1) {
                throw std::runtime_error("Schema did not set version 1");
            }
        } else if (version != 1) {
            throw std::runtime_error("Unsupported database schema version");
        }
        execute("COMMIT");
    } catch (...) {
        execute("ROLLBACK");
        throw;
    }
}

std::vector<Listing> Database::listings(const std::string& dealType) {
    // ?1 — значение, отдельно переданное SQLite, а не кусок SQL-кода.
    Statement query(db_, R"SQL(
        SELECT l.id, l.property_id, l.price, l.deal_type, l.status,
               p.kind, p.address, p.district, p.description, p.area,
               p.latitude, p.longitude, p.rooms, p.floor, ph.url, ph.caption
        FROM listings AS l
        JOIN properties AS p ON p.id = l.property_id
        LEFT JOIN property_photos AS ph ON ph.property_id = p.id
        WHERE l.status = 'available' AND (?1 = '' OR l.deal_type = ?1)
        ORDER BY l.id ASC, ph.sort_order ASC
    )SQL");
    query.bind(1, dealType);
    std::vector<Listing> result;
    while (query.step()) {
        if (result.empty() || result.back().id != query.integer(0)) {
            Listing item{};
            item.id = query.integer(0);
            item.propertyId = query.integer(1);
            item.price = query.integer(2);
            item.dealType = query.text(3);
            item.status = query.text(4);
            item.kind = query.text(5);
            item.address = query.text(6);
            item.district = query.text(7);
            item.description = query.text(8);
            item.area = query.real(9);
            item.latitude = query.real(10);
            item.longitude = query.real(11);
            item.rooms = static_cast<int>(query.integer(12));
            item.floor = static_cast<int>(query.integer(13));
            result.push_back(item);
        }
        if (!query.isNull(14)) {
            result.back().photos.push_back({query.text(14), query.text(15)});
        }
    }
    return result;
}
