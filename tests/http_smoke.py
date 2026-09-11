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
            check(health["properties_count"] == 12 and health["listings_count"] == 12, "Wrong initial counts")
            catalog = get_json("/api/listings")
            check(catalog["count"] == len(catalog["items"]) == 12, "Expected 12 API listings")
            check(get_json("/api/listings?type=rent")["count"] == 6, "Wrong rent count")
            check(get_json("/api/listings?type=sale")["count"] == 6, "Wrong sale count")
            check(request("/api/listings?type=wrong")[0] == 400, "Invalid type accepted")
            check(request("/api/listings?type=%27%20OR%201%3D1--")[0] == 400, "Injection input accepted")
            check(request("/api/listings", "POST")[0] == 405, "POST unexpectedly accepted")
            check(request("/data/apartments.sqlite3")[0] == 404, "Database exposed as static file")
            page = request("/")
            check(page[0] == 200 and 'Свой адрес'.encode() in page[1], "Catalog HTML not served")
            for asset in ("/app.js", "/styles.css", "/images/favicon.svg", "/images/placeholder.svg"):
                check(request(asset)[0] == 200, f"Missing asset: {asset}")
            for item in catalog["items"]:
                check(len(item["photos"]) == 3, "Expected three photos per property")
                check(item["price"] > 0 and item["address"] and item["description"], "Incomplete listing")
            for url in {photo["url"] for item in catalog["items"] for photo in item["photos"]}:
                status, content, headers = request(url)
                check(status == 200 and content.startswith(b"\xff\xd8"), f"Missing JPEG: {url}")

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
            first = next(item for item in edited["items"] if item["id"] == 1)
            check(first["price"] == 321234 and first["description"] == "Проверка сохранения", "API does not read SQLite")
            check(all(item["id"] != 2 for item in edited["items"]), "Closed listing returned")
            before = snapshot()
            stop()
            start()
            check(snapshot() == before, "Restart changed, removed or duplicated data")
            check(get_json("/api/health")["properties_count"] == 13, "Custom property lost")
            check(get_json("/api/listings") == edited, "API changed after restart")
            stop()
            subprocess.run(command + ["--init-db"], check=True, stdout=log, stderr=subprocess.STDOUT, timeout=10)
            check(snapshot() == before, "Explicit initialization changed existing data")
            with sqlite3.connect(database) as db:
                check(db.execute("PRAGMA integrity_check").fetchone()[0] == "ok", "Database is corrupt")
                check(db.execute("PRAGMA foreign_key_check").fetchall() == [], "Broken references")
            print("PASS: HTTP API, assets, SQLite changes, process restart, custom data and repeat initialization")
        except Exception:
            log.flush()
            log.seek(0)
            print(log.read(), file=sys.stderr)
            raise
        finally:
            stop()
