"""Explicit demo CLI, migration and live HTTP. Only a private temporary database."""
import concurrent.futures
import http.cookiejar
import json
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

binary, root = (Path(p).resolve() for p in sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix='apartment-demo-') as temporary:
    database = Path(temporary) / 'demo.sqlite3'
    def rows(sql):
        with sqlite3.connect(database) as db:
            return db.execute(sql).fetchall()
    def write(sql):
        with sqlite3.connect(database) as db:
            db.executescript(sql)
    def snapshot():
        return {t: rows(f'SELECT * FROM {t} ORDER BY rowid') for t in
                ('properties','listings','property_photos','users','bookings','favorites','listing_views')}
    with sqlite3.connect(database) as db:
        for name in ('001_initial.sql','002_catalog_indexes.sql','003_users_bookings.sql','004_resale_origin.sql','005_personalization.sql'):
            db.executescript((root/'sql'/name).read_text())
        fixtures = [
            ('demo-001','apartment','Ереван, ул. Сарьяна, 12','Кентрон',64,2,4,
             'Светлая квартира с отдельной кухней и балконом. Рядом кафе и прогулочные улицы.',40.1850,44.5070,'rent',280000),
            ('demo-002','apartment','Ереван, пр. Комитаса, 38','Арабкир',82,3,6,
             'Просторная гостиная, две спальни и место для работы. Окна выходят во двор.',40.2070,44.5110,'sale',48500000)]
        for i,p in enumerate(fixtures,1):
            db.execute('INSERT INTO properties(demo_key,kind,address,district,area,rooms,floor,description,latitude,longitude) VALUES(?,?,?,?,?,?,?,?,?,?)',p[:10])
            db.execute('INSERT INTO listings(property_id,demo_key,deal_type,price) VALUES(?,?,?,?)',(i,p[0],p[10],p[11]))
            for order,(url,caption) in enumerate((('living-room','Гостиная'),('kitchen','Кухня'),('bedroom','Спальня'))):
                db.execute('INSERT INTO property_photos(property_id,url,caption,sort_order) VALUES(?,?,?,?)',(i,f'/images/{url}.jpg',caption,order))
    command = [str(binary),'--root',str(root),'--db',str(database)]
    def cli(flag, ok=True):
        result = subprocess.run(command+[flag],capture_output=True,text=True,timeout=12)
        assert (result.returncode==0)==ok, (flag,result.stderr)
        return result.stdout
    legacy=snapshot()
    assert 'Eligible: 2' in cli('--preview-demo-update')
    assert rows('PRAGMA user_version')==[(6,)]
    current=snapshot()
    for table,values in legacy.items():
        assert [r[:len(values[0])] for r in current[table]]==values if values else not current[table]
    assert rows('SELECT renovation,demo_revision FROM properties')==[('unspecified',0)]*2
    # Preview/apply never creates missing demonstration objects.
    assert len(current['properties'])==2
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    base=f'http://127.0.0.1:{port}'
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}),urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
    csrf=''
    def request(path,method='GET',body=None):
        headers={'Origin':base,'X-CSRF-Token':csrf,'Content-Type':'application/json'}
        req=urllib.request.Request(base+path,method=method,headers=headers,data=json.dumps(body or {}).encode() if method!='GET' else None)
        with opener.open(req,timeout=5) as response:return json.load(response)
    process=None
    with (Path(temporary)/'server.log').open('w') as log:
        def start():
            global process
            process=subprocess.Popen(command+['--port',str(port)],stdout=log,stderr=subprocess.STDOUT)
            for _ in range(100):
                assert process.poll() is None
                try:
                    if request('/api/health')['status']=='ok':return
                except OSError:time.sleep(.05)
            raise AssertionError('Server did not start')
        def stop():
            if process is not None:
                process.terminate();process.wait(timeout=8)
        try:
            start()
            assert request('/api/listings/1')['renovation']=='unspecified'
            csrf=request('/api/auth/me')['csrf_token']
            user=request('/api/auth/register','POST',{'name':'Demo test','email':'demo@example.com','password':'Only for tests 2026!'})
            csrf=user['csrf_token']
            request('/api/favorites/2','PUT')
            before=snapshot()
            assert 'Eligible: 1, protected: 1' in cli('--preview-demo-update')
            assert snapshot()==before
            write("CREATE TRIGGER fail_update BEFORE UPDATE OF price ON listings WHEN OLD.id=1 BEGIN SELECT RAISE(ABORT,'test rollback'); END;")
            cli('--update-demo-data',False)
            assert snapshot()==before
            write('DROP TRIGGER fail_update;')
            barrier=threading.Barrier(2)
            def update():
                barrier.wait();return cli('--update-demo-data')
            with concurrent.futures.ThreadPoolExecutor(2) as pool:
                results=list(pool.map(lambda _: update(),range(2)))
            assert sum('Updated: 1,' in x for x in results)==1 and sum('Updated: 0,' in x for x in results)==1
            after=snapshot()
            for table in ('users','bookings','favorites','listing_views','property_photos'):
                assert after[table]==before[table]
            assert after['properties'][1]==before['properties'][1] and after['listings'][1]==before['listings'][1]
            assert request('/api/auth/me')['user']['id']==user['user']['id'], 'CLI disturbed live session'
            detail=request('/api/listings/1')
            assert detail['renovation'] in ('needs_repair','cosmetic','good','designer') and detail['price']!=280000
            assert 'Updated: 0,' in cli('--update-demo-data') and snapshot()==after
            cli('--init-db');assert snapshot()==after
            stop();start();assert snapshot()==after
            assert request('/api/listings/1')['renovation']==detail['renovation']
            assert request('/api/auth/me')['user'] is None  # Existing documented restart policy.
            # Busy writer must fail within a bounded wait and leave all data intact.
            with sqlite3.connect(database) as locked:
                locked.execute('BEGIN IMMEDIATE')
                cli('--update-demo-data',False)
            assert snapshot()==after
            print('PASS: migration 5->6, read-only preview, sparse demo, concurrent CLI, live session, HTTP repair, rollback, protected favorite, restart/seed and bounded lock wait')
        finally:stop()
