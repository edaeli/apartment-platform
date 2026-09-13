#include "seed.h"

namespace {
bool originalPhotos(Database& db, std::int64_t id, bool house) {
    const std::vector<Photo> expected{
        {house ? "/images/house.jpg" : "/images/living-room.jpg", house ? "Дом снаружи" : "Гостиная"},
        {"/images/kitchen.jpg", "Кухня"}, {"/images/bedroom.jpg", "Спальня"}};
    Statement query(db.handle(), "SELECT url, caption, sort_order FROM property_photos WHERE property_id = ? ORDER BY sort_order");
    query.bind(1, id);
    std::size_t i = 0;
    while (query.step()) {
        if (i >= expected.size() || query.text(0) != expected[i].url || query.text(1) != expected[i].caption ||
            query.integer(2) != static_cast<std::int64_t>(i)) return false;
        ++i;
    }
    return i == expected.size();
}
}

DemoRefreshResult refreshDemoData(Database& db, bool apply) {
    DemoRefreshResult result;
    // И просмотр, и запись используют один снимок. Во время применения другие
    // писатели не могут добавить бронирование/просмотр между проверкой и UPDATE.
    db.execute("BEGIN IMMEDIATE");
    try {
        for (int number = 1; number <= 1000; ++number) {
            const auto old = legacyDemoProperty(number);
            Statement property(db.handle(), R"SQL(
                SELECT id, kind, address, district, area, rooms, floor, description,
                       latitude, longitude, renovation, demo_revision
                FROM properties WHERE demo_key = ?
            )SQL");
            property.bind(1, old.key);
            if (!property.step()) { ++result.missing; continue; }
            const auto id = property.integer(0);
            Statement history(db.handle(), R"SQL(
                SELECT EXISTS(SELECT 1 FROM bookings WHERE property_id = ?)
                    OR (SELECT COUNT(*) FROM listings WHERE property_id = ?) != 1
                    OR EXISTS(SELECT 1 FROM listings l WHERE l.property_id = ? AND
                        (l.seller_user_id IS NOT NULL OR l.source_booking_id IS NOT NULL
                         OR EXISTS(SELECT 1 FROM bookings b WHERE b.listing_id = l.id)
                         OR EXISTS(SELECT 1 FROM favorites f WHERE f.listing_id = l.id)
                         OR EXISTS(SELECT 1 FROM listing_views v WHERE v.listing_id = l.id)))
            )SQL");
            for (int i = 1; i <= 3; ++i) history.bind(i, id);
            history.step();
            if (history.integer(0)) { ++result.protectedCount; continue; }
            if (property.integer(11) == 2) { ++result.alreadyCurrent; continue; }
            Statement listing(db.handle(), R"SQL(
                SELECT id, demo_key, deal_type, price, status FROM listings WHERE property_id = ?
            )SQL");
            listing.bind(1, id); listing.step();
            // Ключ сам по себе не доказывает происхождение. Сравниваем все старые
            // характеристики и фотографии с неизменяемым эталоном прежнего генератора.
            const bool unchanged = property.text(1) == old.kind && property.text(2) == "Ереван, " + old.address &&
                property.text(3) == old.district && property.real(4) == old.area && property.integer(5) == old.rooms &&
                property.integer(6) == old.floor && property.text(7) == old.description && property.real(8) == old.latitude &&
                property.real(9) == old.longitude && property.text(10) == "unspecified" && property.integer(11) == 0 &&
                listing.text(1) == old.key && listing.text(2) == old.dealType && listing.integer(3) == old.price &&
                listing.text(4) == "available" && originalPhotos(db, id, old.kind == "house");
            if (!unchanged) { ++result.changed; continue; }
            ++result.eligible;
            if (!apply) continue;
            const auto item = demoProperty(number);
            Statement updateProperty(db.handle(), R"SQL(
                UPDATE properties SET area = ?, rooms = ?, floor = ?, description = ?, renovation = ?, demo_revision = 2
                WHERE id = ? AND demo_revision = 0
            )SQL");
            updateProperty.bind(1, item.area); updateProperty.bind(2, static_cast<std::int64_t>(item.rooms));
            updateProperty.bind(3, static_cast<std::int64_t>(item.floor)); updateProperty.bind(4, item.description);
            updateProperty.bind(5, item.renovation); updateProperty.bind(6, id); updateProperty.step();
            if (sqlite3_changes(db.handle()) != 1) throw std::runtime_error("Demo property changed unexpectedly");
            Statement updateListing(db.handle(), "UPDATE listings SET price = ? WHERE id = ? AND status = 'available' AND price = ?");
            updateListing.bind(1, item.price); updateListing.bind(2, listing.integer(0)); updateListing.bind(3, old.price);
            updateListing.step();
            if (sqlite3_changes(db.handle()) != 1) throw std::runtime_error("Demo listing changed unexpectedly");
        }
        db.execute("COMMIT");
        return result;
    } catch (...) {
        db.execute("ROLLBACK");
        throw;
    }
}
