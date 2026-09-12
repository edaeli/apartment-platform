-- Старые строки сохраняются; для исходных объявлений оба новых поля равны NULL.
ALTER TABLE listings ADD COLUMN seller_user_id INTEGER REFERENCES users(id) ON DELETE RESTRICT;
ALTER TABLE listings ADD COLUMN source_booking_id INTEGER REFERENCES bookings(id) ON DELETE RESTRICT
    CHECK ((source_booking_id IS NULL AND seller_user_id IS NULL) OR
           (source_booking_id IS NOT NULL AND seller_user_id IS NOT NULL AND
            deal_type = 'sale' AND demo_key IS NULL AND price <= 1000000000000));

-- Одно завершённое бронирование может породить лишь одно объявление.
CREATE UNIQUE INDEX one_resale_per_booking ON listings(source_booking_id)
    WHERE source_booking_id IS NOT NULL;
CREATE INDEX listings_seller ON listings(seller_user_id);

PRAGMA user_version = 4;
