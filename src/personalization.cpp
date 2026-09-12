#include "database.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

void Database::setFavorite(std::int64_t userId, std::int64_t listingId, bool saved) {
    writeTransaction([&]() -> std::int64_t {
        Statement exists(db_, "SELECT id FROM listings WHERE id = ?");
        exists.bind(1, listingId);
        if (!exists.step()) throw BookingError(BookingFailure::notFound, "Объявление не найдено");
        Statement query(db_, saved ?
            "INSERT INTO favorites(user_id, listing_id) VALUES (?, ?) ON CONFLICT(user_id, listing_id) DO NOTHING" :
            "DELETE FROM favorites WHERE user_id = ? AND listing_id = ?");
        query.bind(1, userId); query.bind(2, listingId); query.step();
        return 0; // Повтор не меняет даже дату добавления. Удалять отсутствующую пару допустимо.
    });
}

bool Database::recordView(std::int64_t userId, std::int64_t listingId) {
    return writeTransaction([&]() -> std::int64_t {
        Statement exists(db_, "SELECT id FROM listings WHERE id = ?");
        exists.bind(1, listingId);
        if (!exists.step()) throw BookingError(BookingFailure::notFound, "Объявление не найдено");
        Statement query(db_, R"SQL(
            INSERT INTO listing_views(user_id, listing_id, last_viewed_at)
            VALUES (?, ?, CAST(strftime('%s', 'now') AS INTEGER))
            ON CONFLICT(user_id, listing_id) DO UPDATE SET
                last_viewed_at = excluded.last_viewed_at, view_count = listing_views.view_count + 1
            WHERE listing_views.last_viewed_at <= excluded.last_viewed_at - 1800
        )SQL");
        query.bind(1, userId); query.bind(2, listingId); query.step();
        return sqlite3_changes(db_); // 0 — повтор внутри окна, без обновления времени/счётчика.
    }) == 1;
}

void Database::markFavorites(std::vector<Listing>& items, std::int64_t userId) {
    if (!userId || items.empty()) return;
    if (items.size() > 100) throw std::invalid_argument("Слишком много объявлений");
    std::string slots;
    for (std::size_t i = 0; i < items.size(); ++i) slots += i ? ",?" : "?";
    Statement query(db_, "SELECT listing_id FROM favorites WHERE user_id = ? AND listing_id IN (" + slots + ")");
    query.bind(1, userId);
    for (std::size_t i = 0; i < items.size(); ++i) query.bind(static_cast<int>(i + 2), items[i].id);
    std::set<std::int64_t> saved;
    while (query.step()) saved.insert(query.integer(0));
    for (auto& item : items) item.isFavorite = saved.count(item.id) != 0;
}

PersonalPage Database::personalPage(std::int64_t userId, bool views, std::int64_t page, std::int64_t pageSize) {
    if (page < 1 || page > 1000000 || pageSize < 1 || pageSize > 100)
        throw std::invalid_argument("Некорректная пагинация");
    // Имена таблиц/полей — только эти два фиксированных варианта, не текст из HTTP.
    const std::string table = views ? "listing_views" : "favorites";
    const std::string fields = views ? "listing_id, last_viewed_at, view_count" : "listing_id, created_at";
    const std::string order = views ? "last_viewed_at DESC, listing_id DESC" : "created_at DESC, listing_id DESC";
    execute("BEGIN");
    try {
        PersonalPage result;
        Statement count(db_, "SELECT COUNT(*) FROM " + table + " WHERE user_id = ?");
        count.bind(1, userId); count.step(); result.total = count.integer(0);
        Statement query(db_, "SELECT " + fields + " FROM " + table + " WHERE user_id = ? ORDER BY " + order + " LIMIT ? OFFSET ?");
        query.bind(1, userId); query.bind(2, pageSize); query.bind(3, (page - 1) * pageSize);
        std::vector<std::int64_t> ids;
        while (query.step()) {
            PersonalEntry entry{};
            entry.listing.id = query.integer(0);
            if (views) { entry.lastViewedAt = query.integer(1); entry.viewCount = query.integer(2); }
            else entry.savedAt = query.text(1);
            result.items.push_back(entry); ids.push_back(entry.listing.id);
        }
        auto items = listingsByIds(ids); // Один запрос со всеми фотографиями, не запрос на карточку.
        markFavorites(items, userId);
        std::map<std::int64_t, Listing> byId;
        for (auto& item : items) byId.emplace(item.id, std::move(item));
        for (auto& entry : result.items) entry.listing = byId.at(entry.listing.id);
        execute("COMMIT");
        return result;
    } catch (...) { execute("ROLLBACK"); throw; }
}

