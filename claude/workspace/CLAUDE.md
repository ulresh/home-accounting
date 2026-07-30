# ДомУчёт — памятка для продолжения разработки (Claude)

Это инструкция для новой сессии. Родительский `/workspace/CLAUDE.md` тоже действует
(ничего не скачивать/устанавливать; менять только рабочую папку; если чего-то не
хватает — сообщить). Здесь — про архитектуру, сборку, тесты и подводные камни.

## 1. Что это
Кросс-платформенный личный учёт расходов «ДомУчёт», два независимых приложения с
ОДИНАКОВЫМ форматом файлов и протоколом синхронизации:
- **Desktop**: Qt6 Widgets, C++20, CMake, boost (asio+json), OpenSSL. Папка `desktop/`.
- **Android**: Kotlin + Jetpack Compose, Gradle, Jackson (JSON), BouncyCastle. Папка `android/`.

Данные хранятся как JSONL-файлы; устройства синхронизируются напрямую по TLS (QR-пэйринг).
Формат и обмен ОБЯЗАНЫ оставаться совместимыми между платформами (см. §6, кросс-тесты).

## 2. Где собирать и как доставлять код
Редактируем локально в `/workspace/home-accounting`. Сеть/тулчейны — на удалённых хостах
(ssh-алиасы уже настроены). Локально НЕ собрать (нет Qt/Android SDK).

- **Desktop → `appbuild`**, проект там в `/home/builder/project/home-accounting`.
- **Android → `androidbuild2`** (системный `gradle` 9.5.1, без wrapper, gradle-прокси настроен),
  проект там же `/home/builder/project/home-accounting`.

Доставка кода — `tar` через ssh (на хостах НЕТ `rsync`):
```bash
# Desktop
cd desktop && tar czf - src tools CMakeLists.txt resources | \
  ssh appbuild 'cd /home/builder/project/home-accounting/desktop && tar xzf -'
# Android  — ВНИМАНИЕ: cd обязан указывать на .../android, иначе файлы лягут мимо!
cd android && tar czf - app/src app/build.gradle.kts | \
  ssh androidbuild2 'cd /home/builder/project/home-accounting/android && tar xzf -'
```
⚠️ `tar` при распаковке НЕ удаляет файлы. Если файл удалён локально — удали его на хосте
вручную (`ssh ... rm ...`). После доставки сверяй `md5sum` локально vs на хосте, прежде чем
доверять результатам сборки/тестов (однажды пуш ушёл в `.../home-accounting/app` мимо
`.../android/app`, и «зелёные» тесты гоняли старый код).

Пользователь иногда сам правит файлы прямо на `appbuild` (напр. `Store.cpp`). Перед
правкой: `scp`/`diff` локального и хостового файла, ПРИНЯТЬ его версию за базу, потом
накатывать своё.

## 3. Сборка и тесты
Гоняй тестовые/синк-команды ПОД `timeout` — баг в стриминге может уйти в busy-loop и повесить
раннер (см. §7). Gradle кэширует тесты — нужен `--rerun-tasks`.

### Desktop (CMake-цели: `home-accounting`, `guitest`, `syncv2test`, `xcompattest`)
```bash
ssh appbuild 'cd .../desktop && cmake -S . -B build && cmake --build build -j4 --target syncv2test'
ssh appbuild 'cd .../desktop && ./build/syncv2test'   # 49 unit-тестов модели+синка
ssh appbuild 'cd .../desktop && ./build/guitest'       # 60 offscreen-тестов UI
                                                      # (guitest сам ставит QT_QPA_PLATFORM=offscreen
                                                      #  и создаёт данные в ./guitest-data)
ssh appbuild 'cd .../desktop && ./build/xcompattest produce <dir> | verify <dir>'  # кросс-формат
# релиз: cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release --target home-accounting
```
Старые `tools/{modeltest,synctest,catfiltertest}.cpp` — ЛЕГАСИ (до инкрементного редизайна),
НЕ в CMake, не поддерживаются. Актуальны `syncv2test`, `xcompattest`, `guitest`.

### Android
```bash
ssh androidbuild2 'cd .../android && gradle :app:testDebugUnitTest --rerun-tasks --console=plain --no-daemon'
ssh androidbuild2 'cd .../android && gradle :app:assembleDebug'    # / :app:assembleRelease
```
`testDebugUnitTest` — JVM-юнит-тесты (`StoreSyncTest` 6 шт. вкл. отмену; `XCompatTest`
по умолчанию пропускается).
В `app/build.gradle.kts` тест-таску задан `LC_ALL=C.UTF-8` (иначе JVM на хосте кодирует
кириллические ИМЕНА файлов как «????????»; содержимое всегда пишется явным UTF-8).

### Кросс-платформенные тесты (формат + кадрирование обмена), оба направления
`xcompattest`/`XCompatTest` каждая платформа *produce* → эталон (БД + `exchange.bin`),
другая *verify*. Оркестрация: произвести на обеих, перекинуть `tar`-пайпом между хостами,
запустить verify у второй. Android-сторона управляется env: `XC_MODE=produce|verify XC_DIR=<dir>`.

