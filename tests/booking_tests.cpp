#include "database.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <functional>

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void constraint(const std::function<void()>& operation, int code) {
    try { operation(); } catch (const SqliteError& error) {
        check(error.code == code, "Unexpected SQLite failure"); return;
    }
    throw std::runtime_error("Database accepted a constraint violation");
}
int main(int argc, char* argv[]) {
    try {
        check(argc == 2, "Expected schema path");
        Database db(":memory:", true);
        // Переход со схемы 2 на текущую: создаём и меняем старые данные до миграции.
        for (const auto& name : {"001_initial.sql", "002_catalog_indexes.sql"}) {
            const std::string path = argv[1];
            std::ifstream file(path.substr(0, path.find_last_of('/') + 1) + name);
            std::ostringstream sql; sql << file.rdbuf(); db.execute(sql.str());
        }
        seedDemoData(db);
        db.execute("UPDATE properties SET description = 'User change' WHERE id = 1");
        db.execute("UPDATE listings SET price = 123456 WHERE id = 1");
        db.migrate(argv[1]);
        check(db.scalar("PRAGMA user_version") == 5 && db.listing(1)->description == "User change" &&
              db.listing(1)->price == 123456 && db.listing(1)->photos.size() == 3, "Migration lost old data");
        const auto first = db.createUser("Первый", "first@example.com", "test-only hash");
        const auto second = db.createUser("Второй", "second@example.com", "test-only hash");
        constraint([&] { db.createUser("Другой", "first@example.com", "hash"); }, SQLITE_CONSTRAINT_UNIQUE);
        db.book(first, 1);
        check(db.bookingsForUser(first).size() == 1 && db.bookingsForUser(second).empty(), "Ownership not isolated");
        constraint([&] { db.execute("INSERT INTO bookings(user_id,listing_id,property_id,price_at_booking,deal_type) VALUES(2,1,1,100,'rent')"); }, SQLITE_CONSTRAINT_UNIQUE);
        constraint([&] { db.execute("INSERT INTO bookings(user_id,listing_id,property_id,price_at_booking,deal_type) VALUES(999,2,2,100,'rent')"); }, SQLITE_CONSTRAINT_FOREIGNKEY);
        constraint([&] { db.execute("INSERT INTO bookings(user_id,listing_id,property_id,price_at_booking,deal_type) VALUES(2,2,3,100,'rent')"); }, SQLITE_CONSTRAINT_FOREIGNKEY);
        constraint([&] { db.execute("DELETE FROM users WHERE id=1"); }, SQLITE_CONSTRAINT_TRIGGER);
        constraint([&] { db.execute("UPDATE bookings SET status='released' WHERE id=1"); }, SQLITE_CONSTRAINT_CHECK);
        // Проверяем возможность хранить завершённую запись и новую аренду.
        db.execute("BEGIN IMMEDIATE");
        db.execute("UPDATE bookings SET status='released', ended_at='2026-09-12T12:00:00Z' WHERE id=1");
        db.execute("UPDATE listings SET status='available' WHERE id=1");
        db.execute("COMMIT");
        db.book(second, 1);
        check(db.scalar("SELECT COUNT(*) FROM bookings WHERE listing_id=1") == 2 &&
              db.scalar("SELECT COUNT(*) FROM bookings WHERE listing_id=1 AND status='active'") == 1,
              "History prevents a new booking or active uniqueness failed");
        // Прямое изменение базы не должно позволить второй активный заказ объекта
        // даже через другое (закрытое историческое) объявление.
        db.execute("INSERT INTO listings(id,property_id,deal_type,price,status) VALUES(1001,1,'sale',5000,'closed')");
        constraint([&] { db.execute("INSERT INTO bookings(user_id,listing_id,property_id,price_at_booking,deal_type) VALUES(1,1001,1,5000,'sale')"); }, SQLITE_CONSTRAINT_UNIQUE);
        db.migrate(argv[1]); seedDemoData(db);
        check(db.scalar("SELECT COUNT(*) FROM users") == 2 && db.scalar("SELECT COUNT(*) FROM bookings") == 2 &&
              db.scalar("SELECT COUNT(*) FROM listings") == 1001, "Initialization changed accounts/history");
        check(db.scalar("SELECT COUNT(*) FROM pragma_foreign_key_check") == 0, "Broken foreign keys");
        std::cout << "PASS: migration 2->current, ownership, foreign keys, unique active booking and preserved history\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