std::vector<Recommendation> Database::recommendations(std::int64_t userId, int limit) {
    if (limit < 1 || limit > 24) throw std::invalid_argument("Размер подборки должен быть от 1 до 24");
    struct Signal { std::string type, district; double price; int weight; };
    struct Candidate { std::int64_t id; int score; std::string reason; };
    std::vector<Signal> signals;
    // Один снимок: признаки, кандидаты и фотографии не расходятся при конкурентной записи.
    execute("BEGIN");
    try {
        if (userId) {
            Statement query(db_, R"SQL(
                WITH saved AS (
                    SELECT listing_id FROM favorites WHERE user_id = ?1
                    ORDER BY created_at DESC, listing_id DESC LIMIT 100
                ), viewed AS (
                    SELECT v.listing_id FROM listing_views AS v WHERE v.user_id = ?1
                    AND NOT EXISTS (SELECT 1 FROM favorites AS f WHERE f.user_id = ?1 AND f.listing_id = v.listing_id)
                    ORDER BY v.last_viewed_at DESC, v.listing_id DESC LIMIT 100
                ), signals AS (
                    SELECT listing_id, 3 AS weight FROM saved
                    UNION ALL SELECT listing_id, 1 AS weight FROM viewed
                )
                SELECT l.deal_type, p.district, l.price, s.weight FROM signals AS s
                JOIN listings AS l ON l.id = s.listing_id JOIN properties AS p ON p.id = l.property_id
            )SQL");
            query.bind(1, userId);
            while (query.step()) signals.push_back({query.text(0), query.text(1), static_cast<double>(query.integer(2)), static_cast<int>(query.integer(3))});
        }
        Statement candidates(db_, R"SQL(
            SELECT l.id, l.deal_type, p.district, l.price FROM listings AS l
            JOIN properties AS p ON p.id = l.property_id
            WHERE l.status = 'available' AND (l.seller_user_id IS NULL OR l.seller_user_id != ?1)
            AND NOT EXISTS (SELECT 1 FROM favorites AS f WHERE f.user_id = ?1 AND f.listing_id = l.id)
            ORDER BY l.id
        )SQL");
        candidates.bind(1, userId);
        std::vector<Candidate> best;
        while (candidates.step()) {
            const auto type = candidates.text(1), district = candidates.text(2);
            const double price = static_cast<double>(candidates.integer(3));
            int points = 0, weight = 0;
            bool sameDistrict = false, districtAndPrice = false;
            for (const auto& signal : signals) {
                if (signal.type != type) continue; // Аренда и продажа никогда не сравниваются по цене.
                const double distance = std::abs(price - signal.price) / signal.price;
                const int proximity = distance <= .10 ? 4 : distance <= .25 ? 2 : distance <= .50 ? 1 : 0;
                const bool districtMatch = district == signal.district;
                points += signal.weight * ((districtMatch ? 4 : 0) + proximity);
                weight += signal.weight;
                sameDistrict |= districtMatch;
                districtAndPrice |= districtMatch && distance <= .25;
            }
            const int score = weight ? points * 100 / weight : 0;
            const std::string reason = !score ? "Доступное жильё из каталога" : districtAndPrice ?
                "Похожая цена в интересующем вас районе" : sameDistrict ?
                "В интересующем вас районе" : "Близкий ценовой диапазон";
            best.push_back({candidates.integer(0), score, reason});
            std::sort(best.begin(), best.end(), [](const Candidate& a, const Candidate& b) {
                return a.score != b.score ? a.score > b.score : a.id < b.id;
            });
            if (best.size() > static_cast<std::size_t>(limit)) best.pop_back();
        }
        std::vector<std::int64_t> ids;
        for (const auto& item : best) ids.push_back(item.id);
        std::map<std::int64_t, Listing> listings;
        for (auto& item : listingsByIds(ids)) listings.emplace(item.id, std::move(item));
        std::vector<Recommendation> result;
        for (const auto& item : best) result.push_back({listings.at(item.id), item.score, item.reason});
        execute("COMMIT");
        return result;
    } catch (...) { execute("ROLLBACK"); throw; }
}
