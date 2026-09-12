-- Только чтение. Замените user_id = 1 своим номером из users.

SELECT id, name, email FROM users ORDER BY id;

SELECT f.listing_id, p.address, p.district, l.deal_type, l.price, l.status, f.created_at
FROM favorites AS f
JOIN listings AS l ON l.id = f.listing_id
JOIN properties AS p ON p.id = l.property_id
WHERE f.user_id = 1
ORDER BY f.created_at DESC, f.listing_id DESC
LIMIT 24 OFFSET 0;

SELECT v.listing_id, p.address, l.deal_type, l.price,
       datetime(v.last_viewed_at, 'unixepoch') AS last_view_utc, v.view_count
FROM listing_views AS v
JOIN listings AS l ON l.id = v.listing_id
JOIN properties AS p ON p.id = l.property_id
WHERE v.user_id = 1
ORDER BY v.last_viewed_at DESC, v.listing_id DESC;

SELECT 'favorites' AS source, COUNT(*) AS total FROM favorites WHERE user_id = 1
UNION ALL
SELECT 'views', COUNT(*) FROM listing_views WHERE user_id = 1;

-- Сигналы для рекомендаций, с теми же ограничениями и весами, что в C++.
WITH saved AS (
    SELECT listing_id FROM favorites WHERE user_id = 1
    ORDER BY created_at DESC, listing_id DESC LIMIT 100
), viewed AS (
    SELECT v.listing_id FROM listing_views AS v WHERE v.user_id = 1
    AND NOT EXISTS (SELECT 1 FROM favorites AS f WHERE f.user_id = 1 AND f.listing_id = v.listing_id)
    ORDER BY v.last_viewed_at DESC, v.listing_id DESC LIMIT 100
), signals AS (
    SELECT listing_id, 3 AS weight FROM saved
    UNION ALL SELECT listing_id, 1 AS weight FROM viewed
)
SELECT l.id, l.deal_type, p.district, l.price, s.weight FROM signals AS s
JOIN listings AS l ON l.id = s.listing_id JOIN properties AS p ON p.id = l.property_id;

EXPLAIN QUERY PLAN
SELECT listing_id, created_at FROM favorites WHERE user_id = 1
ORDER BY created_at DESC, listing_id DESC LIMIT 24 OFFSET 0;

EXPLAIN QUERY PLAN
SELECT listing_id, last_viewed_at FROM listing_views WHERE user_id = 1
ORDER BY last_viewed_at DESC, listing_id DESC LIMIT 100;
