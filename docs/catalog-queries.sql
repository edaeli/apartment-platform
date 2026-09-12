-- 1. Число результатов при совместном применении всех фильтров.
SELECT COUNT(*) AS total
FROM listings AS l
JOIN properties AS p ON p.id = l.property_id
WHERE l.status = 'available'
  AND l.deal_type = 'rent'
  AND p.district = 'Кентрон'
  AND l.price >= 200000
  AND l.price <= 500000
  AND p.rooms = 2;

-- 2. Фактическая форма выборки C++: сначала страница, затем фотографии.
-- Вместо параметров ? подставлены значения для запуска в DB Browser.
WITH page AS (
    SELECT l.id, l.property_id, l.price, l.deal_type, l.status,
           p.kind, p.address, p.district, p.description, p.area,
           p.latitude, p.longitude, p.rooms, p.floor
    FROM listings AS l
    JOIN properties AS p ON p.id = l.property_id
    WHERE l.status = 'available'
      AND l.deal_type = 'rent'
      AND p.district = 'Кентрон'
      AND l.price >= 200000
      AND l.price <= 500000
      AND p.rooms = 2
    ORDER BY l.price ASC, l.id ASC
    LIMIT 24 OFFSET 0
)
SELECT page.*, ph.url, ph.caption
FROM page
LEFT JOIN property_photos AS ph ON ph.property_id = page.property_id
ORDER BY page.price ASC, page.id ASC, ph.sort_order ASC;

-- 3. Вторая страница всего каталога по возрастанию цены (без строк фото).
SELECT l.id, p.address, p.area, l.deal_type, l.price
FROM listings AS l
JOIN properties AS p ON p.id = l.property_id
WHERE l.status = 'available'
ORDER BY l.price ASC, l.id ASC
LIMIT 24 OFFSET 24;

-- 4. Самые дорогие доступные объявления, при равной цене — меньший id первым.
SELECT l.id, p.address, p.area, l.deal_type, l.price
FROM listings AS l
JOIN properties AS p ON p.id = l.property_id
WHERE l.status = 'available'
ORDER BY l.price DESC, l.id ASC
LIMIT 24 OFFSET 0;

-- 5. По возрастанию площади.
SELECT l.id, p.address, p.area, l.deal_type, l.price
FROM listings AS l
JOIN properties AS p ON p.id = l.property_id
WHERE l.status = 'available'
ORDER BY p.area ASC, l.id ASC
LIMIT 24 OFFSET 0;

-- 6. По убыванию площади.
SELECT l.id, p.address, p.area, l.deal_type, l.price
FROM listings AS l
JOIN properties AS p ON p.id = l.property_id
WHERE l.status = 'available'
ORDER BY p.area DESC, l.id ASC
LIMIT 24 OFFSET 0;

-- 7. Поиск по району и комнатам.
SELECT l.id, p.address, p.area, l.price
FROM listings AS l
JOIN properties AS p ON p.id = l.property_id
WHERE l.status = 'available' AND p.district = 'Кентрон' AND p.rooms = 2
ORDER BY l.price ASC, l.id ASC
LIMIT 24 OFFSET 0;

-- 8. Подробности объявления № 1, включая фотографии и любой статус.
SELECT l.id, l.property_id, l.price, l.deal_type, l.status,
       p.kind, p.address, p.district, p.description, p.area,
       p.latitude, p.longitude, p.rooms, p.floor, ph.url, ph.caption
FROM listings AS l
JOIN properties AS p ON p.id = l.property_id
LEFT JOIN property_photos AS ph ON ph.property_id = p.id
WHERE l.id = 1
ORDER BY ph.sort_order ASC;
