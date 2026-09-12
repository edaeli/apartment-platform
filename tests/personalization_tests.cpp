#include "database.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void add(Database& db, std::int64_t id, const std::string& type, const std::string& district,
         std::int64_t price, const std::string& status = "available") {
    Statement property(db.handle(), "INSERT INTO properties(id,kind,address,district,area,rooms,floor,description,latitude,longitude) VALUES(?,'apartment','Test address',?,60,2,3,'Test',40,44)");
    property.bind(1, id); property.bind(2, district); property.step();
    Statement listing(db.handle(), "INSERT INTO listings(id,property_id,deal_type,price,status) VALUES(?,?,?,?,?)");
    listing.bind(1, id); listing.bind(2, id); listing.bind(3, type); listing.bind(4, price); listing.bind(5, status); listing.step();
}

int score(const std::vector<Recommendation>& items, std::int64_t id) {
    for (const auto& item : items) if (item.listing.id == id) return item.score;
    return -1;
}
int position(const std::vector<Recommendation>& items, std::int64_t id) {
    for (std::size_t i=0; i<items.size(); ++i) if (items[i].listing.id == id) return static_cast<int>(i);
    return -1;
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "Need schema path");
        Database db(":memory:", true); db.migrate(argv[1]);
        auto alice = db.createUser("Alice", "alice@example.com", "test-only hash");
        auto bob = db.createUser("Bob", "bob@example.com", "test-only hash");
        add(db, 1, "rent", "Кентрон", 100000); // Один просмотр Алисы.
        add(db, 2, "rent", "Арабкир", 100000); // Цена та же, район другой.
        add(db, 3, "rent", "Кентрон", 110000); // Район и цена близки.
        add(db, 4, "rent", "Кентрон", 400000); // Район тот же, цена значительно выше.
        add(db, 5, "sale", "Кентрон", 100000); // То же число, но ПРОДАЖА: просмотр аренды не влияет.
        add(db, 6, "sale", "Арабкир", 50000000);
        add(db, 7, "sale", "Арабкир", 55000000);
        add(db, 8, "rent", "Арабкир", 100000); // Избранное Алисы.
        add(db, 9, "rent", "Арабкир", 105000);
        add(db, 10, "rent", "Кентрон", 100000, "closed");
        add(db, 11, "rent", "Кентрон", 100000, "reserved");
        auto initial = db.recommendations(alice, 24);
        check(initial.size() == 9 && initial.front().listing.id == 1, "New user fallback order/size");
        for (auto& item : initial) check(item.score == 0 && item.reason == "Доступное жильё из каталога", "False personalization");
        check(db.recordView(alice, 1), "First view ignored");
        check(!db.recordView(alice, 1), "Repeated view counted inside 30 minutes");
        auto viewed = db.recommendations(alice, 24);
        check(viewed.size() == initial.size() && viewed.back().score == 0,
              "Insufficient matches must be filled with ordinary available listings");
        const auto repeated = db.recommendations(alice, 24);
        for (std::size_t i=0; i<viewed.size(); ++i)
            check(viewed[i].listing.id == repeated[i].listing.id && viewed[i].score == repeated[i].score,
                  "Recommendations are not deterministic");
        check(position(viewed, 3) < position(viewed, 2), "Same area and close price should beat another area");
        check(position(viewed, 3) < position(viewed, 4), "Close price should beat expensive same-area home");
        check(score(viewed, 5) == 0 && score(viewed, 7) == 0, "Rent price leaked into sale preferences");
        check(db.personalPage(bob, true, 1, 24).total == 0, "Another user's views leaked");
        db.setFavorite(alice, 8, true); db.setFavorite(alice, 8, true);
        check(db.personalPage(alice, false, 1, 24).total == 1, "Duplicate favorite");
        auto saved = db.recommendations(alice, 24);
        check(position(saved, 8) == -1, "Favorite is not a new suggestion");
        check(position(saved, 2) < position(saved, 3), "Favorite area must outweigh one view of another area");
        check(position(saved, 9) < position(saved, 3), "Similar price in favorite area must outrank viewed area");
        // Даже 100 повторных учтённых просмотров одного объявления — один сигнал района/цены.
        db.execute("UPDATE listing_views SET view_count=100 WHERE listing_id=1");
        check(score(db.recommendations(alice, 24), 2) == score(saved, 2), "Repeat count inflated recommendations");
        db.recordView(alice, 6);
        auto mixed = db.recommendations(alice, 24);
        check(score(mixed, 3) == score(saved, 3) && score(mixed, 2) == score(saved, 2), "Sale signals changed rent scores");
        check(position(mixed, 7) < position(mixed, 5), "Sale should use its own price and area");
        check(position(mixed, 10) == -1 && position(mixed, 11) == -1, "Unavailable listing recommended");
        auto purchase = db.book(alice, 6);
        auto resale = db.resellBooking(alice, purchase, 51000000);
        check(position(db.recommendations(alice, 24), resale) == -1, "Seller sees own recommendation");
        check(position(db.recommendations(bob, 24), resale) >= 0, "Other user cannot see available resale");
        db.setFavorite(alice, 6, true);
        auto history = db.personalPage(alice, false, 1, 24);
        check(history.total == 2, "Closed favorite disappeared");
        bool closed = false;
        for (const auto& item : history.items) if (item.listing.id == 6) closed = item.listing.status == "closed";
        check(closed, "Favorite silently replaced original listing");
        db.setFavorite(alice, 8, false); db.setFavorite(alice, 8, false);
        check(db.personalPage(alice, false, 1, 24).total == 1, "Idempotent delete failed");
        auto limited = db.recommendations(bob, 3);
        check(limited.size() == 3, "Output limit ignored");
        for (std::size_t i=0; i<limited.size(); ++i) for (std::size_t j=0; j<i; ++j)
            check(limited[i].listing.id != limited[j].listing.id, "Duplicate recommendation");
        // Из истории берутся только последние 100 уникальных объявлений.
        db.recordView(bob, 1);
        db.execute("UPDATE listing_views SET last_viewed_at=1 WHERE user_id=2");
        for (std::int64_t id=100; id<200; ++id) {
            add(db, id, "sale", "Другой район", 900000000000, "closed");
            db.recordView(bob, id);
        }
        check(score(db.recommendations(bob, 24), 3) == 0, "More than last 100 views used");
        db.setFavorite(bob, 1, true);
        db.execute("UPDATE favorites SET created_at='2000-01-01T00:00:00Z' WHERE user_id=2");
        for (std::int64_t id=100; id<200; ++id) db.setFavorite(bob, id, true);
        check(score(db.recommendations(bob, 24), 3) == 0, "More than last 100 favorites used");
        check(db.scalar("PRAGMA foreign_keys") == 1, "Foreign keys disabled");
        std::cout << "PASS: known small dataset: area/price relevance, favorite weight, rent/sale isolation, fallback, exclusions, history and signal/output limits\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
