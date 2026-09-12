# Планы запросов SQLite — этап 2

Фактический запуск 12 сентября 2026 на рабочем демонаборе из 1000 объектов. Использовался `-readonly`; данные не изменялись.

Версия SQLite: `3.53.4 2026-07-24 19:02:57 bf7c7f30031888f4e796e429ab3978879485813aaca6f641c7b33e4e09459bcc (64-bit)`.

Номера соответствуют [catalog-queries.sql](catalog-queries.sql). Для повторения выделите один запрос в DB Browser и добавьте `EXPLAIN QUERY PLAN` перед `SELECT` или `WITH`. Вывод зависит от версии SQLite, статистики и содержимого базы.

## Запрос 1

Сам SELECT выполнен: 1 строк результата.

```text
QUERY PLAN
|--SEARCH l USING INDEX listings_available_type_price (deal_type=? AND price>? AND price<?)
`--SEARCH p USING COVERING INDEX properties_district_rooms (district=? AND rooms=? AND id=? AND rowid=?)
```

## Запрос 2

Сам SELECT выполнен: 33 строк результата.

```text
QUERY PLAN
|--CO-ROUTINE page
|  |--SEARCH l USING INDEX listings_available_type_price (deal_type=? AND price>? AND price<?)
|  `--SEARCH p USING INTEGER PRIMARY KEY (rowid=?)
|--SCAN page
|--SEARCH ph USING INDEX sqlite_autoindex_property_photos_1 (property_id=?) LEFT-JOIN
`--USE TEMP B-TREE FOR LAST TERM OF ORDER BY
```

## Запрос 3

Сам SELECT выполнен: 24 строк результата.

```text
QUERY PLAN
|--SCAN l USING INDEX listings_available_price
`--SEARCH p USING INTEGER PRIMARY KEY (rowid=?)
```

## Запрос 4

Сам SELECT выполнен: 24 строк результата.

```text
QUERY PLAN
|--SCAN l USING INDEX listings_available_price
|--SEARCH p USING INTEGER PRIMARY KEY (rowid=?)
`--USE TEMP B-TREE FOR LAST TERM OF ORDER BY
```

## Запрос 5

Сам SELECT выполнен: 24 строк результата.

```text
QUERY PLAN
|--SCAN l USING INDEX listings_available_price
|--SEARCH p USING INTEGER PRIMARY KEY (rowid=?)
`--USE TEMP B-TREE FOR ORDER BY
```

## Запрос 6

Сам SELECT выполнен: 24 строк результата.

```text
QUERY PLAN
|--SCAN l USING INDEX listings_available_price
|--SEARCH p USING INTEGER PRIMARY KEY (rowid=?)
`--USE TEMP B-TREE FOR ORDER BY
```

## Запрос 7

Сам SELECT выполнен: 23 строк результата.

```text
QUERY PLAN
|--SEARCH p USING INDEX properties_district_rooms (district=? AND rooms=?)
|--SEARCH l USING INDEX listings_property_id (property_id=?)
`--USE TEMP B-TREE FOR ORDER BY
```

## Запрос 8

Сам SELECT выполнен: 3 строк результата.

```text
QUERY PLAN
|--SEARCH l USING INTEGER PRIMARY KEY (rowid=?)
|--SEARCH p USING INTEGER PRIMARY KEY (rowid=?)
`--SEARCH ph USING INDEX sqlite_autoindex_property_photos_1 (property_id=?) LEFT-JOIN
```
