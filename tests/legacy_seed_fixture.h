#pragma once
#include "seed.h"

// Только тестовая фикстура схем 1–5. Приложение больше не генерирует этот набор.
inline void legacyFixture(Database& db, int count = 1000) {
    for (int n = 1; n <= count; ++n) {
        const auto p = legacyDemoProperty(n);
        Statement property(db.handle(), "INSERT INTO properties(demo_key,kind,address,district,area,rooms,floor,description,latitude,longitude) VALUES(?,?,?,?,?,?,?,?,?,?)");
        property.bind(1,p.key); property.bind(2,p.kind); property.bind(3,"Ереван, " + p.address); property.bind(4,p.district);
        property.bind(5,p.area); property.bind(6,static_cast<std::int64_t>(p.rooms)); property.bind(7,static_cast<std::int64_t>(p.floor));
        property.bind(8,p.description); property.bind(9,p.latitude); property.bind(10,p.longitude); property.step();
        const auto id=static_cast<std::int64_t>(sqlite3_last_insert_rowid(db.handle()));
        Statement listing(db.handle(),"INSERT INTO listings(property_id,demo_key,deal_type,price) VALUES(?,?,?,?)");
        listing.bind(1,id); listing.bind(2,p.key); listing.bind(3,p.dealType); listing.bind(4,p.price); listing.step();
        const std::vector<Photo> photos{{p.kind=="house"?"/images/house.jpg":"/images/living-room.jpg",p.kind=="house"?"Дом снаружи":"Гостиная"}, {"/images/kitchen.jpg","Кухня"},{"/images/bedroom.jpg","Спальня"}};
        for (std::size_t i=0;i<photos.size();++i) {
            Statement photo(db.handle(),"INSERT INTO property_photos(property_id,url,caption,sort_order) VALUES(?,?,?,?)");
            photo.bind(1,id); photo.bind(2,photos[i].url); photo.bind(3,photos[i].caption); photo.bind(4,static_cast<std::int64_t>(i));photo.step();
        }
    }
}
