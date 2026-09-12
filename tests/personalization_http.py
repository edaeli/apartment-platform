"""Этап 5: временная база версии 4, настоящие HTTP-запросы и конкурентные личные действия."""
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
with tempfile.TemporaryDirectory(prefix="apartment-personal-") as temporary:
    path = Path(temporary) / "test.sqlite3"
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    command = [str(binary), "--root", str(root), "--db", str(path), "--port", str(port)]
    tables = ("properties", "listings", "property_photos", "users", "bookings", "favorites", "listing_views")
    process = None

    def rows(sql, values=()):
        with sqlite3.connect(path) as db:
            return db.execute(sql, values).fetchall()

    def write(sql, values=()):
        with sqlite3.connect(path) as db:
            db.execute("PRAGMA foreign_keys=ON")
            db.execute(sql, values)

    def snapshot():
        return {t: rows(f"SELECT * FROM {t} ORDER BY " + ("user_id,listing_id" if t in ("favorites", "listing_views") else "id")) for t in tables}

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
                data=json.dumps(data if data is not None else {}).encode() if method in ("POST", "PUT", "DELETE") else None)
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
            data = {"email": email, "password": "Personal-test-password!"}
            if register:
                data["name"] = "Тест предпочтений"
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

    # Схема 4 с уже совершённой перепродажей. Новая миграция должна сохранить её.
    with sqlite3.connect(path) as db:
        db.execute("PRAGMA foreign_keys=ON")
        for file in ("001_initial.sql", "002_catalog_indexes.sql", "003_users_bookings.sql", "004_resale_origin.sql"):
            db.executescript((root / "sql" / file).read_text())
        db.executescript("""
            INSERT INTO users(id,name,email,password_hash) VALUES(777,'Legacy user','legacy@example.com','preserved test hash');
            INSERT INTO properties(id,demo_key,kind,address,district,area,rooms,floor,description,latitude,longitude)
            VALUES(8000,'demo-001','apartment','Preserve address','Preserve district',60,2,3,'Preserve description',40,44);
            INSERT INTO property_photos(property_id,url,caption,sort_order) VALUES(8000,'/images/kitchen.jpg','Preserve photo',0);
            INSERT INTO listings(id,property_id,demo_key,deal_type,price,status)
            VALUES(9000,8000,'demo-001','sale',40000000,'closed');
            INSERT INTO bookings(id,user_id,listing_id,property_id,price_at_booking,deal_type,status,ended_at)
            VALUES(888,777,9000,8000,40000000,'sale','resold','2026-09-01T00:00:00Z');
            INSERT INTO listings(id,property_id,deal_type,price,status,seller_user_id,source_booking_id)
            VALUES(9001,8000,'sale',50000000,'available',777,888);
        """)
    legacy = {t: rows(f"SELECT * FROM {t}") for t in tables[:5]}
    with (Path(temporary) / "server.log").open("w+") as log:
        try:
            start()
            check(rows("PRAGMA user_version") == [(5,)], "Migration 5 not applied")
            for table, records in legacy.items():
                current = {r[0]: r for r in rows(f"SELECT * FROM {table}")}
                check(all(current[r[0]] == r for r in records), "Migration lost stage4 " + table)
            check(rows("SELECT COUNT(*) FROM properties") == [(1000,)], "Seed duplicated property")
            a, a2, b, guest = Client(), Client(), Client(), Client()
            a.sign_in("a@example.com", True); a2.sign_in("a@example.com"); b.sign_in("b@example.com", True)
            rent, sale = free("rent"), free("sale")
            def favorite(client, listing, saved=True, status=200, **kwargs):
                return client.expect(f"/api/favorites/{listing}", status, method="PUT" if saved else "DELETE", **kwargs)
            def view(client, listing, status=200, **kwargs):
                return client.expect(f"/api/listings/{listing}/view", status, method="POST", **kwargs)
            for route in ("/api/favorites", "/api/views"):
                guest.expect(route, 401)
                b.expect(route + "?user_id=" + str(a.user['id']), 400)
                for params in ("page=0", "page=-1", "page=1.5", "page=2&page=3", "page_size=101", "page_size=0", "page=9999999999999999999999", "page=1&", "page=abc", "page=%xx", "page=1&&page_size=2"):
                    a.expect(route + "?" + params, 400)
            for method, route in (("PUT", f"/api/favorites/{rent}"), ("DELETE", f"/api/favorites/{rent}"), ("POST", f"/api/listings/{rent}/view")):
                guest.expect(route, 401, method=method)
                a.expect(route, 403, method=method, csrf=False)
                a.expect(route, 403, method=method, origin="https://other.example")
                a.expect(route, 400, method=method, data={"user_id": b.user['id']})
                a.expect(route+"?user_id=1", 400, method=method)
                a.expect(route.rsplit('/', 1)[0] + '/999999' if method != 'POST' else '/api/listings/999999/view', 404, method=method)
            a.expect("/api/favorites/nope", 400, method="PUT")
            view(a, "9223372036854775808", 400)
            check(rows("SELECT * FROM favorites") == [] and rows("SELECT * FROM listing_views") == [], "Rejected mutation wrote data")
            baseline = guest.expect("/api/recommendations")
            check(not baseline['personalized'] and baseline['count'] == 6, "Guest fallback incorrect")
            check(not b.expect("/api/recommendations")['personalized'], "New user falsely personalized")
            for param in ("limit=0", "limit=25", "limit=1&limit=2", "user_id=1", "limit=1.5"):
                a.expect('/api/recommendations?' + param, 400)
            # GET подробностей и каталога ничего не записывает в историю.
            for _ in range(2):
                a.expect(f"/api/listings/{rent}"); a.expect(f"/listings/{rent}"); a.expect('/api/listings')
            check(rows("SELECT * FROM listing_views") == [], "GET silently recorded a view")
            favorite(a, rent); stable=rows("SELECT * FROM favorites")
            favorite(a, rent)
            check(rows("SELECT * FROM favorites") == stable, "Favorite repeat changed date or created duplicate")
            check(a.expect(f"/api/listings/{rent}")['is_favorite'], "Detail favorite missing")
            check(not b.expect(f"/api/listings/{rent}")['is_favorite'], "Favorite flag leaked to other user")
            check(b.expect('/api/favorites')['total'] == 0, "Another user's favorites leaked")
            favorite(b, rent, False)
            check(rows("SELECT * FROM favorites") == stable, "Other user removed Alice favorite")
            try:
                write("INSERT INTO favorites(user_id,listing_id) VALUES(?,?)", (a.user['id'], rent))
                raise AssertionError("Duplicate pair accepted by DB")
            except sqlite3.IntegrityError:
                pass
            favorite(a, rent, False); favorite(a, rent, False)
            for _ in range(5):
                route=f'/api/favorites/{rent}'
                for method, expected in (('PUT',1),('DELETE',0)):
                    result=race([lambda: a.request(route,method), lambda: a2.request(route,method)])
                    check([r[0] for r in result] == [200,200], 'Idempotent race should succeed twice')
                    check(rows('SELECT COUNT(*) FROM favorites WHERE user_id=? AND listing_id=?', (a.user['id'],rent)) == [(expected,)], 'Favorite race duplicated/missed state')
            # Конкурентное открытие одной подробной страницы: только один учтённый просмотр.
            result=race([lambda: a.request(f'/api/listings/{rent}/view','POST'), lambda: a2.request(f'/api/listings/{rent}/view','POST')])
            check([r[0] for r in result] == [200,200] and sum(r[1]['counted'] for r in result)==1, 'Concurrent view counted twice')
            stable=rows('SELECT * FROM listing_views')
            check(not view(a,rent)['counted'] and rows('SELECT * FROM listing_views')==stable, 'View cooldown changed count/time')
            write("UPDATE listing_views SET last_viewed_at=CAST(strftime('%s','now') AS INTEGER)-1801 WHERE user_id=? AND listing_id=?", (a.user['id'],rent))
            check(view(a,rent)['counted'], 'View not counted after cooldown')
            check(a.expect('/api/views')['items'][0]['view_count']==2, 'View count incorrect')
            check(b.expect('/api/views')['total']==0, 'Private history leaked')
            # Удаление чужой истории отсутствует; подмена времени/счётчика отвергается.
            a.expect(f'/api/listings/{rent}/view',400,method='POST',data={'last_viewed_at':0,'view_count':999})
            status,_=b.request('/api/views', 'DELETE')
            check(status in (404,405), 'History deletion endpoint should not exist')
            # Пагинация сохраняет закрытое исходное объявление и фотографии.
            ids=[r[0] for r in rows('SELECT id FROM listings ORDER BY id LIMIT 30')]
            for listing in ids: favorite(a,listing)
            favorite(a,rent); favorite(a,sale)
            expected=[r[0] for r in rows('SELECT listing_id FROM favorites WHERE user_id=? ORDER BY created_at DESC, listing_id DESC',(a.user['id'],))]
            seen=[]
            for page in range(1,8):
                data=a.expect(f'/api/favorites?page_size=5&page={page}')
                check(data['total']==len(expected) and data['page_size']==5, 'Wrong favorite page metadata')
                seen += [r['id'] for r in data['items']]
            check(seen==expected and len(set(seen))==len(expected), 'Favorites pagination unstable')
            booking=a.book(rent); a.release(booking); b.book(rent)
            purchase=a.book(sale); new_sale=a.resell(purchase)['listing_id']
            favorites=a.expect('/api/favorites?page_size=100')['items']
            statuses={r['id']:r['status'] for r in favorites}
            check(statuses[rent]=='reserved' and statuses[sale]=='closed' and new_sale not in statuses, 'Favorites replaced/discarded historical listing')
            rec=a.expect('/api/recommendations?limit=24')
            rec_ids=[r['id'] for r in rec['items']]
            check(len(rec_ids)==len(set(rec_ids)) and len(rec_ids)<=24, 'Duplicate/oversize recommendation')
            check(set(rec_ids).isdisjoint(expected+[rent,sale,new_sale]), 'Unavailable/saved/own listing recommended')
            check(all(r['status']=='available' and not r['is_favorite'] for r in rec['items']), 'Invalid suggestion')
            # Новые таблицы тоже не изменяются при блокировке; ограниченное ожидание даёт 503.
            stable=snapshot()
            with sqlite3.connect(path) as lock:
                lock.execute('BEGIN IMMEDIATE')
                started=time.monotonic(); favorite(a,new_sale,status=503)
                check(.5 < time.monotonic()-started < 4, 'Unbounded favorite wait')
                view(a,new_sale,503); lock.rollback()
            check(snapshot()==stable, 'Busy request partially wrote personal data')
            # Связь/пагинация страницы проверены через HTTP, не выдаём это за браузер.
            a.expect('/favorites?page=2'); a.expect('/view-history'); a.expect('/personal.html')
            for resource in ('personal-client.js','personal.js','recommendations.js'):
                a.expect('/'+resource)
            saved_favorites=a.expect('/api/favorites?page_size=100'); saved_views=a.expect('/api/views')
            before=snapshot(); stop(); start()
            check(snapshot()==before, 'Restart/seed changed personal or existing data')
            a.expect('/api/favorites',401); a.expect('/api/views',401)
            a.sign_in('a@example.com')
            check(a.expect('/api/favorites?page_size=100')==saved_favorites and a.expect('/api/views')==saved_views, 'Personal state lost after login')
            stop(); subprocess.run(command+['--init-db'],check=True,stdout=log,stderr=subprocess.STDOUT,timeout=10)
            check(snapshot()==before, 'Generator changed personal data')
            check(rows('PRAGMA integrity_check')==[('ok',)] and rows('PRAGMA foreign_key_check')==[], 'DB integrity failed')
            print('PASS: migration 4->5 preservation, favorites PUT/DELETE and 5 add/delete races, unique pair, privacy/CSRF/401/400, stable pages and closed history')
            print('PASS: explicit POST view/cooldown/race, guest/new user fallback, recommendation exclusions, stage4 lifecycle, busy limits, persistence of all 7 tables')
        except Exception:
            log.flush(); log.seek(0); print(log.read(),file=sys.stderr)
            raise
        finally:
            stop()
