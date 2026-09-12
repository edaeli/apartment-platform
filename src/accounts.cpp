#include "database.h"

#include <chrono>
#include <thread>

namespace {
std::optional<User> readUser(Statement& query) {
    if (!query.step()) return std::nullopt;
    return User{query.integer(0), query.text(1), query.text(2), query.text(3), query.text(4)};
}
}

std::optional<User> Database::userByEmail(const std::string& email) {
    Statement query(db_, "SELECT id, name, email, password_hash, created_at FROM users WHERE email = ?");
    query.bind(1, email);
    return readUser(query);
}

std::optional<User> Database::userById(std::int64_t id) {
    Statement query(db_, "SELECT id, name, email, password_hash, created_at FROM users WHERE id = ?");
    query.bind(1, id);
    return readUser(query);
}

std::int64_t Database::createUser(const std::string& name, const std::string& email, const std::string& hash) {
    Statement query(db_, "INSERT INTO users(name, email, password_hash) VALUES (?, ?, ?)");
    query.bind(1, name);
    query.bind(2, email);
    query.bind(3, hash);
    query.step();
    return sqlite3_last_insert_rowid(db_);
}

std::int64_t Database::book(std::int64_t userId, std::int64_t listingId) {
    // Ожидание ограничено: до 250 мс за попытку, всего 4 попытки.
    sqlite3_busy_timeout(db_, 250);
    for (int attempt = 0; attempt < 4; ++attempt) {
        bool transaction = false;
        try {
            execute("BEGIN IMMEDIATE");
            transaction = true;
            {
                Statement exists(db_, "SELECT id FROM listings WHERE id = ?");
                exists.bind(1, listingId);
                if (!exists.step()) throw BookingError(BookingFailure::notFound, "Объявление не найдено");
            }
            Statement update(db_, "UPDATE listings SET status = 'reserved' WHERE id = ? AND status = 'available'");
            update.bind(1, listingId);
            update.step();
            if (sqlite3_changes(db_) != 1) {
                throw BookingError(BookingFailure::conflict, "Это жильё уже забронировали");
            }
            // Цена и тип берутся из базы в этой же транзакции, а userId — из сессии.
            Statement insert(db_, R"SQL(
                INSERT INTO bookings(user_id, listing_id, property_id, price_at_booking, deal_type)
                SELECT ?, id, property_id, price, deal_type FROM listings WHERE id = ?
            )SQL");
            insert.bind(1, userId);
            insert.bind(2, listingId);
            insert.step();
            const auto id = sqlite3_last_insert_rowid(db_);
            execute("COMMIT");
            return id;
        } catch (const SqliteError& error) {
            if (transaction) execute("ROLLBACK");
            if (!error.busy()) throw;
            if (attempt == 3) throw BookingError(BookingFailure::busy,
                "База данных временно занята. Повторите попытку позже");
            // При SQLITE_BUSY повторяется ВСЯ транзакция, включая проверку состояния.
            std::this_thread::sleep_for(std::chrono::milliseconds(25 * (attempt + 1)));
        } catch (...) {
            if (transaction) execute("ROLLBACK");
            throw;
        }
    }
    throw BookingError(BookingFailure::busy, "База данных временно занята");
}

std::vector<Booking> Database::bookingsForUser(std::int64_t userId) {
    Statement query(db_, R"SQL(
        SELECT id, user_id, listing_id, property_id, price_at_booking,
               deal_type, status, created_at, COALESCE(ended_at, '')
        FROM bookings WHERE user_id = ? ORDER BY id DESC
    )SQL");
    query.bind(1, userId);
    std::vector<Booking> result;
    while (query.step()) {
        Booking item{};
        item.id = query.integer(0);
        item.userId = query.integer(1);
        item.listingId = query.integer(2);
        item.propertyId = query.integer(3);
        item.price = query.integer(4);
        item.dealType = query.text(5);
        item.status = query.text(6);
        item.createdAt = query.text(7);
        item.endedAt = query.text(8);
        const auto object = listing(item.listingId);
        if (!object) throw std::runtime_error("Booking references missing listing");
        item.listing = *object;
        result.push_back(item);
    }
    return result;
}
