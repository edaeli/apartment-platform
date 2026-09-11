-- Выполняется C++ внутри транзакции только для базы с user_version = 0.
-- PRAGMA foreign_keys = ON включается C++ ДО транзакции на каждом соединении.

CREATE TABLE properties (
    id INTEGER PRIMARY KEY,
    demo_key TEXT UNIQUE,
    kind TEXT NOT NULL CHECK (kind IN ('apartment', 'house')),
    address TEXT NOT NULL,
    district TEXT NOT NULL,
    area REAL NOT NULL CHECK (area > 0),
    rooms INTEGER NOT NULL CHECK (rooms > 0),
    floor INTEGER NOT NULL CHECK (floor >= 0),
    description TEXT NOT NULL,
    latitude REAL NOT NULL CHECK (latitude BETWEEN -90 AND 90),
    longitude REAL NOT NULL CHECK (longitude BETWEEN -180 AND 180)
);

CREATE TABLE listings (
    id INTEGER PRIMARY KEY,
    property_id INTEGER NOT NULL REFERENCES properties(id) ON DELETE RESTRICT,
    demo_key TEXT UNIQUE,
    deal_type TEXT NOT NULL CHECK (deal_type IN ('rent', 'sale')),
    -- Цена в целых армянских драмах: за месяц аренды или за весь объект.
    price INTEGER NOT NULL CHECK (typeof(price) = 'integer' AND price > 0),
    status TEXT NOT NULL DEFAULT 'available'
        CHECK (status IN ('available', 'reserved', 'closed')),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

-- Исторических закрытых объявлений может быть много, незакрытое — одно.
CREATE UNIQUE INDEX one_open_listing_per_property
    ON listings(property_id) WHERE status IN ('available', 'reserved');
CREATE INDEX listings_property_id ON listings(property_id);

CREATE TABLE property_photos (
    id INTEGER PRIMARY KEY,
    property_id INTEGER NOT NULL REFERENCES properties(id) ON DELETE RESTRICT,
    url TEXT NOT NULL,
    caption TEXT NOT NULL,
    sort_order INTEGER NOT NULL CHECK (sort_order >= 0),
    UNIQUE (property_id, sort_order)
);

PRAGMA user_version = 1;