## 4. Формат данных на диске
Полное описание формата смотри в /workspace/home-accounting/data.txt
.
При расхождении требований в файлах /workspace/home-accounting/CLAUDE.md
и /workspace/home-accounting/data.txt сообщить и прервать выполнение
полученного задания.

Корень: desktop `~/.data/home-accounting`, Android `filesDir`. Внутри:
`database.jsonl` (список баз), `config.json`, `identity/` (ключ/сертификат),
и на каждую базу — папка `<ИмяБазы>` (по умолчанию `Основная`) с:
- `device.jsonl`
- `people.jsonl`
- `catalog.jsonl`
- `sync/<DN>.jsonl` — индекс синхронизации с партнёром DN (см. §5)
- декадные папки `2020/`, `2030/`… → месячные `YYMM.jsonl` (события)

**Месячный событийный файл** = последовательность JSON-значений (не построчно — границы
определяет парсер).
Виды строк:
- `header`: `{"header":[...колонки...],"reference":["edit_datetime","rec_no","dev_no","event_datetime"]}`.
  Наша каноническая схема — 9 колонок: `event_datetime,subject,cost,edit_datetime,rec_no,dev_no,people,volume,comment`.
  Строки парсятся по ДЕЙСТВУЮЩЕМУ заголовку; партнёр может прислать другой порядок/состав —
  пишем «как получили» (только DN map), поэтому в одном файле бывают разные схемы.
- событие — массив по колонкам схемы (хвостовые null обрезаются, но не короче 6 полей).
- удаление — `{"delete":[ref удаляемого],"this":[ref записи-удаления],"update":[ref нового]?}`,
  где ref = поля из `reference` (edit_datetime,rec_no,dev_no). Наличие `update` означает, что
  это была правка (editEvent = delete старого + add нового); `update` — именно МАССИВ-ссылка
  на новую запись, как в data.txt, а не `true`.

Идентичность записи = `(edit_datetime, rec_no, dev_no)`. Файл выбирается по
`yyyymmOf(event_datetime)` — И для события, И для записи о его удалении: удаление обязано
лежать в ТОМ ЖЕ файле, что и удаляемое событие (data.txt: «Записи группируем в файл по
значению `<event_datetime>`»). Загрузка — потоковая, по месяцам в порядке возрастания,
удаления применяются в пределах своего месяца; `raw` в памяти не держим.

## 5. Синхронизация (важное)
Полностью **потоковая и прерываемая**, peer-state индекс. Не накапливать файлы/блоки в памяти.
- **Чтение файлов** — блоками в инкрементный парсер (desktop `boost::json::stream_parser`;
  Android — Jackson `Jk.forEachValue` в `model/Jsonl.kt`). `stateOf` (size+sha1) тоже стримит.
- **Отдача** — сетевой слой читает файл блоками и сразу пишет в сокет.
- **Приём** — блок читается из сокета и сразу разбирается: desktop корутинами
  (`co_await aReadToSink` → `syncRecvFeed`), Android `Store.syncReceiveBlob(...,BoundedInputStream)`.
- **Async/cancel**: desktop = boost.asio C++20 корутины (`co_spawn`/`co_await`), `cancel()`
  постит закрытие сокета/акцептора в io_context → прерывает ЛЮБОЙ await. Android = блокирующий
  I/O в потоке, `cancel()` закрывает сохранённый сокет → прерывает любой read/write/accept.
  И сервер, и клиент отменяемы; UI закрытием диалога отменяет оба.
- **Индекс `sync/<peerDn>.jsonl` хранит СОСТОЯНИЕ СОБЕСЕДНИКА**: строки `[yyyymm, offset]` =
  сколько байт нашего месячного файла у партнёра уже есть. Справочники — через обмен
  МАНИФЕСТАМИ (size+sha1 people/catalog/device) в начале сессии; список шлём, только если наша
  версия отличается от версии партнёра.
  При вливании событий: игнорировать с DN→свой (эхо) и с DN не в карте.

## 6. Инварианты — НЕ ломать
- **Кросс-платформенная совместимость**: одинаковые имена колонок/reference.
Любая правка протокола/формата — на ОБЕИХ
  платформах, иначе кросс-тесты упадут. После правок ОБЯЗАТЕЛЬНО гонять кросс-тесты в обе стороны.
- Дробная цена: desktop пишет `12.50`, Android `12.5` — обе стороны парсят в 12.5 (ок). Целое —
  без дробной части (`250`).
- JSON пишется сырым UTF-8 (без `\uXXXX`-эскейпа кириллицы) — и boost::json, и Jackson по
  умолчанию так и делают; не включать ESCAPE_NON_ASCII.
- «Метки» переименованы только в UI: «Кому» (people), «Количество» (volume); имена
  полей/переменных не трогать.

## 7. Подводные камни (уже наступали)
- **boost::json и целые числа**: парсер кладёт неотрицательное целое в `int64`, а `uint64`
  выбирает, только если значение не влезает в `int64`. Поэтому `as_uint64()`/`is_uint64()`
  к РАЗОБРАННОМУ значению неприменимы: `as_uint64()` бросает, `is_uint64()` всегда false.
  Использовать `asU64()`/`isNum()` (есть в `Store.cpp` и `SyncService.cpp`). Из-за этого
  однажды молча не грузились `device.jsonl` и `sync/DN.jsonl`, а синхронизация падала на
  первом же блоке.
