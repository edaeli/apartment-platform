-- Только новые индексы: существующие таблицы и данные не пересоздаются.
CREATE INDEX listings_available_price
    ON listings(price, id) WHERE status = 'available';
CREATE INDEX listings_available_type_price
    ON listings(deal_type, price, id) WHERE status = 'available';
CREATE INDEX properties_district_rooms
    ON properties(district, rooms, id);

PRAGMA user_version = 2;
