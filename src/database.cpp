#include "database.h"

#include <fstream>
#include <filesystem>
#include <map>
#include <sstream>
#include <stdexcept>

Statement::Statement(sqlite3* db, const std::string& sql) {
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement_, nullptr) != SQLITE_OK) {
        const std::string message = sqlite3_errmsg(db);
        const int code = sqlite3_extended_errcode(db);
        sqlite3_finalize(statement_);
        throw SqliteError(code, message);
    }
}

Statement::~Statement() { sqlite3_finalize(statement_); }

void Statement::check(int result) const {
    if (result != SQLITE_OK) {
        throw SqliteError(sqlite3_extended_errcode(sqlite3_db_handle(statement_)),
                          sqlite3_errmsg(sqlite3_db_handle(statement_)));
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
    throw SqliteError(sqlite3_extended_errcode(sqlite3_db_handle(statement_)),
                          sqlite3_errmsg(sqlite3_db_handle(statement_)));
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
        sqlite3_extended_result_codes(db_, 1);
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
        const int code = sqlite3_extended_errcode(db_);
        sqlite3_free(error);
        throw SqliteError(code, message);
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
        if (version < 0 || version > 4) {
            throw std::runtime_error("Unsupported database schema version");
        }
        const std::vector<std::string> migrations{
            schemaPath,
            (std::filesystem::path(schemaPath).parent_path() / "002_catalog_indexes.sql").string(),
            (std::filesystem::path(schemaPath).parent_path() / "003_users_bookings.sql").string(),
            (std::filesystem::path(schemaPath).parent_path() / "004_resale_origin.sql").string()
        };
        for (auto next = version + 1; next <= 4; ++next) {
            const auto& path = migrations.at(static_cast<std::size_t>(next - 1));
            std::ifstream file(path);
            if (!file) throw std::runtime_error("Cannot read schema: " + path);
            std::ostringstream sql;
            sql << file.rdbuf();
            execute(sql.str());
            if (scalar("PRAGMA user_version") != next) {
                throw std::runtime_error("Migration did not set expected schema version");
            }
        }
        execute("COMMIT");
    } catch (...) {
        execute("ROLLBACK");
        throw;
    }
}

namespace {
// В SQL добавляются только фиксированные фрагменты. Значения связываются отдельно.
std::string whereClause(const ListingFilters& filters) {
    std::string sql = " WHERE l.status = 'available'";
    if (!filters.type.empty()) sql += " AND l.deal_type = ?";
    if (!filters.district.empty()) sql += " AND p.district = ?";
    if (filters.minPrice) sql += " AND l.price >= ?";
    if (filters.maxPrice) sql += " AND l.price <= ?";
    if (filters.rooms) sql += " AND p.rooms = ?";
    return sql;
}

int bindFilters(Statement& query, const ListingFilters& filters) {
    int index = 1;
    if (!filters.type.empty()) query.bind(index++, filters.type);
    if (!filters.district.empty()) query.bind(index++, filters.district);
    if (filters.minPrice) query.bind(index++, *filters.minPrice);
    if (filters.maxPrice) query.bind(index++, *filters.maxPrice);
    if (filters.rooms) query.bind(index++, *filters.rooms);
    return index;
}

const std::string columns = R"SQL(
    l.id, l.property_id, l.price, l.deal_type, l.status,
    p.kind, p.address, p.district, p.description, p.area,
    p.latitude, p.longitude, p.rooms, p.floor,
    l.seller_user_id, l.source_booking_id, l.created_at
)SQL";
const std::string tables = " FROM listings AS l JOIN properties AS p ON p.id = l.property_id";

std::vector<Listing> readListings(Statement& query) {
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
            if (!query.isNull(14)) item.sellerUserId = query.integer(14);
            if (!query.isNull(15)) item.sourceBookingId = query.integer(15);
            item.createdAt = query.text(16);
            result.push_back(item);
        }
        if (!query.isNull(17)) result.back().photos.push_back({query.text(17), query.text(18)});
    }
    return result;
}
}

ListingPage Database::listings(const ListingFilters& filters) {
    // Белый список: пользователь не может подставить произвольный ORDER BY.
    const std::map<std::string, std::pair<std::string, std::string>> orders{
        {"price_asc", {"l.price ASC, l.id ASC", "page.price ASC, page.id ASC"}},
        {"price_desc", {"l.price DESC, l.id ASC", "page.price DESC, page.id ASC"}},
        {"area_asc", {"p.area ASC, l.id ASC", "page.area ASC, page.id ASC"}},
        {"area_desc", {"p.area DESC, l.id ASC", "page.area DESC, page.id ASC"}}
    };
    const auto order = orders.find(filters.sort);
    if (order == orders.end() || filters.page < 1 || filters.page > 1000000 ||
        filters.pageSize < 1 || filters.pageSize > 100) {
        throw std::invalid_argument("Некорректная сортировка или пагинация");
    }
    const auto where = whereClause(filters);
    // COUNT и сама страница читают один снимок базы даже при параллельных изменениях.
    execute("BEGIN");
    try {
        ListingPage result;
        {
            Statement count(db_, "SELECT COUNT(*)" + tables + where);
            bindFilters(count, filters);
            if (!count.step()) throw std::runtime_error("Expected count");
            result.total = count.integer(0);
        }
        // LIMIT применяется к объявлениям ДО соединения с несколькими фото.
        Statement query(db_, "WITH page AS (SELECT " + columns + tables + where +
            " ORDER BY " + order->second.first + " LIMIT ? OFFSET ?) "
            "SELECT page.*, ph.url, ph.caption FROM page "
            "LEFT JOIN property_photos AS ph ON ph.property_id = page.property_id "
            "ORDER BY " + order->second.second + ", ph.sort_order ASC");
        int index = bindFilters(query, filters);
        query.bind(index++, filters.pageSize);
        query.bind(index, (filters.page - 1) * filters.pageSize);
        result.items = readListings(query);
        execute("COMMIT");
        return result;
    } catch (...) {
        execute("ROLLBACK");
        throw;
    }
}

std::optional<Listing> Database::listing(std::int64_t id) {
    // Прямая страница показывает также закрытые объявления, сохраняя историю.
    Statement query(db_, "SELECT " + columns + ", ph.url, ph.caption" + tables +
        " LEFT JOIN property_photos AS ph ON ph.property_id = p.id "
        "WHERE l.id = ? ORDER BY ph.sort_order ASC");
    query.bind(1, id);
    auto result = readListings(query);
    if (result.empty()) return std::nullopt;
    return result.front();
}

std::vector<std::string> Database::districts() {
    Statement query(db_, "SELECT DISTINCT district FROM properties ORDER BY district");
    std::vector<std::string> result;
    while (query.step()) result.push_back(query.text(0));
    return result;
}
