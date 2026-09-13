"""Подготовка необязательного DOM-теста. Рабочая база не используется."""
import hashlib
from pathlib import Path
import tarfile
import urllib.request

# Standalone DOM без npm и дополнительных зависимостей. Это не движок браузера.
VERSION = "0.18.12"
SHA256 = "b15cc6324234cd73785619b01a86fd7ada68277f2ec0c571dc0de94d477f750d"
destination = Path(__file__).resolve().parents[1] / "build" / "test-deps"
destination.mkdir(parents=True, exist_ok=True)
archive_path = destination / f"linkedom-{VERSION}.tgz"
if not archive_path.exists():
    url = f"https://registry.npmjs.org/linkedom/-/linkedom-{VERSION}.tgz"
    with urllib.request.urlopen(url, timeout=30) as response:
        content = response.read()
    if hashlib.sha256(content).hexdigest() != SHA256:
        raise RuntimeError("Контрольная сумма загрузки LinkeDOM не совпадает")
    archive_path.write_bytes(content)
if hashlib.sha256(archive_path.read_bytes()).hexdigest() != SHA256:
    raise RuntimeError("Контрольная сумма архива LinkeDOM не совпадает")
with tarfile.open(archive_path) as archive:
    # Извлекаем только известный файл, не произвольные пути архива.
    worker = archive.extractfile("package/worker.js")
    if worker is None:
        raise RuntimeError("В архиве отсутствует worker.js")
    (destination / "linkedom-worker.mjs").write_bytes(worker.read())
print(f"DOM для теста подготовлен: {destination / 'linkedom-worker.mjs'}")
