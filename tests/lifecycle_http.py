"""Этап 4: реальные HTTP-запросы, отдельная база версии 3, гонки и сбои транзакций."""
import concurrent.futures
import json
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request


def check(value, message):
    if not value:
        raise AssertionError(message)


binary, root = [Path(arg).resolve() for arg in sys.argv[1:3]]
with tempfile.TemporaryDirectory(prefix="apartment-lifecycle-") as temporary:
    path = Path(temporary) / "test.sqlite3"
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    command = [str(binary), "--root", str(root), "--db", str(path), "--port", str(port)]
    tables = ("properties", "listings", "property_photos", "users", "bookings")
    process = None

    def rows(sql, values=()):
        with sqlite3.connect(path) as db:
            return db.execute(sql, values).fetchall()

    def write(sql, values=()):
        with sqlite3.connect(path) as db:
            db.execute("PRAGMA foreign_keys=ON")
            db.execute(sql, values)

    def snapshot():
        return {t: rows(f"SELECT * FROM {t} ORDER BY id") for t in tables}

    def free(kind):
        return rows("SELECT id FROM listings WHERE deal_type=? AND status='available' AND source_booking_id IS NULL ORDER BY id LIMIT 1", (kind,))[0][0]

    class Client:
        def __init__(self):
            self.cookie = ""
            self.csrf = ""
            self.user = None
            self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

        def request(self, route, method="GET", data=None, csrf=True, origin=base):
            headers = {"Cookie": self.cookie, "Origin": origin, "Content-Type": "application/json"}
            if csrf:
                headers["X-CSRF-Token"] = self.csrf
            req = urllib.request.Request(base + route, method=method, headers=headers,
                data=json.dumps(data if data is not None else {}).encode() if method == "POST" else None)
            try:
                response = self.opener.open(req, timeout=6)
            except urllib.error.HTTPError as error:
                response = error
            with response:
                status, raw, headers = response.code, response.read(), response.headers
            if headers.get("Set-Cookie"):
                self.cookie = headers["Set-Cookie"].split(";", 1)[0]
            result = json.loads(raw) if "application/json" in headers.get("Content-Type", "") else raw
            if isinstance(result, dict) and "csrf_token" in result:
                self.csrf, self.user = result["csrf_token"], result["user"]
            return status, result

        def expect(self, route, status=200, **kwargs):
            actual, result = self.request(route, **kwargs)
            check(actual == status, f"{route}: expected {status}, got {actual}: {result!r}")
            return result

        def sign_in(self, email, register=False):
            self.expect("/api/auth/me")
            data = {"email": email, "password": "Lifecycle-test-password!"}
            if register:
                data["name"] = "Тест истории"
            self.expect("/api/auth/" + ("register" if register else "login"), 201 if register else 200, method="POST", data=data)

        def book(self, listing, status=201):
            return self.expect(f"/api/listings/{listing}/book", status, method="POST")["id"] if status == 201 else self.expect(f"/api/listings/{listing}/book", status, method="POST")

        def release(self, booking, status=200, **kwargs):
            return self.expect(f"/api/bookings/{booking}/release", status, method="POST", **kwargs)

        def resell(self, booking, price=55000000, status=201, **kwargs):
            return self.expect(f"/api/bookings/{booking}/resell", status, method="POST", data={"price": price}, **kwargs)

    def start():
        global process
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        probe = Client()
        for _ in range(100):
            check(process.poll() is None, "Server exited at startup")
            try:
                if probe.request("/api/health")[0] == 200:
                    return
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(.05)
        raise AssertionError("Startup timed out")

    def stop():
        global process
        if process is not None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(timeout=5)
            process = None

    def race(actions):
        barrier = threading.Barrier(2)
        def run(action):
            barrier.wait(timeout=5)
            return action()
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(run, action) for action in actions]
            return [future.result(timeout=10) for future in futures]

    # Подготавливаем старую схему 3 и пользовательские изменения ДО запуска нового сервера.
    with sqlite3.connect(path) as db:
        db.execute("PRAGMA foreign_keys=ON")
        for file in ("001_initial.sql", "002_catalog_indexes.sql", "003_users_bookings.sql"):
            db.executescript((root / "sql" / file).read_text())
        db.executescript("""
            INSERT INTO properties(id,demo_key,kind,address,district,area,rooms,floor,description,latitude,longitude)
            VALUES(5000,'demo-001','apartment','Edited address','Edited district',62,2,4,'Keep my edit',40,44);
            INSERT INTO property_photos(property_id,url,caption,sort_order) VALUES(5000,'/images/kitchen.jpg','Edited caption',0);
            INSERT INTO listings(id,property_id,demo_key,deal_type,price,status,created_at)
            VALUES(6000,5000,'demo-001','rent',123456,'reserved','2026-01-01T00:00:00Z');
            INSERT INTO users(id,name,email,password_hash) VALUES(100,'Legacy user','legacy@example.com','test-only preserved hash');
            INSERT INTO bookings(user_id,listing_id,property_id,price_at_booking,deal_type) VALUES(100,6000,5000,123456,'rent');
        """)
    old = snapshot()
    old_columns = {t: ','.join(row[1] for row in rows(f"PRAGMA table_info({t})")) for t in tables}

    with (Path(temporary) / "server.log").open("w+") as log:
        try:
            start()
            check(rows("PRAGMA user_version") == [(4,)], "Migration 4 not applied")
            for t in tables:
                current = {row[0]: row for row in rows(f"SELECT {old_columns[t]} FROM {t}")}
                check(all(current[row[0]] == row for row in old[t]), "Migration/seed lost old " + t)
            check(rows("SELECT COUNT(*) FROM properties") == [(1000,)], "Demo expansion duplicated legacy object")
            a, a2, b, guest = Client(), Client(), Client(), Client()
            a.sign_in("a@example.com", True); b.sign_in("b@example.com", True); a2.sign_in("a@example.com")
            aid, bid = a.user["id"], b.user["id"]
            rent, sale = free("rent"), free("sale")
            rent_booking, sale_booking = a.book(rent), a.book(sale)
            unchanged = snapshot()
            for booking, action in ((rent_booking, "release"), (sale_booking, "resell")):
                route = f"/api/bookings/{booking}/{action}"
                data = {} if action == "release" else {"price": 55000000}
                guest.expect(route, 401, method="POST", data=data)
                b.expect(route, 403, method="POST", data=data)
                a.expect(route, 403, method="POST", data=data, csrf=False)
                a.expect(route, 403, method="POST", data=data, origin="https://attacker.example")
                check(a.request(route)[0] in (404, 405), "GET unexpectedly allows a state-changing action")
                a.expect(route + "?user_id=" + str(aid), 400, method="POST", data=data)
                a.expect(route, 400, method="POST", data={**data, "user_id": bid})
            a.release(sale_booking, 409); a.resell(rent_booking, status=409)
            a.release(999999, 404); a.resell(999999, status=404)
            a.release("bad", 400); a.resell("9223372036854775808", status=400)
            for price in (0, -1, 1.5, 1.0, 1e6, True, None, [], {}, "abc", "0", "-1", "1.5", "1e6", " 50", "+50", "5 000", "", 1000000000001, 2**63, "9"*100, "1000000000001"):
                a.resell(sale_booking, price, 400)
            a.expect(f"/api/bookings/{sale_booking}/resell", 400, method="POST", data={})
            check(snapshot() == unchanged, "Rejected action changed data")

            # Освобождение, история, исходная цена и запоздалый запрос после новой аренды.
            saved_price = rows("SELECT price_at_booking FROM bookings WHERE id=?", (rent_booking,))[0][0]
            write("UPDATE listings SET price=765432 WHERE id=?", (rent,))
            a.release(rent_booking)
            history = rows("SELECT status,ended_at,price_at_booking FROM bookings WHERE id=?", (rent_booking,))[0]
            check(history[0] == "released" and history[1] and history[2] == saved_price, "Release lost history/price/date")
            check(rows("SELECT status FROM listings WHERE id=?", (rent,)) == [("available",)], "Release did not reopen original listing")
            new_rent_booking = b.book(rent)
            stable = snapshot()
            a.release(rent_booking, 409)
            check(snapshot() == stable, "Late release touched new tenant or ended_at")
            check(rows("SELECT user_id,status FROM bookings WHERE id=?", (new_rent_booking,)) == [(bid, "active")], "New tenant lost booking")

            # Перепродажа с происхождением, новой датой/id и без копирования недвижимости/фото.
            write("UPDATE listings SET created_at='2001-01-01T00:00:00Z' WHERE id=?", (sale,))
            original_listing = rows("SELECT * FROM listings WHERE id=?", (sale,))[0]
            photos_before, properties_before = rows("SELECT * FROM property_photos"), rows("SELECT * FROM properties")
            new_sale = a.resell(sale_booking)["listing_id"]
            new = a.expect(f"/api/listings/{new_sale}")
            check(new_sale != sale and new["property_id"] == original_listing[1] and new["price"] == 55000000 and
                  new["status"] == "available" and new["deal_type"] == "sale" and new["seller_user_id"] == aid and
                  new["source_booking_id"] == sale_booking and new["created_at"] != original_listing[6], "Wrong resale attributes/origin")
            check(rows("SELECT demo_key FROM listings WHERE id=?", (new_sale,)) == [(None,)], "Resale has a demo key")
            expected_old = list(original_listing); expected_old[5] = "closed"
            check(rows("SELECT * FROM listings WHERE id=?", (sale,))[0] == tuple(expected_old), "Old listing history changed")
            check(rows("SELECT * FROM property_photos") == photos_before and rows("SELECT * FROM properties") == properties_before, "Resale copied/changed physical property/photos")
            record = next(item for item in a.expect("/api/bookings")["items"] if item["id"] == sale_booking)
            check(record["status"] == "resold" and record["ended_at"] and record["resale_listing_id"] == new_sale and record["price_at_booking"] == original_listing[4], "Resale history or link missing")
            check(a.expect(f"/api/listings/{sale}")["status"] == "closed", "Old detail missing")
            a.expect(f"/listings/{sale}")
            # В каталоге нет закрытого id, новое доступное объявление учитывается в общем COUNT.
            available = []
            for page in range(1, 12):
                available += [item["id"] for item in a.expect(f"/api/listings?page_size=100&page={page}")["items"]]
            check(sale not in available and new_sale in available, "Catalog contains closed/misses new listing")
            stable = snapshot(); a.resell(sale_booking, status=409); a.book(new_sale, 403)
            check(snapshot() == stable, "Repeat/self purchase changed data")
            bought_again = b.book(new_sale)
            stable = snapshot(); a.resell(sale_booking, status=409)
            check(snapshot() == stable, "Old seller affected new owner")
            third_sale = b.resell(bought_again, "60000000")["listing_id"]
            check(rows("SELECT property_id,seller_user_id,source_booking_id FROM listings WHERE id=?", (third_sale,)) == [(new["property_id"], bid, bought_again)], "Chain of resale broken")
            b.book(third_sale, 403); a.book(third_sale)
            # Частичный UNIQUE действует и против прямой вставки второго объявления из того же бронирования.
            try:
                write("INSERT INTO listings(property_id,deal_type,price,status,seller_user_id,source_booking_id) VALUES(?,'sale',1,'closed',?,?)", (new["property_id"], aid, sale_booking))
                raise AssertionError("Duplicate resale origin accepted by database")
            except sqlite3.IntegrityError as error:
                check("source_booking_id" in str(error), "Unexpected constraint rejected duplicate origin")

            # Пять пар конкурентных освобождений и пять пар перепродаж, независимые сессии одного владельца.
            for _ in range(5):
                listing = free("rent"); booking = a.book(listing)
                route = f"/api/bookings/{booking}/release"
                result = race([lambda: a.request(route, "POST"), lambda: a2.request(route, "POST")])
                check(sorted(item[0] for item in result) == [200, 409], "Concurrent release must be 200+409")
                check(rows("SELECT status FROM listings WHERE id=?", (listing,)) == [("available",)], "Concurrent release did not reopen listing")
                b.book(listing); stable = snapshot(); a.release(booking, 409)
                check(snapshot() == stable, "Delayed concurrent release changed new booking")
                listing = free("sale"); booking = a.book(listing)
                route = f"/api/bookings/{booking}/resell"
                result = race([lambda: a.request(route, "POST", {"price": 44000000}), lambda: a2.request(route, "POST", {"price": 45000000})])
                check(sorted(item[0] for item in result) == [201, 409], "Concurrent resale must be 201+409")
                expected_price = 44000000 if result[0][0] == 201 else 45000000
                check(rows("SELECT price,status,seller_user_id FROM listings WHERE source_booking_id=?", (booking,)) == [(expected_price, "available", aid)], "Resale race duplicated listing or lost winning price")
                check(rows("SELECT COUNT(*) FROM listings WHERE property_id=(SELECT property_id FROM bookings WHERE id=?) AND status IN ('available','reserved')", (booking,)) == [(1,)], "Multiple open listings")

            # Граничные корректные цены, числовой JSON и строковое представление.
            for price in (1, "1000000000000"):
                booking = a.book(free("sale"))
                listing = a.resell(booking, price)["listing_id"]
                check(rows("SELECT price FROM listings WHERE id=?", (listing,)) == [(int(price),)], "Boundary price rejected/changed")

            # Принудительный сбой после завершения бронирования, до обновления объявления.
            for kind, action in (("rent", "release"), ("sale", "resell")):
                listing = free(kind); booking = a.book(listing)
                write(f"CREATE TRIGGER fail_listing BEFORE UPDATE OF status ON listings WHEN OLD.id={listing} BEGIN SELECT RAISE(ABORT,'test failure'); END")
                stable = snapshot()
                if action == "release": a.release(booking, 500)
                else: a.resell(booking, status=500)
                check(snapshot() == stable, "Failure at listing update did not roll back booking")
                write("DROP TRIGGER fail_listing")
                # Ноль изменённых строк — тоже причина отката, даже без SQLite-исключения.
                write("UPDATE listings SET status='available' WHERE id=?", (listing,))
                stable = snapshot()
                if action == "release": a.release(booking, 409)
                else: a.resell(booking, status=409)
                check(snapshot() == stable, "Expected-status conflict committed partial completion")
                write("UPDATE listings SET status='reserved' WHERE id=?", (listing,))
                # Длительная блокировка: 503, а не успех/конфликт и не частичная запись.
                stable = snapshot()
                with sqlite3.connect(path) as lock:
                    lock.execute("BEGIN IMMEDIATE")
                    started = time.monotonic()
                    if action == "release": a.release(booking, 503)
                    else: a.resell(booking, status=503)
                    check(.5 < time.monotonic()-started < 4, "Unbounded busy waiting")
                    lock.rollback()
                check(snapshot() == stable, "Busy request modified data")
                if action == "release": a.release(booking)
                else:
                    # Сбой последнего шага: исходное объявление уже закрыто внутри транзакции.
                    write(f"CREATE TRIGGER fail_resale BEFORE INSERT ON listings WHEN NEW.source_booking_id={booking} BEGIN SELECT RAISE(ABORT,'test final insert failure'); END")
                    stable = snapshot(); a.resell(booking, status=500)
                    check(snapshot() == stable, "Failed resale INSERT left old listing closed or booking finished")
                    write("DROP TRIGGER fail_resale"); a.resell(booking)

            a_bookings, b_bookings = a.expect("/api/bookings"), b.expect("/api/bookings")
            check(not ({item['id'] for item in a_bookings['items']} & {item['id'] for item in b_bookings['items']}), "Another user's history leaked")
            before = snapshot()
            stop(); start()
            check(snapshot() == before, "Restart or generator changed history/origin/users/objects")
            a.expect("/api/bookings", 401)
            a.sign_in("a@example.com"); b.sign_in("b@example.com")
            check(a.expect("/api/bookings") == a_bookings and b.expect("/api/bookings") == b_bookings, "History not preserved after login")
            stop()
            subprocess.run(command + ["--init-db"], check=True, stdout=log, stderr=subprocess.STDOUT, timeout=10)
            check(snapshot() == before, "Explicit initialization duplicated/changed resales")
            check(rows("PRAGMA integrity_check") == [("ok",)] and rows("PRAGMA foreign_key_check") == [], "Integrity failure")
            check(rows("SELECT listing_id FROM bookings WHERE status='active' GROUP BY listing_id HAVING COUNT(*)>1") == [], "Duplicate active bookings")
            print("PASS: migration 3->4 preservation, release/rebook/history, ownership/types/CSRF, integer price bounds, resale provenance/self-purchase/chain")
            print("PASS: 5 HTTP release races 200+409 and 5 resale races 201+409, late repeats, rollback at each step, busy retries, restart and generator")
        except Exception:
            log.flush(); log.seek(0); print(log.read(), file=sys.stderr)
            raise
        finally:
            stop()
