#include "database.h"

#include <functional>
#include <fstream>
#include <iostream>
#include <sstream>
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
        require(db.scalar("PRAGMA user_version") == 2, "Wrong schema version");
        require(db.scalar("SELECT COUNT(*) FROM properties") == 1000, "Expected 1000 properties");
        require(db.scalar("SELECT COUNT(*) FROM listings") == 1000, "Expected 1000 listings");
        require(db.scalar("SELECT COUNT(*) FROM property_photos") == 3000, "Expected 3000 photos");
        ListingFilters filters;
        filters.type = "rent";
        require(db.listings(filters).total == 500, "Expected 500 rentals");
        filters.type = "sale";
        require(db.listings(filters).total == 500, "Expected 500 sales");
        filters.type = "' OR 1=1 --";
        require(db.listings(filters).total == 0, "Input was interpreted as SQL");
        filters.type.clear();
        const auto firstPage = db.listings(filters);
        require(firstPage.items.size() == 24 && firstPage.total == 1000, "Wrong default page");
        for (const auto& listing : firstPage.items) {
            require(listing.photos.size() == 3, "Missing photos");
        }
        require(db.listing(1)->address == "Ереван, ул. Сарьяна, 12", "Original object changed");
        require(!db.listing(99999), "Missing listing returned");

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
        require(db.scalar("SELECT COUNT(*) FROM properties") == 1000, "Duplicate properties");
        require(db.scalar("SELECT COUNT(*) FROM listings") == 1001, "History lost or duplicate listings");
        require(db.scalar("SELECT COUNT(*) FROM property_photos") == 3000, "Duplicate photos");
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

        // Обновляем именно базу версии 1 с изменённой демонстрационной записью.
        // id намеренно не совпадает с номером demo_key.
        Database upgrade(":memory:", true);
        std::ifstream schemaFile(argv[1]);
        std::ostringstream oldSchema;
        oldSchema << schemaFile.rdbuf();
        upgrade.execute(oldSchema.str());
        upgrade.execute(R"SQL(
            INSERT INTO properties(id, demo_key, kind, address, district, area, rooms, floor,
                description, latitude, longitude)
            VALUES (5000, 'demo-001', 'house', 'Edited address', 'Edited district', 90, 3, 0,
                'Preserve this description', 40, 44);
            INSERT INTO listings(id, property_id, demo_key, deal_type, price, status)
            VALUES (6000, 5000, 'demo-001', 'sale', 1234567, 'closed');
            INSERT INTO property_photos(property_id, url, caption, sort_order)
            VALUES (5000, '/images/custom.jpg', 'User photo', 0);
        )SQL");
        upgrade.migrate(argv[1]);
        seedDemoData(upgrade);
        upgrade.migrate(argv[1]);
        seedDemoData(upgrade);
        const auto oldListing = upgrade.listing(6000);
        require(upgrade.scalar("PRAGMA user_version") == 2, "Old schema not upgraded");
        require(upgrade.scalar("SELECT COUNT(*) FROM properties") == 1000, "Upgrade duplicated objects");
        require(oldListing && oldListing->propertyId == 5000 && oldListing->price == 1234567 &&
                oldListing->status == "closed" && oldListing->address == "Edited address" &&
                oldListing->photos.size() == 1 && oldListing->photos[0].url == "/images/custom.jpg",
                "Migration/seed overwrote existing data");
        // Неизвестную будущую версию не сбрасываем и не изменяем.
        upgrade.execute("PRAGMA user_version = 99");
        mustFail([&] { upgrade.migrate(argv[1]); }, "Future schema accepted");
        require(upgrade.scalar("PRAGMA user_version") == 99, "Future schema was reset");
        std::cout << "PASS: schema, foreign keys, constraints, parameter binding, seed preservation and rollback\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
