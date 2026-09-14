# Ubuntu 24.04 LTS: запуск для преподавателя

Целевая ОС — **Ubuntu 24.04 LTS (amd64 или arm64)**. Это инструкция для проверки на Ubuntu, а не отчёт об уже выполненной Linux-сборке. На 14 сентября 2026 доступной Ubuntu-среды у исполнителя нет. Реально проверена macOS 15.0 arm64; результаты находятся в [verification.md](verification.md). Не переносите бинарники, CMakeCache, базу автора или готовые модели между ОС.

## 1. Инструменты и библиотеки (выполнять в Ubuntu)

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake git pkg-config ca-certificates \
  libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev libsqlite3-dev sqlite3 \
  libsodium-dev python3 curl lsof
```

`build-essential` содержит GCC/G++ и make; C++17 и CMake >=3.20 необходимы проекту. SQLite >=3.24 содержит библиотеку и заголовки, `sqlite3` — консоль. libsodium обеспечивает Argon2id и случайные токены. Python >=3.8 нужен только HTTP-тестам: сервер и ML используют C++. JsonCpp, UUID, zlib и OpenSSL нужны Drogon/Trantor. Eigen, Python ML-библиотеки, PostgreSQL и MySQL проекту не нужны.

### Drogon 1.9.13 из исходников

В [Ubuntu Noble libdrogon-dev](https://packages.ubuntu.com/noble/web/libdrogon-dev) находится версия 1.8.7. Чтобы не подменять версию фреймворка, используем **v1.9.13**, проверенную проектом на macOS. Её сборка из исходников на Ubuntu пока **не выполнена**. Тег и параметры сверены с [CMake upstream v1.9.13](https://github.com/drogonframework/drogon/blob/v1.9.13/CMakeLists.txt). Trantor берётся как submodule с ревизией, закреплённой этим тегом. Никакой установки Homebrew на Ubuntu не требуется.

В новой папке для исходников зависимостей:

```sh
mkdir -p "$HOME/src/apartment-deps"
cd "$HOME/src/apartment-deps"
git clone --branch v1.9.13 --depth 1 --recurse-submodules \
  https://github.com/drogonframework/drogon.git drogon-1.9.13
cmake -S drogon-1.9.13 -B drogon-1.9.13/build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local/opt/drogon-1.9.13" \
  -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_CTL=OFF \
  -DBUILD_POSTGRESQL=OFF -DBUILD_MYSQL=OFF -DBUILD_REDIS=OFF \
  -DBUILD_SQLITE=ON -DBUILD_BROTLI=OFF -DBUILD_YAML_CONFIG=OFF
cmake --build drogon-1.9.13/build -j 2
cmake --install drogon-1.9.13/build
```

Установка в каталог пользователя не требует `sudo`. Не устанавливайте одновременно другую версию Drogon ради этой инструкции. Каталоги выше должны быть новыми при первом выполнении; при повторе используйте существующий checkout тега. Пакеты Ubuntu могут получать обновления безопасности: фактические версии фиксируйте через `dpkg-query -W g++ cmake libsqlite3-dev libsodium-dev libjsoncpp-dev libssl-dev`.

## 2. Получение именно версии с ML

Текущая разработка находится в **codex/ml-price-estimation**. В этом задании push не выполняется; опубликованный `main` нельзя считать содержащим эти изменения.

После отдельной разрешённой публикации этой ветки:

```sh
git clone --branch codex/ml-price-estimation https://github.com/edaeli/apartment-platform.git
cd apartment-platform
git log -1 --oneline
git status --short
```

До публикации преподавателю можно передать Git bundle (только коммиты, без игнорируемых данных). После финального локального коммита на компьютере автора, из корня проекта:

```sh
mkdir -p build-transfer
git bundle create build-transfer/apartment-platform.bundle codex/ml-price-estimation
```

Передайте только этот файл. В Ubuntu, рядом с полученным bundle:

```sh
git clone --branch codex/ml-price-estimation apartment-platform.bundle apartment-platform
cd apartment-platform
```

Этот вариант не выполняет push. Bundle в `build-transfer/` игнорируется Git. Проверьте идентификатор коммита с автором; обе процедуры получения — альтернативы, не последовательные команды.

## 3. Сборка и отдельная база с ML

Все следующие команды выполняются **из корня полученного проекта**:

```sh
cmake -S . -B build-ubuntu-on -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$HOME/.local/opt/drogon-1.9.13" \
  -DBUILD_PRICE_ML=ON -DBUILD_TESTING=ON
cmake --build build-ubuntu-on -j 2
./build-ubuntu-on/apartment_server --root . --db build-ubuntu-on/demo.sqlite3 --init-db
./build-ubuntu-on/apartment_server --root . --db build-ubuntu-on/demo.sqlite3 --init-db
sqlite3 build-ubuntu-on/demo.sqlite3 \
  'SELECT (SELECT COUNT(*) FROM properties), (SELECT COUNT(*) FROM listings), (SELECT COUNT(*) FROM property_photos);'
```

Ожидается **1000|1000|3000** после первого и повторного заполнения новой базы. `--init-db` применяет только отсутствующие миграции и добавляет недостающие демоданные; не очищает существующую базу. Не используйте здесь рабочую базу автора.

```sh
./build-ubuntu-on/price_ml --db build-ubuntu-on/demo.sqlite3 --out build-ubuntu-on/demo-models
ctest --test-dir build-ubuntu-on --output-on-failure
lsof -nP -iTCP:8082 -sTCP:LISTEN
# Только если порт свободен:
./build-ubuntu-on/apartment_server --root . --db build-ubuntu-on/demo.sqlite3 \
  --port 8082 --models-dir build-ubuntu-on/demo-models
