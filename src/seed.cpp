#include "database.h"

#include <array>

namespace {
struct DemoProperty {
    const char* key;
    const char* kind;
    const char* address;
    const char* district;
    double area;
    int rooms;
    int floor;
    const char* dealType;
    std::int64_t price;
    const char* description;
    double latitude;
    double longitude;
};

// Стабильные ключи, фиксированные данные: генератор воспроизводим, без случайности.
const std::array<DemoProperty, 12> demoProperties{{
    {"demo-001", "apartment", "ул. Сарьяна, 12", "Кентрон", 64, 2, 4, "rent", 280000,
     "Светлая квартира с отдельной кухней и балконом. Рядом кафе и прогулочные улицы.", 40.1850, 44.5070},
    {"demo-002", "apartment", "пр. Комитаса, 38", "Арабкир", 82, 3, 6, "sale", 48500000,
     "Просторная гостиная, две спальни и место для работы. Окна выходят во двор.", 40.2070, 44.5110},
    {"demo-003", "house", "ул. Багреванда, 24", "Нор Норк", 156, 5, 0, "rent", 520000,
     "Дом с небольшим садом, террасой и парковочным местом. Подходит для большой семьи.", 40.2100, 44.5740},
    {"demo-004", "apartment", "ул. Аргишти, 7", "Кентрон", 45, 1, 3, "sale", 32500000,
     "Компактная квартира с открытой кухней и большим окном. Удобная планировка.", 40.1730, 44.5040},
    {"demo-005", "apartment", "ул. Алабяна, 16", "Аджапняк", 73, 3, 5, "rent", 230000,
     "Три отдельные комнаты и лоджия. Спокойный двор, рядом магазины и остановка.", 40.2030, 44.4780},
    {"demo-006", "house", "ул. Ачаряна, 52", "Аван", 210, 6, 0, "sale", 98000000,
     "Двухэтажный дом с террасой и двором. На первом этаже кухня и общая гостиная.", 40.2230, 44.5590},
    {"demo-007", "apartment", "ул. Гюльбенкяна, 21", "Арабкир", 58, 2, 2, "rent", 250000,
     "Уютная квартира с местом для чтения и отдельной спальней. Много естественного света.", 40.2020, 44.5020},
    {"demo-008", "apartment", "ул. Ширака, 10", "Шенгавит", 91, 4, 7, "sale", 39000000,
     "Семейная квартира с четырьмя комнатами. Из окон открывается вид на город.", 40.1340, 44.4880},
    {"demo-009", "house", "ул. Давида Бека, 64", "Эребуни", 128, 4, 0, "rent", 360000,
     "Одноэтажный дом с отдельным входом и зелёным двором. Есть место для летнего стола.", 40.1550, 44.5480},
    {"demo-010", "apartment", "ул. Вильнюса, 8", "Нор Норк", 67, 2, 8, "sale", 31000000,
     "Двухкомнатная квартира с просторной кухней. Рядом зелёная зона для прогулок.", 40.1910, 44.5770},
    {"demo-011", "apartment", "ул. Исакова, 19", "Малатия-Себастия", 52, 2, 4, "rent", 190000,
     "Практичная квартира с необходимой мебелью и балконом. Тихий жилой квартал.", 40.1680, 44.4690},
    {"demo-012", "house", "ул. Руставели, 30", "Канакер-Зейтун", 175, 5, 0, "sale", 76000000,
     "Дом с большой гостиной, кабинетом и участком. Пространство для семейных встреч.", 40.2240, 44.5370}
}};
}

void seedDemoData(Database& db) {
    db.execute("BEGIN IMMEDIATE");
    try {
        for (const auto& item : demoProperties) {
            Statement property(db.handle(), R"SQL(
                INSERT INTO properties
                    (demo_key, kind, address, district, area, rooms, floor,
                     description, latitude, longitude)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(demo_key) DO NOTHING
            )SQL");
            property.bind(1, std::string(item.key));
            property.bind(2, std::string(item.kind));
            property.bind(3, "Ереван, " + std::string(item.address));
            property.bind(4, std::string(item.district));
            property.bind(5, item.area);
            property.bind(6, static_cast<std::int64_t>(item.rooms));
            property.bind(7, static_cast<std::int64_t>(item.floor));
            property.bind(8, std::string(item.description));
            property.bind(9, item.latitude);
            property.bind(10, item.longitude);
            property.step();

            // Существующий объект полностью пропускаем: его цена, статус,
            // фотографии и будущая история не должны сбрасываться при запуске.
            if (sqlite3_changes(db.handle()) == 0) continue;
            const auto propertyId = static_cast<std::int64_t>(sqlite3_last_insert_rowid(db.handle()));

            Statement listing(db.handle(), R"SQL(
                INSERT INTO listings (property_id, demo_key, deal_type, price)
                VALUES (?, ?, ?, ?)
            )SQL");
            listing.bind(1, propertyId);
            listing.bind(2, std::string(item.key));
            listing.bind(3, std::string(item.dealType));
            listing.bind(4, item.price);
            listing.step();

            const bool house = std::string(item.kind) == "house";
            const std::array<Photo, 3> photos{{
                {house ? "/images/house.jpg" : "/images/living-room.jpg", house ? "Дом снаружи" : "Гостиная"},
                {"/images/kitchen.jpg", "Кухня"},
                {"/images/bedroom.jpg", "Спальня"}
            }};
            for (std::size_t i = 0; i < photos.size(); ++i) {
                Statement photo(db.handle(), R"SQL(
                    INSERT INTO property_photos (property_id, url, caption, sort_order)
                    VALUES (?, ?, ?, ?)
                )SQL");
                photo.bind(1, propertyId);
                photo.bind(2, photos[i].url);
                photo.bind(3, photos[i].caption);
                photo.bind(4, static_cast<std::int64_t>(i));
                photo.step();
            }
        }
        db.execute("COMMIT");
    } catch (...) {
        db.execute("ROLLBACK");
        throw;
    }
}
