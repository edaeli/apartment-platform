"""Интеграционная проверка настоящего C++ сервера. Только стандартная библиотека Python."""

import json
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from urllib.parse import urlencode


def check(condition, message):
    if not condition:
        raise AssertionError(message)


binary = Path(sys.argv[1]).resolve()
root = Path(sys.argv[2]).resolve()

with tempfile.TemporaryDirectory(prefix="apartment-http-") as temporary:
    database = Path(temporary) / "test.sqlite3"
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    command = [str(binary), "--root", str(root), "--db", str(database), "--port", str(port)]
    process = None
    # Не направлять локальные проверки через системный HTTP-прокси.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def request(path, method="GET"):
        try:
            with opener.open(urllib.request.Request(base + path, method=method), timeout=3) as response:
                return response.status, response.read(), response.headers
        except urllib.error.HTTPError as error:
            return error.code, error.read(), error.headers

    def get_json(path):
        status, body, headers = request(path)
        check(status == 200, f"{path}: HTTP {status}: {body!r}")
        check("application/json" in headers.get("Content-Type", ""), "Missing JSON content type")
        return json.loads(body)

    def start():
        global process
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            check(process.poll() is None, "Server exited during startup")
            try:
                if request("/api/health")[0] == 200:
                    return
            except (urllib.error.URLError, TimeoutError, ConnectionError):
                pass
            time.sleep(0.1)
        raise AssertionError("Server startup timeout")

    def stop():
        global process
        if process is not None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            process = None

    def snapshot():
        with sqlite3.connect(database) as db:
            # Имена таблиц — фиксированные константы теста, не пользовательский ввод.
            return {table: db.execute(f"SELECT * FROM {table} ORDER BY id").fetchall()
                    for table in ("properties", "listings", "property_photos")}

    with (Path(temporary) / "server.log").open("w+") as log:
        try:
            start()
            health = get_json("/api/health")
            check(health["status"] == "ok" and health["foreign_keys"], "Unhealthy database")
            check(health["properties_count"] == 1000 and health["listings_count"] == 1000, "Wrong initial counts")
            catalog = get_json("/api/listings")
            check(catalog["count"] == len(catalog["items"]) == 24 and catalog["total"] == 1000, "Expected 24 of 1000 listings")
            check(catalog["page"] == 1 and catalog["page_size"] == 24 and catalog["total_pages"] == 42, "Wrong pagination metadata")
            check(health["schema_version"] == 3, "Migration not applied")
            check(get_json("/api/listings?type=rent")["total"] == 500, "Wrong rent count")
            check(get_json("/api/listings?type=sale")["total"] == 500, "Wrong sale count")
            check(request("/api/listings?type=wrong")[0] == 400, "Invalid type accepted")
            check(request("/api/listings?type=%27%20OR%201%3D1--")[0] == 400, "Injection input accepted")
            check(request("/api/listings", "POST")[0] == 405, "POST unexpectedly accepted")
            check(request("/data/apartments.sqlite3")[0] == 404, "Database exposed as static file")
            page = request("/")
            check(page[0] == 200 and 'Свой адрес'.encode() in page[1], "Catalog HTML not served")
            for asset in ("/app.js", "/common.js", "/listing.js", "/styles.css", "/images/favicon.svg", "/images/placeholder.svg"):
                check(request(asset)[0] == 200, f"Missing asset: {asset}")
            for item in catalog["items"]:
                check(len(item["photos"]) == 3, "Expected three photos per property")
                check(item["price"] > 0 and item["address"] and item["description"], "Incomplete listing")
            # Проверяем изображения всего демонабора, а не только первой страницы.
            with sqlite3.connect(database) as db:
                photo_urls = [row[0] for row in db.execute("SELECT DISTINCT url FROM property_photos")]
                incomplete = db.execute("""SELECT p.id FROM properties p
                    LEFT JOIN property_photos ph ON ph.property_id=p.id
                    GROUP BY p.id HAVING COUNT(ph.id) != 3""").fetchall()
                check(incomplete == [], "Some demo properties do not have three photos")
            check(len(photo_urls) == 4, "Expected four reused illustration files")
            for url in photo_urls:
                status, content, headers = request(url)
                check(status == 200 and content.startswith(b"\xff\xd8"), f"Missing JPEG: {url}")

            # Полные данные SQLite — эталон для проверки API, сортируем в Python.
            with sqlite3.connect(database) as db:
                db.row_factory = sqlite3.Row
                source = [dict(row) for row in db.execute("""SELECT l.id, l.deal_type, l.price,
                    p.district, p.rooms, p.area FROM listings l JOIN properties p
                    ON p.id = l.property_id WHERE l.status = 'available'""")]
                check(db.execute("SELECT COUNT(*) FROM property_photos").fetchone()[0] == 3000, "Wrong photo count")
                check(db.execute("SELECT COUNT(DISTINCT district) FROM properties").fetchone()[0] == 9, "Wrong district diversity")
                check(db.execute("SELECT COUNT(DISTINCT kind || deal_type) FROM properties p JOIN listings l ON l.property_id=p.id").fetchone()[0] == 4, "Missing property/deal combinations")
            check(len({row["price"] for row in source}) < len(source), "Fixture needs price ties")
            check(len({row["area"] for row in source}) < len(source), "Fixture needs area ties")

            def collect(params, expected):
                found = []
                size = 37
                pages = max(1, (len(expected) + size - 1) // size)
                for page_number in range(1, pages + 1):
                    result = get_json("/api/listings?" + urlencode(dict(params, page=page_number, page_size=size)))
                    check(result["total"] == len(expected), "COUNT disagrees with filters")
                    check(result["total_pages"] == (len(expected) + size - 1) // size, "Wrong total_pages")
                    check(result["count"] == len(result["items"]), "Wrong count")
                    check(result["has_next"] == (page_number < pages), "Wrong has_next")
                    found.extend(item["id"] for item in result["items"])
                check(found == expected, f"Wrong order/page/filter: {params}")
                check(len(found) == len(set(found)), "Duplicate listings across pages")

            for sort, field, direction in [("price_asc", "price", 1), ("price_desc", "price", -1),
                                            ("area_asc", "area", 1), ("area_desc", "area", -1)]:
                expected = [row["id"] for row in sorted(source, key=lambda row: (direction * row[field], row["id"]))]
                collect({"sort": sort}, expected)
                # Совместное действие всех пяти фильтров с каждым из четырёх порядков.
                params = dict(type="rent", district="Кентрон", min_price=200000, max_price=500000, rooms=2, sort=sort)
                matching = [row for row in source if row["deal_type"] == "rent" and row["district"] == "Кентрон"
                            and 200000 <= row["price"] <= 500000 and row["rooms"] == 2]
                check(matching, "Combined-filter fixture must not be empty")
                expected = [row["id"] for row in sorted(matching, key=lambda row: (direction * row[field], row["id"]))]
                collect(params, expected)
            # Включённые границы диапазона, отсутствующий район, пустая и дальняя страницы.
            collect({"min_price": 280000, "max_price": 280000},
                    sorted(row["id"] for row in source if row["price"] == 280000))
            empty = get_json("/api/listings?" + urlencode({"district": "Такого района нет"}))
            check(empty["items"] == [] and empty["total"] == 0 and empty["total_pages"] == 0, "Wrong empty result")
            check(get_json("/api/listings?district=%27%20OR%201%3D1--")["total"] == 0, "District injection executed")
            distant = get_json("/api/listings?page=1000000")
            check(distant["items"] == [] and distant["total"] == 1000, "Wrong out-of-range page")
            check(get_json("/api/listings?page_size=100")["count"] == 100, "Maximum page size failed")
            for query in ["page=0", "page=-1", "page=1.5", "page=abc", "page=", "page=1000001",
                          "page=9999999999999999999999999", "page_size=0", "page_size=101", "page_size=-1",
                          "min_price=-1", "min_price=1.5", "max_price=nan", "max_price=1000000000001",
                          "min_price=300000&max_price=200000", "rooms=0", "rooms=101", "rooms=2.5",
                          "sort=price", "sort=price%20DESC%3BDROP%20TABLE%20listings", "type=", "district=",
                          "district=%00", "district=" + "a" * 101, "unknown=1", "page=1&page=2", "page=1&%70age=2",
                          "page", "min_price", "district=%", "district=%0", "district=%GG"]:
                status, body, _ = request("/api/listings?" + query)
                check(status == 400 and json.loads(body).get("error"), f"Expected understandable 400: {query}")

            detail = get_json("/api/listings/1")
            check(detail["id"] == 1 and detail["property_id"] == 1 and detail["price"] == 280000, "Wrong detail data")
            check(len(detail["photos"]) == 3 and detail["description"], "Incomplete detail")
            for _ in range(2):
                status, body, _ = request("/listings/1")
                check(status == 200 and b'listing.js' in body, "Direct detail/reload failed")
            check(request("/api/listings/99999999")[0] == 404, "Missing detail should be 404")
            check(request("/listings/99999999")[0] == 404, "Missing detail page should be 404")
            for value in ("abc", "0", "-1", "1.5", "99999999999999999999999999"):
                check(request("/api/listings/" + value)[0] == 400, "Invalid id should be 400")

            # Изменение SQLite должно немедленно появиться в API: никакого массива-заглушки.
            with sqlite3.connect(database) as db:
                db.execute("PRAGMA foreign_keys = ON")
                db.execute("UPDATE properties SET description = ? WHERE id = ?", ("Проверка сохранения", 1))
                db.execute("UPDATE listings SET price = ? WHERE id = ?", (321234, 1))
                db.execute("UPDATE listings SET status = ? WHERE id = ?", ("closed", 2))
                cursor = db.execute("""INSERT INTO properties(kind, address, district, area, rooms, floor,
                    description, latitude, longitude) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)""",
                    ("house", "Учебный адрес пользователя", "Аван", 80, 2, 0, "Новый объект", 40.2, 44.5))
                db.execute("INSERT INTO listings(property_id, deal_type, price) VALUES (?, ?, ?)",
                           (cursor.lastrowid, "sale", 20000000))
            edited = get_json("/api/listings")
            first = get_json("/api/listings/1")
            check(first["price"] == 321234 and first["description"] == "Проверка сохранения", "API does not read SQLite")
            check(all(item["id"] != 2 for item in edited["items"]), "Closed listing returned")
            check(get_json("/api/listings/2")["status"] == "closed", "Closed history detail not accessible")
            custom_id = 1001
            check(get_json(f"/api/listings/{custom_id}")["photos"] == [], "Listing without photos must still exist")
            # Новое объявление того же объекта — количество объявлений не ограничено 1000.
            with sqlite3.connect(database) as db:
                db.execute("PRAGMA foreign_keys = ON")
                db.execute("INSERT INTO listings(property_id, deal_type, price) VALUES (?, ?, ?)", (2, "sale", 55000000))
            edited = get_json("/api/listings")
            check(edited["total"] == 1001, "Resale listing was incorrectly limited")
            before = snapshot()
            stop()
            start()
            check(snapshot() == before, "Restart changed, removed or duplicated data")
            check(get_json("/api/health")["properties_count"] == 1001, "Custom property lost")
            check(get_json("/api/listings") == edited, "API changed after restart")
            stop()
            subprocess.run(command + ["--init-db"], check=True, stdout=log, stderr=subprocess.STDOUT, timeout=10)
            check(snapshot() == before, "Explicit initialization changed existing data")
            with sqlite3.connect(database) as db:
                check(db.execute("PRAGMA integrity_check").fetchone()[0] == "ok", "Database is corrupt")
                check(db.execute("PRAGMA foreign_key_check").fetchall() == [], "Broken references")

            # Две независимые новые базы должны содержать одинаковый демонабор.
            # Время создания намеренно не сравниваем: оно отражает реальный запуск.
            def demo_snapshot(path):
                with sqlite3.connect(path) as db:
                    return (
                        db.execute("SELECT * FROM properties ORDER BY id").fetchall(),
                        db.execute("SELECT id, property_id, demo_key, deal_type, price, status FROM listings ORDER BY id").fetchall(),
                        db.execute("SELECT * FROM property_photos ORDER BY id").fetchall(),
                    )
            fresh_paths = [Path(temporary) / "fresh-a.sqlite3", Path(temporary) / "fresh-b.sqlite3"]
            for path in fresh_paths:
                subprocess.run([str(binary), "--root", str(root), "--db", str(path), "--init-db"],
                               check=True, stdout=log, stderr=subprocess.STDOUT, timeout=10)
            check(demo_snapshot(fresh_paths[0]) == demo_snapshot(fresh_paths[1]), "Demo generation is not reproducible")
            print("PASS: HTTP API, all filters, four sorts, stable pagination, detail/404/400, images, restart and preservation")
        except Exception:
            log.flush()
            log.seek(0)
            print(log.read(), file=sys.stderr)
            raise
        finally:
            stop()
