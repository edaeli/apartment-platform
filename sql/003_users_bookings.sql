CREATE TABLE users (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL CHECK (length(name) BETWEEN 2 AND 80),
    email TEXT NOT NULL UNIQUE
        CHECK (email = lower(trim(email)) AND length(email) BETWEEN 3 AND 254),
    password_hash TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

-- Составной внешний ключ бронирования проверяет, что объявление относится
-- именно к указанному объекту. Для такого ключа SQLite требует UNIQUE.
CREATE UNIQUE INDEX listings_id_property ON listings(id, property_id);

CREATE TABLE bookings (
    id INTEGER PRIMARY KEY,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE RESTRICT,
    listing_id INTEGER NOT NULL,
    property_id INTEGER NOT NULL REFERENCES properties(id) ON DELETE RESTRICT,
    price_at_booking INTEGER NOT NULL
        CHECK (typeof(price_at_booking) = 'integer' AND price_at_booking > 0),
    deal_type TEXT NOT NULL CHECK (deal_type IN ('rent', 'sale')),
    status TEXT NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'released', 'resold')),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    ended_at TEXT,
    FOREIGN KEY (listing_id, property_id) REFERENCES listings(id, property_id) ON DELETE RESTRICT,
    CHECK ((status = 'active' AND ended_at IS NULL) OR
           (status != 'active' AND ended_at IS NOT NULL))
);

CREATE UNIQUE INDEX one_active_booking_per_listing
    ON bookings(listing_id) WHERE status = 'active';
CREATE UNIQUE INDEX one_active_booking_per_property
    ON bookings(property_id) WHERE status = 'active';
CREATE INDEX bookings_user_id ON bookings(user_id, id);
CREATE INDEX bookings_listing_property ON bookings(listing_id, property_id);

PRAGMA user_version = 3;
