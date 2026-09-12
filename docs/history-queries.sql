-- Только чтение. Выполняйте запросы по одному в DB Browser.
-- В примерах выбран property_id = 1; замените 1 нужным номером объекта.

-- 1. Все объявления одного физического объекта, включая закрытые.
SELECT p.id AS property_id, p.address, p.district,
       l.id AS listing_id, l.deal_type, l.price, l.status, l.created_at,
       l.seller_user_id, u.name AS seller_name, l.source_booking_id
FROM properties AS p
JOIN listings AS l ON l.property_id = p.id
LEFT JOIN users AS u ON u.id = l.seller_user_id
WHERE p.id = 1
ORDER BY l.id;

-- 2. Кто и когда бронировал объект, исходные цены и новое объявление.
SELECT b.id AS booking_id, b.listing_id, u.name AS customer,
       b.deal_type, b.price_at_booking, b.status, b.created_at, b.ended_at,
       r.id AS resale_listing_id, r.price AS resale_price
FROM bookings AS b
JOIN users AS u ON u.id = b.user_id
LEFT JOIN listings AS r ON r.source_booking_id = b.id
WHERE b.property_id = 1
ORDER BY b.id;

-- 3. Происхождение каждого нового объявления для выбранного объекта.
SELECT old.id AS old_listing_id, old.status AS old_status,
       b.id AS source_booking_id, b.price_at_booking, b.ended_at,
       newer.id AS new_listing_id, newer.price AS new_price,
       newer.status AS new_status, newer.created_at AS published_at,
       newer.property_id, newer.seller_user_id, seller.name AS seller_name
FROM listings AS newer
JOIN bookings AS b ON b.id = newer.source_booking_id
JOIN listings AS old ON old.id = b.listing_id
JOIN users AS seller ON seller.id = newer.seller_user_id
WHERE newer.property_id = 1
ORDER BY newer.id;

-- 4. Фотографии принадлежат объекту; число перепродаж их не увеличивает.
SELECT id, property_id, url, caption, sort_order
FROM property_photos
WHERE property_id = 1
ORDER BY sort_order, id;

-- 5. Найти объекты с историей перепродажи, чтобы выбрать пример для запросов выше.
SELECT DISTINCT p.id AS property_id, p.address
FROM properties AS p
JOIN listings AS l ON l.property_id = p.id
WHERE l.source_booking_id IS NOT NULL
ORDER BY p.id;
