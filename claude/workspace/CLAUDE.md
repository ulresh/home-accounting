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
ssh appbuild 'cd .../desktop && ./build/syncv2test'                         # 41 unit-тест модели+синка
ssh appbuild 'cd .../desktop && QT_QPA_PLATFORM=offscreen ./build/guitest'  # отмена синка из UI
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
по умолчанию пропускается). Эмулятор/Appium НЕ используем — всё на unit-тестах.
В `app/build.gradle.kts` тест-таску задан `LC_ALL=C.UTF-8` (иначе JVM на хосте кодирует
кириллические ИМЕНА файлов как «????????»; содержимое всегда пишется явным UTF-8).

### Кросс-платформенные тесты (формат + кадрирование обмена), оба направления
`xcompattest`/`XCompatTest` каждая платформа *produce* → эталон (БД + `exchange.bin`),
другая *verify*. Оркестрация: произвести на обеих, перекинуть `tar`-пайпом между хостами,
запустить verify у второй. Android-сторона управляется env: `XC_MODE=produce|verify XC_DIR=<dir>`.

## 4. Формат данных на диске
Корень: desktop `~/.data/home-accounting`, Android `filesDir`. Внутри:
`database.jsonl` (список баз), `config.json`, `identity/` (ключ/сертификат),
и на каждую базу — папка `<ИмяБазы>` (по умолчанию `Основная`) с:
- `device.jsonl` — `[DN,"<pubkey b64>","имя"]`
- `people.jsonl` — строки-имена
- `catalog.jsonl` — `["Категория","поз1","поз2"...]` (позиция может быть вложенной категорией)
- `sync/<DN>.jsonl` — индекс синхронизации с партнёром DN (см. §5)
- декадные папки `2020/`, `2030/`… → месячные `YYMM.jsonl` (события)

**Месячный событийный файл** = последовательность JSON-значений (не построчно — границы
определяет парсер). Виды строк:
- `header`: `{"header":[...колонки...],"reference":["edit_datetime","rec_no","dev_no"]}`.
  Наша каноническая схема — 9 колонок: `event_datetime,subject,cost,edit_datetime,rec_no,dev_no,people,volume,comment`.
  Строки парсятся по ДЕЙСТВУЮЩЕМУ заголовку; партнёр может прислать другой порядок/состав —
  пишем «как получили» (только DN map), поэтому в одном файле бывают разные схемы.
- событие — массив по колонкам схемы (хвостовые null обрезаются, но не короче 6 полей).
- удаление — `{"delete":[ref удаляемого],"this":[ref записи-удаления],"update":true?}`,
  где ref = поля из `reference` (edit_datetime,rec_no,dev_no). `update:true` — это была правка
  (editEvent = delete старого с update + add нового).

Идентичность записи = `(edit_datetime, rec_no, dev_no)`. Файл события выбирается по
`yyyymmOf(event_datetime)`; запись удаления — по `yyyymmOf(target.edit_datetime)`.
Загрузка — потоковая, по месяцам в порядке возрастания, удаления применяются на лету;
`raw` в памяти не держим.

## 5. Синхронизация (важное)
Полностью **потоковая и прерываемая**, peer-state индекс. Не накапливать файлы/блоки в памяти.
- **Чтение файлов** — блоками в инкрементный парсер (desktop `boost::json::stream_parser`;
  Android — Jackson `Jk.forEachValue` в `model/Jsonl.kt`). `stateOf` (size+sha1) тоже стримит.
- **Отдача** — `syncPlanOutgoing(peerManifest)` возвращает ПЛАН (путь/offset/len/prepend, без
  данных); сетевой слой читает файл блоками и сразу пишет в сокет.
- **Приём** — блок читается из сокета и сразу разбирается: desktop корутинами
  (`co_await aReadToSink` → `syncRecvFeed`), Android `Store.syncReceiveBlob(...,BoundedInputStream)`.
- **Async/cancel**: desktop = boost.asio C++20 корутины (`co_spawn`/`co_await`), `cancel()`
  постит закрытие сокета/акцептора в io_context → прерывает ЛЮБОЙ await. Android = блокирующий
  I/O в потоке, `cancel()` закрывает сохранённый сокет → прерывает любой read/write/accept.
  И сервер, и клиент отменяемы; UI закрытием диалога отменяет оба.
- **Индекс `sync/<peerDn>.jsonl` хранит СОСТОЯНИЕ СОБЕСЕДНИКА**: строки `[yyyymm, offset]` =
  сколько байт нашего месячного файла у партнёра уже есть. Справочники — через обмен
  МАНИФЕСТАМИ (size+sha1 people/catalog/device) в начале сессии; список шлём, только если наша
  версия отличается от версии партнёра. `syncCommit` пишет текущие размеры месяцев как offset.
- **DN-map**: device-data шлётся всегда; получатель строит карту DN партнёра → свой DN по pubkey.
  При вливании событий: игнорировать с DN→свой (эхо) и с DN не в карте. Конфликт одинаковых
  стартовых DN решается `renumberSelf` (меняет только одно устройство, у которого нет данных).
- **Дедуп**: `syncDedup` удаляет более поздний дубликат (совпадение всех полей кроме служебных),
  оставляя самый ранний по (edit_datetime,dev_no,rec_no). Строки `delete` не дублируем
  (ключ = target+update, `this` не в счёт); набор существующих delete строится лениво.
- people/catalog сливаются на ОДНОЙ стороне (сервер мёржит, шлёт итог; клиент принимает как есть).

## 6. Инварианты — НЕ ломать
- **Кросс-платформенная совместимость**: одинаковые имена колонок/reference, кадрирование
  обмена (`["event-tail",yyyymm,offset,size]\n<байты>\n`, `["device-data",size]\n…`, финал
  `["end"]`), хендшейк (`hello`/`ok` с db/device_no/pubkey/code/has_data/max_dn), обмен
  манифестами, сводки `{"summary":{"received":N}}`. Любая правка протокола/формата — на ОБЕИХ
  платформах, иначе кросс-тесты упадут. После правок ОБЯЗАТЕЛЬНО гонять кросс-тесты в обе стороны.
- Дробная цена: desktop пишет `12.50`, Android `12.5` — обе стороны парсят в 12.5 (ок). Целое —
  без дробной части (`250`).
- JSON пишется сырым UTF-8 (без `\uXXXX`-эскейпа кириллицы) — и boost::json, и Jackson по
  умолчанию так и делают; не включать ESCAPE_NON_ASCII.
- «Метки» переименованы только в UI: «Кому» (people), «Количество» (volume); имена
  полей/переменных не трогать.

## 7. Подводные камни (уже наступали)
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

## 8. Текущий статус (на момент написания)
Всё реализовано и зелёное:
- Desktop: `syncv2test` 41/41 (вкл. отмену во время accept и во время обмена), `guitest`
  (отмена из UI), `xcompattest` self 14/14; релиз-бинарь собирается (~946 КБ).
- Android (Jackson): `StoreSyncTest` 6/6 (вкл. `cancelInterrupts`), кросс-тесты обе стороны
  (desktop↔Android 14/14 и crossVerify без ошибок); релиз-APK собирается (~14 МБ, **unsigned**),
  Jackson внутри, kotlinx удалён.

## 9. Возможные следующие шаги
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