- **Использование после `std::move`**: в приёме удалений `MonthDeletions::Op op{std::move(d),
  ...}` обнулял `d`, а следующая строка искала `store.events_.find(&d)` — по пустым строкам,
  т.е. `erase` никогда не срабатывал и удаление не доходило до памяти. Искать по `op.del`.
- **Висячий `else`**: в разборе `people.jsonl` ветка `else if(v.is_array())` без скобок
  прилипала к внутреннему `if(time.is_string())`, из-за чего маркер `["delete"]` не
  переключал список и удалённые люди читались как действующие.
- **Файл записи удаления**: `writeDelete` обязан выбирать месяц по `event_datetime`
  УДАЛЯЕМОГО события, а не по его `edit_datetime`. Однажды было по `edit_datetime` — записи
  удаления уезжали в файл следующего месяца (событие за июнь, удаление в июле), и после
  перезапуска удалённое и старые версии правок воскресали. Не «чинить» это в загрузчике:
  единственно верное место — `writeDelete`.
- **Android JSON**: `kotlinx-serialization` `decodeToSequence` НЕ тянет смешанные типы значений
  в JSONL — поэтому ушли на **Jackson streaming** (он умеет инкрементный разбор произвольной
  последовательности значений). Не возвращать kotlinx.
- **Зависание тестов = busy-loop, не deadlock**: пустой/отсутствующий файл нельзя открывать в
  `sendItems` (гард `fileLen>0 && exists()`); `readLine` при EOF должен БРОСАТЬ, а не возвращать
  `""` (иначе `if(line.isEmpty()) continue` крутится на 98% CPU). Диагностика: `jstack <pid
  gradle test worker>` — RUNNABLE + огромный CPU = busy-loop. Всегда гонять тесты под `timeout`.
- **`Crypto` на Android — пер-Store** (keystore передаётся в `sslContext`), НЕ object-синглтон,
  иначе два Store в одном тест-JVM делят одну TLS-личность.
- **JVM-локаль имён файлов**: тест-таска ставит `LC_ALL=C.UTF-8` (см. §3).
- **GCC C++20 корутины**: нельзя `co_await ...(json::object{{...}})` с инициализатор-листом внутри
  корутины — «array used as initializer». Собирать объект в именованную переменную заранее.
- **Путь доставки tar** (см. §2) — сверять md5.

## 8. Текущий статус (после переработки model/sync и приведения к ней UI)
- Desktop UI приведён к новому API модели: события — `shared_ptr<Event>`, поля
  `people/volume/comment` — обычные строки, `people()`/`catalog()` — map с отметками времени.
  Новый `PeopleDialog`, переписанный `CatalogDialog` (пишет сразу, без «Сохранить»).
- `guitest` — 60/60: таблица, фильтр, добавление/правка/удаление через диалоги, редакторы
  каталога и людей, перезагрузка с диска, отмена синхронизации закрытием окна.
- `syncv2test` — 45/49. Остаток — незавершённое слияние в sync (см. ниже), не UI.
- Релиз-бинарь собирается (~1,04 МБ).
- **`xcompattest` исключён из сборки по умолчанию** (`EXCLUDE_FROM_ALL`): написан под старый
  API (`syncBegin`/`syncPlanOutgoing`/`syncRecvFeed`/`ListManifest`), которого больше нет.
- Android не трогали; кросс-тесты обе стороны из-за `xcompattest` сейчас не гоняются.

### Что ещё не работает в sync (падает в `syncv2test`)
- Дедупликация одинаковых событий выполняется только на стороне сервера: у клиента дубликат
  остаётся и в памяти, и в файлах.
- В `SyncService.cpp` остались пометки `// TODO +++ dnMap` в приёме событий/удалений.

## 9. Возможные следующие шаги
- Доделать слияние в sync (список выше) — это единственные красные тесты.
- Переписать `xcompattest` под новый потоковый API и вернуть кросс-тесты обе стороны.
- **Подписать релиз-APK** (`app-release-unsigned.apk`) — нужен keystore (свой или сгенерить);
  можно прописать `signingConfig` в `app/build.gradle.kts` или подписать `apksigner`-ом.
- Реальная end-to-end TLS-синхронизация C++↔JVM по сети между хостами (сейчас кросс-проверка —
  файловая: формат БД + кадрирование `exchange.bin`, а не живой сокет).
- Перечитать существующий код перед изменениями (он мог поменяться) — факты тут point-in-time.

## 10. Правила работы
- Ничего не качать/устанавливать на хостах (gradle тянет зависимости через прокси — это норм).
- Менять только рабочую папку; чего не хватает — сообщать.
- После ЛЮБОЙ правки формата/протокола: собрать обе платформы, прогнать unit-тесты И кросс-тесты
  в обе стороны, под `timeout`, сверив, что гоняется именно свежий код (md5).
