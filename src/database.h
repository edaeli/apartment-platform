#pragma once

#include <sqlite3.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

class SqliteError : public std::runtime_error {
public:
    SqliteError(int code, const std::string& message) : std::runtime_error(message), code(code) {}
    bool busy() const { return (code & 255) == SQLITE_BUSY || (code & 255) == SQLITE_LOCKED; }
    int code;
};

enum class BookingFailure { notFound, forbidden, conflict, busy };
class BookingError : public std::runtime_error {
public:
    BookingError(BookingFailure reason, const std::string& message)
        : std::runtime_error(message), reason(reason) {}
    BookingFailure reason;
};

// RAII: деструктор освобождает запрос, даже если возникло исключение.
class Statement {
public:
    Statement(sqlite3* db, const std::string& sql);
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int index, const std::string& value);
    void bind(int index, std::int64_t value);
    void bind(int index, double value);
    bool step(); // true = получена строка; false = запрос завершён
    std::int64_t integer(int column) const;
    double real(int column) const;
    std::string text(int column) const;
    bool isNull(int column) const;

private:
    void check(int result) const;
    sqlite3_stmt* statement_ = nullptr;
};

struct Photo {
    std::string url;
    std::string caption;
};

struct Listing {
    std::int64_t id, propertyId, price;
    std::string dealType, status, kind, address, district, description;
    double area, latitude, longitude;
    int rooms, floor;
    std::vector<Photo> photos;
    std::optional<std::int64_t> sellerUserId, sourceBookingId;
    std::string createdAt;
    std::string renovation = "unspecified";
    bool isFavorite = false;
};

struct ListingFilters {
    std::string type;
    std::string district;
    std::optional<std::int64_t> minPrice, maxPrice, rooms;
    std::string sort = "price_asc";
    std::int64_t page = 1;
    std::int64_t pageSize = 24;
};

struct ListingPage {
    std::vector<Listing> items;
    std::int64_t total = 0;
};

struct User {
    std::int64_t id;
    std::string name, email, passwordHash, createdAt;
};

struct Booking {
    std::int64_t id, userId, listingId, propertyId, price;
    std::string dealType, status, createdAt, endedAt;
    Listing listing;
    std::optional<std::int64_t> resaleListingId;
};

struct PersonalEntry {
    Listing listing;
    std::string savedAt;
    std::int64_t lastViewedAt = 0, viewCount = 0;
};
struct PersonalPage {
    std::vector<PersonalEntry> items;
    std::int64_t total = 0;
};
struct Recommendation {
    Listing listing;
    int score = 0;
    std::string reason;
};

// Одно соединение на операцию/HTTP-запрос. Соединение не делится между потоками.
class Database {
public:
    explicit Database(const std::string& path, bool create = false);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void execute(const std::string& sql); // Только фиксированный SQL из кода/схемы!
    std::int64_t scalar(const std::string& sql);
    void migrate(const std::string& schemaPath);
    ListingPage listings(const ListingFilters& filters);
    std::optional<Listing> listing(std::int64_t id);
    std::vector<std::string> districts();
    std::optional<User> userByEmail(const std::string& email);
    std::optional<User> userById(std::int64_t id);
    std::int64_t createUser(const std::string& name, const std::string& email, const std::string& hash);
    std::int64_t book(std::int64_t userId, std::int64_t listingId);
    void releaseBooking(std::int64_t userId, std::int64_t bookingId);
    std::int64_t resellBooking(std::int64_t userId, std::int64_t bookingId, std::int64_t price);
    static constexpr std::int64_t maxResalePrice = 1000000000000LL;
    std::vector<Booking> bookingsForUser(std::int64_t userId);
    void setFavorite(std::int64_t userId, std::int64_t listingId, bool saved);
    bool recordView(std::int64_t userId, std::int64_t listingId);
    PersonalPage personalPage(std::int64_t userId, bool views, std::int64_t page, std::int64_t pageSize);
    void markFavorites(std::vector<Listing>& items, std::int64_t userId);
    std::vector<Listing> listingsByIds(const std::vector<std::int64_t>& ids);
    std::vector<Recommendation> recommendations(std::int64_t userId, int limit);
    sqlite3* handle() const { return db_; }

private:
    std::int64_t writeTransaction(const std::function<std::int64_t()>& operation);
    std::int64_t finishBooking(std::int64_t userId, std::int64_t bookingId,
                               std::optional<std::int64_t> resalePrice);
    sqlite3* db_ = nullptr;
};

void seedDemoData(Database& db);
