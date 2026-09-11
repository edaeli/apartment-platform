#include "database.h"

#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void mustFail(const std::function<void()>& action, const std::string& message) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    require(failed, message);
}
}

int main(int argc, char* argv[]) {
    try {
        require(argc == 2, "Expected schema path");
        Database db(":memory:", true);
        Database secondConnection(":memory:", true);
        require(db.scalar("PRAGMA foreign_keys") == 1, "Foreign keys disabled");
        require(secondConnection.scalar("PRAGMA foreign_keys") == 1, "Foreign keys disabled on second connection");
        db.migrate(argv[1]);
        seedDemoData(db);
        require(db.scalar("PRAGMA user_version") == 1, "Wrong schema version");
        require(db.scalar("SELECT COUNT(*) FROM properties") == 12, "Expected 12 properties");
        require(db.scalar("SELECT COUNT(*) FROM listings") == 12, "Expected 12 listings");
        require(db.scalar("SELECT COUNT(*) FROM property_photos") == 36, "Expected 36 photos");
        require(db.listings("rent").size() == 6, "Expected 6 rentals");
        require(db.listings("sale").size() == 6, "Expected 6 sales");
        require(db.listings("' OR 1=1 --").empty(), "Input was interpreted as SQL");
        for (const auto& listing : db.listings("")) {
            require(listing.photos.size() == 3, "Missing photos");
        }

        mustFail([&] { db.execute("INSERT INTO listings(property_id, deal_type, price) VALUES (9999, 'rent', 100)"); }, "Orphan listing accepted");
        mustFail([&] { db.execute("INSERT INTO property_photos(property_id, url, caption, sort_order) VALUES (9999, '/x.jpg', 'x', 0)"); }, "Orphan photo accepted");
        mustFail([&] { db.execute("UPDATE listings SET price = 0 WHERE id = 1"); }, "Zero price accepted");
        mustFail([&] { db.execute("UPDATE listings SET price = -100 WHERE id = 1"); }, "Negative price accepted");
        mustFail([&] { db.execute("UPDATE listings SET price = 100.5 WHERE id = 1"); }, "Fractional price accepted");
        mustFail([&] { db.execute("UPDATE properties SET area = -1 WHERE id = 1"); }, "Negative area accepted");
        mustFail([&] { db.execute("UPDATE listings SET status = 'wrong' WHERE id = 1"); }, "Invalid status accepted");
        mustFail([&] { db.execute("INSERT INTO listings(property_id, deal_type, price) VALUES (1, 'sale', 100)"); }, "Second open listing accepted");
        mustFail([&] { db.execute("DELETE FROM properties WHERE id = 1"); }, "Referenced property deleted");

        // Проверяем сохранение пользовательских изменений и закрытой истории.
        db.execute("UPDATE properties SET description = 'Edited by user' WHERE id = 1");
        db.execute("UPDATE listings SET price = 123456, status = 'closed' WHERE id = 1");
        db.execute("INSERT INTO listings(property_id, deal_type, price) VALUES (1, 'sale', 50000000)");
        db.migrate(argv[1]);
        seedDemoData(db);
        seedDemoData(db);
        require(db.scalar("SELECT COUNT(*) FROM properties") == 12, "Duplicate properties");
        require(db.scalar("SELECT COUNT(*) FROM listings") == 13, "History lost or duplicate listings");
        require(db.scalar("SELECT COUNT(*) FROM property_photos") == 36, "Duplicate photos");
        require(db.scalar("SELECT price FROM listings WHERE id = 1") == 123456, "Edited price overwritten");
        require(db.scalar("SELECT COUNT(*) FROM listings WHERE id = 1 AND status = 'closed'") == 1, "Closed listing reopened");
        require(db.scalar("SELECT COUNT(*) FROM properties WHERE description = 'Edited by user'") == 1, "Description overwritten");
        require(db.scalar("SELECT COUNT(*) FROM pragma_foreign_key_check") == 0, "Foreign key violation");

        // Ошибка в середине заполнения должна откатить весь набор.
        Database brokenSeed(":memory:", true);
        brokenSeed.migrate(argv[1]);
        brokenSeed.execute(R"SQL(
            CREATE TRIGGER reject_demo_photo BEFORE INSERT ON property_photos
            WHEN NEW.property_id = 4
            BEGIN SELECT RAISE(ABORT, 'test failure'); END;
        )SQL");
        mustFail([&] { seedDemoData(brokenSeed); }, "Expected seed error");
        require(brokenSeed.scalar("SELECT COUNT(*) FROM properties") == 0, "Partial seed was committed");
        require(brokenSeed.scalar("SELECT COUNT(*) FROM listings") == 0, "Partial listings committed");
        require(brokenSeed.scalar("SELECT COUNT(*) FROM property_photos") == 0, "Partial photos committed");
        std::cout << "PASS: schema, foreign keys, constraints, parameter binding, seed preservation and rollback\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
