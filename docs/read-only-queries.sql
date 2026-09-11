-- Выполнять в DB Browser, вкладка Execute SQL / Выполнить SQL.
-- Все три запроса только читают данные.

-- 1. Список физических объектов недвижимости.
SELECT id, kind, address, district, area, rooms, floor
FROM properties
ORDER BY id;

-- 2. Список всех объявлений, включая закрытые, если они появятся.
SELECT id, property_id, deal_type, price, status, created_at
FROM listings
ORDER BY id;

-- 3. Существующий объект № 1 вместе с его объявлением.
-- p и l — короткие имена таблиц; связываем их по property_id.
SELECT p.id AS property_id,
       p.address, p.district, p.area, p.rooms, p.floor,
       l.id AS listing_id,
       l.deal_type, l.price, l.status
FROM properties AS p
JOIN listings AS l ON l.property_id = p.id
WHERE p.id = 1
ORDER BY l.id;
