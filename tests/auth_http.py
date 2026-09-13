"""HTTP-проверки этапа 3. Только отдельная временная база, реальные C++ обработчики."""
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


def check(condition, message):
    if not condition:
        raise AssertionError(message)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


binary, root = (Path(value).resolve() for value in sys.argv[1:3])
password = "Test-only password 2026!"
with tempfile.TemporaryDirectory(prefix="apartment-auth-") as temporary:
    database = Path(temporary) / "auth.sqlite3"
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    command = [str(binary), "--root", str(root), "--db", str(database), "--port", str(port)]
    process = None
    issued_secrets = []

    class Client:
        def __init__(self):
            self.cookie = ""
            self.csrf = ""
            self.user = None
            self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

        def request(self, path, method="GET", data=None, csrf=True, origin=base, extra=None):
            headers = {"Cookie": self.cookie}
            if method == "POST":
                headers["Content-Type"] = "application/json"
                if csrf:
                    headers["X-CSRF-Token"] = self.csrf
                if origin is not None:
                    headers["Origin"] = origin
            if extra:
                headers.update(extra)
            payload = json.dumps(data if data is not None else {}).encode() if method == "POST" else None
            req = urllib.request.Request(base + path, data=payload, method=method, headers=headers)
            try:
                response = self.opener.open(req, timeout=6)
            except urllib.error.HTTPError as error:
                response = error
            with response:
                status, raw, response_headers = response.code, response.read(), response.headers
            cookie = response_headers.get("Set-Cookie")
            if cookie:
                self.cookie = cookie.split(";", 1)[0]
                issued_secrets.append(self.cookie.partition("=")[2])
            body = json.loads(raw) if "application/json" in response_headers.get("Content-Type", "") else raw
            if isinstance(body, dict) and "csrf_token" in body:
                self.csrf = body["csrf_token"]
                self.user = body["user"]
                issued_secrets.append(self.csrf)
            return status, body, response_headers

        def expect(self, path, status=200, **kwargs):
            actual, body, headers = self.request(path, **kwargs)
            check(actual == status, f"{path}: expected {status}, got {actual}: {body!r}")
            return body, headers

        def prepare(self):
            return self.expect("/api/auth/me")

        def register(self, email, name="Тестовый пользователь"):
            return self.expect("/api/auth/register", 201, method="POST", data={"name": name, "email": email, "password": password})

        def login(self, email, value=password, status=200):
            return self.expect("/api/auth/login", status, method="POST", data={"email": email, "password": value})

        def book(self, listing, status=201, **kwargs):
            return self.expect(f"/api/listings/{listing}/book", status, method="POST", **kwargs)

    def start(extra=()):
        global process
        process = subprocess.Popen(command + list(extra), stdout=log, stderr=subprocess.STDOUT)
        probe = Client()
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            check(process.poll() is None, "Server exited on startup")
            try:
                if probe.request("/api/health")[0] == 200:
                    return
            except (urllib.error.URLError, TimeoutError, ConnectionError):
                pass
            time.sleep(.05)
        raise AssertionError("Server startup timed out")

    def stop():
        global process
        if process:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            process = None

    def rows(sql, params=()):
        with sqlite3.connect(database) as db:
            return db.execute(sql, params).fetchall()

    def write(sql):
        with sqlite3.connect(database) as db:
            db.execute("PRAGMA foreign_keys=ON")
            db.executescript(sql)

    def snapshot():
        return {table: rows(f"SELECT * FROM {table} ORDER BY id")
                for table in ("properties", "listings", "property_photos", "users", "bookings")}

    def simultaneous(actions):
        # Оба клиента готовы ДО отправки. У каждого отдельное HTTP-соединение.
        barrier = threading.Barrier(len(actions))
        def run(action):
            barrier.wait(timeout=5)
            return action()
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(actions)) as pool:
            futures = [pool.submit(run, action) for action in actions]
            return [future.result(timeout=10) for future in futures]

    with (Path(temporary) / "server.log").open("w+") as log:
        try:
            start()
            guest = Client()
            guest.book(1, 401)
            guest.expect("/api/bookings", 401)
            guest.expect("/bookings.html", 302)
            _, headers = guest.expect("/my-bookings", 302)
            check(headers["Location"] == "/login?next=%2Fmy-bookings", "Private page not gated")
            state, headers = guest.prepare()
            check(state["user"] is None, "Guest has a user")
            cookie = headers["Set-Cookie"].lower()
            check("httponly" in cookie and "samesite=lax" in cookie and "path=/" in cookie and
                  "max-age=43200" in cookie and "secure" not in cookie, "Wrong local session cookie flags")
            data = {"name": "Алиса", "email": "alice@example.com", "password": password}
            guest.expect("/api/auth/register", 403, method="POST", data=data, csrf=False)
            guest.expect("/api/auth/register", 403, method="POST", data=data, origin="https://attacker.example")
            guest.expect("/api/auth/login", 403, method="POST", data={"email": data["email"], "password": password}, csrf=False)
            guest.expect("/api/auth/register", 400, method="POST", data={**data, "password": "short"})
            guest.expect("/api/auth/register", 400, method="POST", data={**data, "email": "bad-address"})
            guest.expect("/api/auth/register", 400, method="POST", data={**data, "user_id": 10})
            guest.expect("/api/auth/login", 400, method="POST", data={"email": 42, "password": password})
            for path in ("/login?next=%2Flistings%2F1", "/register", "/auth-client.js", "/auth-page.js", "/bookings.js"):
                guest.expect(path)

            alice = guest
            old_guest = Client()
            old_guest.cookie, old_guest.csrf = alice.cookie, alice.csrf
            alice.register("  ALICE@Example.COM  ", "Алиса")
            alice_id = alice.user["id"]
            check(alice.user["email"] == "alice@example.com", "Email not normalized")
            check(alice.cookie != old_guest.cookie and alice.csrf != old_guest.csrf, "Registration did not rotate session")
            old_guest.expect("/api/bookings", 401)
            old_guest.expect("/api/auth/login", 403, method="POST", data={"email": "alice@example.com", "password": password})
            user_json = alice.prepare()[0]
            check(set(user_json["user"]) == {"id", "name", "email", "created_at"}, "Private fields exposed")
            stored_hash = rows("SELECT password_hash FROM users WHERE id=?", (alice_id,))[0][0]
            check(stored_hash.startswith("$argon2id$"), "Password not Argon2id")

            bob = Client(); bob.prepare()
            bob.expect("/api/auth/register", 409, method="POST", data=data)
            bob.register("bob@example.com", "Боб")
            bob_id = bob.user["id"]
            check(rows("SELECT password_hash FROM users WHERE id=?", (bob_id,))[0][0] != stored_hash, "Same password has same hash/salt")
            bob.expect("/api/bookings?user_id=" + str(alice_id), 400)
            alice.expect("/my-bookings")

            login = Client(); login.prepare()
            login.login("alice@example.com", "Incorrect password", 401)
            old_login_cookie, old_login_csrf = login.cookie, login.csrf
            login.login(" ALICE@example.com ")
            check(login.cookie != old_login_cookie and login.csrf != old_login_csrf, "Login did not rotate session")
            stale = Client(); stale.cookie, stale.csrf = old_login_cookie, old_login_csrf
            stale.expect("/api/bookings", 401)
            login.expect("/api/auth/logout", 403, method="POST", csrf=False)
            replay = Client(); replay.cookie, replay.csrf = login.cookie, login.csrf
            _, headers = login.expect("/api/auth/logout", method="POST")
            check("max-age=0" in headers["Set-Cookie"].lower(), "Cookie not expired on logout")
            replay.expect("/api/bookings", 401)
            replay.book(1, 401)
            check(replay.prepare()[0]["user"] is None, "Logged-out server session still valid")

            for csrf_headers in ({"X-CSRF-Token": "0" * 64}, {"Origin": "https://attacker.example"}, {"Sec-Fetch-Site": "cross-site"}):
                alice.book(1, 403, extra=csrf_headers)
            alice.book(1, 403, csrf=False)
            alice.book(1, 400, data={"user_id": bob_id})
            alice.book(1, 400, data={"price": 1})
            alice.book(999999, 404)
            alice.book("bad", 400)
            original_price = rows("SELECT price FROM listings WHERE id=1")[0][0]
            alice.book(1)
            alice.book(1, 409)
            bob.book(1, 409)
            check(rows("SELECT user_id,price_at_booking,status FROM bookings WHERE listing_id=1") ==
                  [(alice_id, original_price, "active")], "Booking owner/price/count wrong")
            check(alice.expect("/api/listings/1")[0]["status"] == "reserved", "Status not reserved")
            alice.expect("/listings/1")
            check(alice.expect("/api/listings")[0]["total"] == 999, "Reserved listing remains in available catalog")
            check(bob.expect("/api/bookings")[0]["items"] == [], "Bob can see Alice's bookings")
            write("UPDATE listings SET price=987654 WHERE id=1;")
            item = alice.expect("/api/bookings")[0]["items"][0]
            check(item["price_at_booking"] == original_price and item["listing"]["price"] == 987654 and len(item["listing"]["photos"]) == 3,
                  "Price snapshot or property/photos lost")

            # Реальные гонки повторены на пяти свободных объявлениях: сервер имеет 2 рабочих потока.
            for listing in range(21, 26):
                results = simultaneous([
                    lambda n=listing: alice.request(f"/api/listings/{n}/book", "POST"),
                    lambda n=listing: bob.request(f"/api/listings/{n}/book", "POST")])
                check(sorted(result[0] for result in results) == [201, 409], "Concurrent booking must return exactly 201 + 409")
                winner = alice_id if results[0][0] == 201 else bob_id
                loser = results[1] if results[0][0] == 201 else results[0]
                check(loser[1]["error"] == "Это жильё уже забронировали", "Wrong conflict message")
                if listing == 21:
                    print("Concurrent HTTP evidence:", sorted(result[0] for result in results), "loser body:", json.dumps(loser[1], ensure_ascii=False))
                check(rows("SELECT user_id,status FROM bookings WHERE listing_id=?", (listing,)) == [(winner, "active")], "Wrong winner or duplicate booking")
                check(rows("SELECT status FROM listings WHERE id=?", (listing,)) == [("reserved",)], "Race left wrong listing status")

            # Ошибка INSERT наступает ПОСЛЕ условного UPDATE. Обе операции должны откатиться.
            write("CREATE TRIGGER fail_booking BEFORE INSERT ON bookings WHEN NEW.listing_id=40 BEGIN SELECT RAISE(ABORT,'test insert failure'); END;")
            alice.book(40, 500)
            check(rows("SELECT status FROM listings WHERE id=40") == [("available",)] and not rows("SELECT id FROM bookings WHERE listing_id=40"), "Failed INSERT did not roll back UPDATE")
            write("DROP TRIGGER fail_booking;")
            alice.book(40)

            with sqlite3.connect(database) as lock:
                lock.execute("BEGIN IMMEDIATE")
                started = time.monotonic()
                bob.book(41, 503)
                elapsed = time.monotonic() - started
                check(.5 < elapsed < 4, "Busy waiting is not bounded")
                check(lock.execute("SELECT status FROM listings WHERE id=41").fetchone()[0] == "available", "Busy request changed listing")
                check(lock.execute("SELECT COUNT(*) FROM bookings WHERE listing_id=41").fetchone()[0] == 0, "Busy request created booking")
                lock.rollback()
            bob.book(41)
            # Краткая блокировка снимается в пределах бюджета: запрос может успешно продолжить.
            with sqlite3.connect(database) as lock, concurrent.futures.ThreadPoolExecutor() as pool:
                lock.execute("BEGIN IMMEDIATE")
                pending = pool.submit(bob.book, 42)
                time.sleep(.35)
                lock.rollback()
                pending.result(timeout=5)

            # Дубликат email разрешается ограничением UNIQUE, в том числе при гонке.
            reg_a, reg_b = Client(), Client()
            reg_a.prepare(); reg_b.prepare()
            reg_data = {"name": "Гонка email", "email": "race@example.com", "password": password}
            results = simultaneous([
                lambda: reg_a.request("/api/auth/register", "POST", reg_data),
                lambda: reg_b.request("/api/auth/register", "POST", {**reg_data, "email": "RACE@EXAMPLE.COM"})])
            check(sorted(result[0] for result in results) == [201, 409], "Concurrent registration duplicated email")
            check(rows("SELECT COUNT(*) FROM users WHERE email='race@example.com'")[0][0] == 1, "Duplicate normalized email rows")

            limited = Client(); limited.prepare()
            for _ in range(5):
                limited.login("absent@example.com", status=401)
            limited.login("absent@example.com", status=429)
            # Другой email остаётся доступным при ещё не исчерпанном IP-лимите.
            limited.login("different@example.com", status=401)

            before = snapshot()
            saved_alice = alice.expect("/api/bookings")[0]
            saved_bob = bob.expect("/api/bookings")[0]
            check(all(item["id"] not in {b["id"] for b in saved_bob["items"]} for item in saved_alice["items"]), "Private bookings overlap")
            stop(); start()
            check(snapshot() == before, "Restart/seed changed saved data")
            alice.expect("/api/bookings", 401)
            bob.expect("/api/bookings", 401)
            check(alice.prepare()[0]["user"] is None, "Sessions unexpectedly persisted after restart")
            bob.prepare()
            alice.login("alice@example.com"); bob.login("bob@example.com")
            check(alice.expect("/api/bookings")[0] == saved_alice and bob.expect("/api/bookings")[0] == saved_bob, "Bookings not restored after login/restart")
            stop()
            subprocess.run(command + ["--init-db"], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=10)
            check(snapshot() == before, "Repeated initialization changed users/bookings")
            check(rows("PRAGMA integrity_check") == [("ok",)] and rows("PRAGMA foreign_key_check") == [], "Database integrity failed")
            start(["--secure-cookies", "--origin", "https://housing.example"])
            secure = Client()
            _, headers = secure.prepare()
            check("secure" in headers["Set-Cookie"].lower(), "HTTPS configuration did not set Secure")
            secure.expect("/api/auth/login", 403, method="POST", data={"email": "alice@example.com", "password": password})
            secure.login("alice@example.com", status=403)  # HTTP origin не совпадает с настроенным HTTPS.
            secure.expect("/api/auth/login", method="POST", data={"email": "alice@example.com", "password": password}, origin="https://housing.example")
            stop()
            log.flush(); log.seek(0); text = log.read()
            check(password not in text and stored_hash not in text and not any(value in text for value in issued_secrets if value), "Sensitive values leaked to logs")
            print("PASS: HTTP auth, normalization/email race, Argon2id, session rotation/logout/restart, CSRF, rate limits, private bookings")
            print("PASS: 5 synchronized booking races (201+409), owner/status/count, rollback, bounded busy/retry, persistence, cookie flags")
        except Exception:
            log.flush(); log.seek(0); print(log.read(), file=sys.stderr)
            raise
        finally:
            stop()
