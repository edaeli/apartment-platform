-- Личные списки ссылаются именно на объявление, включая его закрытую историю.
CREATE TABLE favorites (
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE RESTRICT,
    listing_id INTEGER NOT NULL REFERENCES listings(id) ON DELETE RESTRICT,
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    PRIMARY KEY (user_id, listing_id)
);
CREATE INDEX favorites_user_recent ON favorites(user_id, created_at DESC, listing_id DESC);
CREATE INDEX favorites_listing ON favorites(listing_id);

-- Одна строка на пару: последнее УЧТЁННОЕ открытие и число таких открытий.
CREATE TABLE listing_views (
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE RESTRICT,
    listing_id INTEGER NOT NULL REFERENCES listings(id) ON DELETE RESTRICT,
    last_viewed_at INTEGER NOT NULL CHECK (typeof(last_viewed_at) = 'integer' AND last_viewed_at >= 0),
    view_count INTEGER NOT NULL DEFAULT 1 CHECK (typeof(view_count) = 'integer' AND view_count > 0),
    PRIMARY KEY (user_id, listing_id)
);
CREATE INDEX views_user_recent ON listing_views(user_id, last_viewed_at DESC, listing_id DESC);
CREATE INDEX views_listing ON listing_views(listing_id);

PRAGMA user_version = 5;
