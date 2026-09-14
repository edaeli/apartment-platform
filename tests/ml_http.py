"""ML API: временная база/порт. Python только управляет тестом, прогноз вычисляет C++."""
import json
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.request

server, root, trainer, reference = map(lambda p: Path(p).resolve(), sys.argv[1:5])
disabled = '--disabled' in sys.argv[5:]
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
with tempfile.TemporaryDirectory(prefix='apartment-ml-http-') as directory:
    tmp = Path(directory)
    db = tmp / 'test.sqlite3'
    models = tmp / 'models'
    subprocess.run([server, '--root', root, '--db', db, '--init-db'], check=True, capture_output=True)
    subprocess.run([trainer, '--db', db, '--out', models], check=True, capture_output=True)
    original = {deal: (models / (deal + '.model')).read_text() for deal in ('rent', 'sale')}
    connection = sqlite3.connect(db)
    ids = dict(connection.execute('SELECT deal_type, MIN(id) FROM listings GROUP BY deal_type'))
    process = None
    log_path = tmp / 'server.log'
    def get(path):
        with opener.open(base + path, timeout=5) as response:
            data = response.read()
            return json.loads(data) if 'application/json' in response.headers.get('Content-Type', '') else data
    def stop():
        global process
        if process:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
            process = None
    def start(use_models=True):
        global process, base
        stop()
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
        base = f'http://127.0.0.1:{port}'
        command = [server, '--root', root, '--db', db, '--port', str(port)]
        if use_models:
            command += ['--models-dir', models]
        with log_path.open('ab') as log:
            process = subprocess.Popen(command, stdout=log, stderr=log)
        for _ in range(100):
            assert process.poll() is None, log_path.read_text()
            try:
                if get('/api/health')['status'] == 'ok':
                    return
            except (OSError, TimeoutError):
                pass
            time.sleep(.05)
        raise AssertionError('Server startup timeout')
    def estimate(deal):
        return get('/api/listings/' + str(ids[deal]))['price_estimate']
    def unavailable(deal):
        result = estimate(deal)
        assert result == {'status': 'unavailable'}, result
    def expected():
        output = subprocess.check_output([reference, db, models], text=True)
        return {int(i): float(p) for i, p in (line.split() for line in output.splitlines())}
    def restore():
        for deal, text in original.items():
            (models / (deal + '.model')).write_text(text)
    try:
        snapshot = '\n'.join(connection.iterdump())
        start(False)
        for deal in ids: unavailable(deal)
        assert b'model-price' in get('/listings/' + str(ids['rent']))
        assert get('/api/listings')['items']
        start()
        if disabled:
            for deal in ids: unavailable(deal)
            assert b'<!doctype html>' in get('/')
            print('PASS: BUILD_PRICE_ML=OFF starts with models-dir, ordinary pages/API work; estimate unavailable')
        else:
            predictions = expected()
            for listing, property_id in connection.execute('SELECT id, property_id FROM listings'):
                result = get('/api/listings/' + str(listing))['price_estimate']
                assert result['status'] == 'available'
                assert abs(result['amount_amd']-predictions[property_id]) <= max(1e-7, abs(predictions[property_id])*1e-12)
            # Артефакты удалены после загрузки: HTTP не перечитывает их и не обучает модель.
            for path in models.glob('*.model'): path.unlink()
            for deal in ids: assert estimate(deal)['status'] == 'available'
            assert not list(models.glob('*.model'))
            assert snapshot == '\n'.join(connection.iterdump())
            start()
            for deal in ids: unavailable(deal)
            restore()
            (models / 'rent.model').write_text('APARTMENT_RIDGE 999\n')
            start(); unavailable('rent'); assert estimate('sale')['status'] == 'available'
            assert 'Unsupported model format' in log_path.read_text()
            (models / 'rent.model').write_text(original['sale'])
            start(); unavailable('rent'); assert 'does not match filename' in log_path.read_text()
            (models / 'rent.model').write_text(original['rent'][:50])
            start(); unavailable('rent')
            # Валидные конечные коэффициенты, но переполнение в predict.
            restore(); lines = original['rent'].splitlines()
            lines[3] = '0 1e308 100'
            for i in range(8, len(lines)): lines[i] = lines[i].rsplit(' ', 1)[0] + (' 2' if i == 8 else ' 0')
            (models / 'rent.model').write_text('\n'.join(lines)+'\n')
            start(); unavailable('rent'); assert estimate('sale')['status'] == 'available'
            assert get('/api/listings')['items']
            stop()  # Завершение сбрасывает буфер журнала перед проверкой причины.
            assert 'prediction unavailable' in log_path.read_text()
            # Существующая граница 1 AMD сохранена, а не изменена адаптером.
            lines[3] = '-100 1 100'
            for i in range(8, len(lines)): lines[i] = lines[i].rsplit(' ', 1)[0] + ' 0'
            (models / 'rent.model').write_text('\n'.join(lines)+'\n')
            start(); assert estimate('rent')['amount_amd'] == 1
            restore(); start()
            before = estimate('rent')['amount_amd']
            connection.execute('UPDATE listings SET price=price+123456 WHERE id=?', (ids['rent'],)); connection.commit()
            assert estimate('rent')['amount_amd'] == before, 'Listing price leaked into prediction'
            connection.execute("UPDATE properties SET district='TEST_UNKNOWN' WHERE id=(SELECT property_id FROM listings WHERE id=?)", (ids['rent'],)); connection.commit()
            predictions = expected()
            prop = connection.execute('SELECT property_id FROM listings WHERE id=?', (ids['rent'],)).fetchone()[0]
            assert abs(estimate('rent')['amount_amd']-predictions[prop]) < 1e-6
            print('PASS: 1000 API/reference predictions, separate deals, startup-only load, missing/corrupt/version/wrong-deal models, prediction failure, min=1, unknown, no price leakage; source preserved before explicit fixture edits')
    finally:
        stop(); connection.close()
