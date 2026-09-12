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

// Общий механизм для пользовательских операций записи. Повторяется вся операция, не её кусок.
std::int64_t Database::writeTransaction(const std::function<std::int64_t()>& operation) {
    sqlite3_busy_timeout(db_, 250);
    for (int attempt = 0; attempt < 4; ++attempt) {
        try {
            execute("BEGIN IMMEDIATE");
            const auto result = operation();
            execute("COMMIT");
            return result;
        } catch (const SqliteError& error) {
            if (!sqlite3_get_autocommit(db_)) execute("ROLLBACK");
            if (!error.busy()) throw;
            if (attempt == 3) throw BookingError(BookingFailure::busy,
                "База данных временно занята. Повторите попытку позже");
            std::this_thread::sleep_for(std::chrono::milliseconds(25 * (attempt + 1)));
        } catch (...) {
            if (!sqlite3_get_autocommit(db_)) execute("ROLLBACK");
            throw;
        }
    }
    throw BookingError(BookingFailure::busy, "База данных временно занята");
}

std::int64_t Database::book(std::int64_t userId, std::int64_t listingId) {
    return writeTransaction([&] {
        {
            Statement exists(db_, "SELECT seller_user_id FROM listings WHERE id = ?");
            exists.bind(1, listingId);
            if (!exists.step()) throw BookingError(BookingFailure::notFound, "Объявление не найдено");
            if (!exists.isNull(0) && exists.integer(0) == userId) {
                throw BookingError(BookingFailure::forbidden, "Нельзя забронировать собственное объявление");
            }
        }
        Statement update(db_, R"SQL(
            UPDATE listings SET status = 'reserved'
            WHERE id = ? AND status = 'available' AND (seller_user_id IS NULL OR seller_user_id != ?)
        )SQL");
        update.bind(1, listingId);
        update.bind(2, userId);
        update.step();
        if (sqlite3_changes(db_) != 1) {
            throw BookingError(BookingFailure::conflict, "Это жильё уже забронировали");
        }
        Statement insert(db_, R"SQL(
            INSERT INTO bookings(user_id, listing_id, property_id, price_at_booking, deal_type)
            SELECT ?, id, property_id, price, deal_type FROM listings WHERE id = ?
        )SQL");
        insert.bind(1, userId);
        insert.bind(2, listingId);
        insert.step();
        return sqlite3_last_insert_rowid(db_);
    });
}

void Database::releaseBooking(std::int64_t userId, std::int64_t bookingId) {
    finishBooking(userId, bookingId, std::nullopt);
}

std::int64_t Database::resellBooking(std::int64_t userId, std::int64_t bookingId, std::int64_t price) {
    if (price < 1 || price > maxResalePrice) throw std::invalid_argument("Цена должна быть от 1 до 1000000000000 драмов");
    return finishBooking(userId, bookingId, price);
}

std::int64_t Database::finishBooking(std::int64_t userId, std::int64_t bookingId,
                                    std::optional<std::int64_t> resalePrice) {
    return writeTransaction([&]() -> std::int64_t {
        const bool resale = resalePrice.has_value();
        const std::string type = resale ? "sale" : "rent";
        std::int64_t listingId, propertyId;
        {
            Statement query(db_, "SELECT user_id, listing_id, property_id, deal_type, status FROM bookings WHERE id = ?");
            query.bind(1, bookingId);
            if (!query.step()) throw BookingError(BookingFailure::notFound, "Бронирование не найдено");
            if (query.integer(0) != userId) throw BookingError(BookingFailure::forbidden, "Это бронирование другого пользователя");
            if (query.text(3) != type) throw BookingError(BookingFailure::conflict,
                resale ? "Перепродавать можно только купленное жильё" : "Освободить можно только арендованное жильё");
            if (query.text(4) != "active") throw BookingError(BookingFailure::conflict, "Это бронирование уже завершено");
            listingId = query.integer(1);
            propertyId = query.integer(2);
        }
        Statement finish(db_, R"SQL(
            UPDATE bookings SET status = ?, ended_at = strftime('%Y-%m-%dT%H:%M:%SZ', 'now')
            WHERE id = ? AND user_id = ? AND deal_type = ? AND status = 'active'
        )SQL");
        finish.bind(1, std::string(resale ? "resold" : "released"));
        finish.bind(2, bookingId);
        finish.bind(3, userId);
        finish.bind(4, type);
        finish.step();
        if (sqlite3_changes(db_) != 1) throw BookingError(BookingFailure::conflict, "Это бронирование уже завершено");

        Statement update(db_, R"SQL(
            UPDATE listings SET status = ?
            WHERE id = ? AND property_id = ? AND deal_type = ? AND status = 'reserved'
        )SQL");
        update.bind(1, std::string(resale ? "closed" : "available"));
        update.bind(2, listingId);
        update.bind(3, propertyId);
        update.bind(4, type);
        update.step();
        if (sqlite3_changes(db_) != 1) throw BookingError(BookingFailure::conflict,
            "Состояние объявления изменилось. Обновите кабинет");
        if (!resale) return listingId;

        Statement insert(db_, R"SQL(
            INSERT INTO listings(property_id, deal_type, price, status, seller_user_id, source_booking_id)
            VALUES (?, 'sale', ?, 'available', ?, ?)
        )SQL");
        insert.bind(1, propertyId);
        insert.bind(2, *resalePrice);
        insert.bind(3, userId);
        insert.bind(4, bookingId);
        insert.step(); // demo_key остаётся NULL, created_at заполняется текущим временем.
        return sqlite3_last_insert_rowid(db_);
    });
}

std::vector<Booking> Database::bookingsForUser(std::int64_t userId) {
    Statement query(db_, R"SQL(
        SELECT b.id, b.user_id, b.listing_id, b.property_id, b.price_at_booking,
               b.deal_type, b.status, b.created_at, COALESCE(b.ended_at, ''), r.id
        FROM bookings AS b LEFT JOIN listings AS r ON r.source_booking_id = b.id
        WHERE b.user_id = ? ORDER BY b.id DESC
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
        if (!query.isNull(9)) item.resaleListingId = query.integer(9);
        const auto object = listing(item.listingId);
        if (!object) throw std::runtime_error("Booking references missing listing");
        item.listing = *object;
        result.push_back(item);
    }
    return result;
}