```

`--out` должен указывать на новый каталог. При повторном запуске сайта используйте уже созданные модели, не повторяйте обучение. Относительные `--db` и `--models-dir` отсчитываются от текущего каталога терминала; `--root` задаёт расположение `public/` и `sql/`. Все пути можно задать абсолютными, пути с пробелами заключайте в кавычки.

Адрес в браузере **внутри Ubuntu**: http://127.0.0.1:8082/. Сервер слушает loopback; для SSH-сервера используйте туннель со своего компьютера `ssh -L 8082:127.0.0.1:8082 user@ubuntu-host` (локальный порт тоже должен быть свободен). Остановка сервера — Ctrl+C только в его терминале. Пользователям после перезапуска потребуется войти заново; SQLite сохраняется.

## 4. HTTP и оценка модели

Во втором терминале Ubuntu, пока работает тестовый сервер:

```sh
curl --fail http://127.0.0.1:8082/api/health
curl --fail 'http://127.0.0.1:8082/api/listings?page=1'
curl --fail http://127.0.0.1:8082/api/listings/618
curl --fail -o /dev/null http://127.0.0.1:8082/listings/1
curl --fail -o /dev/null http://127.0.0.1:8082/listing.js
curl --fail -o /dev/null http://127.0.0.1:8082/styles.css
curl --fail -o /dev/null http://127.0.0.1:8082/images/living-room.jpg
```

Сборка ON регистрирует **12 CTest**, OFF — **10**. `ml_http` создаёт отдельную временную базу, обучает модели отдельным CLI, сравнивает 1000 API-прогнозов с C++, проверяет обе сделки, отсутствующие/повреждённые модели, ошибку прогноза и **lower_clipped**. При нижней обрезке API не возвращает сумму, а сообщает `status=unavailable, reason=lower_clipped`. На странице: «Учебная оценка для этого объекта недоступна». Фиксированный негативный случай есть в тесте независимо от того, какой id даёт такой прогноз на новой базе. Для обычной аренды единицы — «в месяц», для продажи — «за объект».

Новый демонабор не идентичен прежней пользовательской базе: не требуйте точного числа 552674 или прежних MAE от новой генерации. Отрицательные исходные прогнозы допустимы у линейной модели, но не показываются как цена. Модель изучает синтетический каталог, не рынок. Алгоритм, исходные метрики и ограничения: [ml-price-estimation.md](ml-price-estimation.md).

Необязательная проверка DOM (не реальные клики, требуется интернет для единственной загрузки фиксированного LinkeDOM):

```sh
sudo apt-get install -y nodejs
python3 tests/prepare_dom_tests.py
node tests/booking_ui_checks.mjs
node tests/pagination_ui_checks.mjs
node tests/ui_state_checks.mjs
```

Node нужен только этим тестам, сервер его не использует. Подготовщик создаёт собственный `build/test-deps/` из проверяемого по SHA256 архива; готовый `build/` автора не нужен.

## 5. Сборка и запуск без ML

```sh
cmake -S . -B build-ubuntu-off -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$HOME/.local/opt/drogon-1.9.13" \
  -DBUILD_PRICE_ML=OFF -DBUILD_TESTING=ON
cmake --build build-ubuntu-off -j 2
ctest --test-dir build-ubuntu-off --output-on-failure
python3 tests/ml_http.py ./build-ubuntu-off/apartment_server . \
  ./build-ubuntu-on/price_ml ./build-ubuntu-on/ml_reference --disabled
./build-ubuntu-off/apartment_server --root . --db build-ubuntu-off/demo.sqlite3 --init-db
lsof -nP -iTCP:8083 -sTCP:LISTEN
# Только если порт свободен:
./build-ubuntu-off/apartment_server --root . --db build-ubuntu-off/demo.sqlite3 --port 8083
```

Открыть http://127.0.0.1:8083/. Обычные функции работают, оценка модели недоступна. В ON без `--models-dir` поведение аналогично; отсутствующие модели не мешают запуску. Команда `--disabled` отдельно проверяет OFF даже с переданным каталогом моделей. Для сборки без любых тестов/Python установите `-DBUILD_TESTING=OFF`.

## Границы готовности

CMake ищет библиотеки стандартными механизмами и связывает импортированные targets, пути Homebrew в исходниках отсутствуют. GCC/Clang поддерживают используемые C++17 API; единственный POSIX-вызов `mkdtemp` находится в ML-тесте, поддерживается macOS/Linux. Проверен точный регистр 29 ссылок HTML на локальные файлы; коллизий регистра среди Git-путей нет. Регистр остальных динамических ресурсов проверяют HTTP-тесты, но файловая система Linux пока не испытана.

Перед сдачей ещё нужно **выполнить эту инструкцию на Ubuntu 24.04**, записать версии, результаты 12/10 CTest и OFF-проверки, проверить реальные страницы и ML-блок в браузере Ubuntu. Автоматические DOM/HTTP-проверки не подтверждают реальные клики. macOS-сборка не доказывает чистую установку Ubuntu.
