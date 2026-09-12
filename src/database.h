#pragma once

#include <sqlite3.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
    sqlite3* handle() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

void seedDemoData(Database& db);
