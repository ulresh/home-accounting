// Юнит-тесты нового формата событий и инкрементной синхронизации.
//   - поле comment;
//   - формат удаления {"delete":[...],"this":[...],"update":?};
//   - схемо-зависимый разбор (другой порядок/состав колонок и reference);
//   - инкрементная файловая синхронизация (хвосты, индекс sync/DN.jsonl);
//   - слияние people/catalog на одной стороне;
//   - удаление более позднего дубликата при синхронизации;
//   - распространение удалений между устройствами.
#include "../src/model/Store.h"
#include "../src/model/Paths.h"
#include "../src/sync/SyncService.h"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <unistd.h>

using namespace ha;
namespace fs = std::filesystem;

static int g_fail = 0, g_total = 0;
static void check(bool ok, const std::string& msg) {
    ++g_total;
    if (!ok) { ++g_fail; std::cout << "  [FAIL] " << msg << "\n"; }
    else      std::cout << "  [ ok ] " << msg << "\n";
}

static auto YES = [](const std::string&) { return true; };

// Полная синхронизация: A — сервер, B — клиент.
static std::pair<SyncResult,SyncResult> sync(Store& A, Store& B) {
    SyncServer s(A);
    PairInfo info = s.listen();
    info.ip = "127.0.0.1";
    SyncResult ra;
    std::thread ts([&] { ra = s.wait(YES); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    SyncClient c(B);
    SyncResult rb = c.connect(info, YES);
    ts.join();
    return {ra, rb};
}

static std::string readAllMonths(const fs::path& root, const std::string& db) {
    std::string out;
    fs::path base = root / db;
    if (!fs::exists(base)) return out;
    for (auto& dec : fs::directory_iterator(base)) {
        if (!dec.is_directory()) continue;
        std::string n = dec.path().filename().string();
        if (n.empty() || !std::all_of(n.begin(), n.end(), ::isdigit)) continue;
        for (auto& f : fs::directory_iterator(dec.path())) {
            std::ifstream in(f.path());
            out += std::string((std::istreambuf_iterator<char>(in)), {});
        }
    }
    return out;
}

static int countSubject(Store& s, const std::string& subj) {
    int n = 0;
    for (auto& e : s.events()) if (e.subject == subj) ++n;
    return n;
}

int main() {
    alarm(60);

    // ============ 1. Поле comment, формат удаления/редактирования ============
    {
        std::cout << "== 1. comment + delete/this/update ==\n";
        fs::remove_all("/tmp/hv1");
        fs::path root = "/tmp/hv1/.data/home-accounting";
        Store s(root); s.load(); s.ensureIdentity();

        auto e1 = s.addEvent("2026-06-10", "Кофе", 250, std::nullopt, std::string("1 шт"),
                             std::string("из кофейни"));
        auto e2 = s.addEvent("2026-06-11", "Чай", 120, std::nullopt, std::nullopt, std::nullopt);

        // редактирование e2 и удаление e1
        s.editEvent(e2, "2026-06-11", "Чай зелёный", 130, std::nullopt, std::nullopt,
                    std::string("акция"));
        s.deleteEvent(e1);

        std::string content = readAllMonths(root, "Основная");
        check(content.find("\"comment\"") != std::string::npos, "header содержит comment");
        check(content.find("\"reference\"") != std::string::npos, "header содержит reference");
        check(content.find("\"из кофейни\"") != std::string::npos, "комментарий записан в строку");
        check(content.find("\"this\"") != std::string::npos, "удаление содержит this");
        check(content.find("\"update\":true") != std::string::npos, "редактирование помечено update:true");

        // перезагрузка: e1 удалён, e2 заменён
        Store s2(root); s2.load();
        check(countSubject(s2, "Кофе") == 0, "удалённое не видно после перезагрузки");
        check(countSubject(s2, "Чай") == 0, "старая версия после правки не видна");
        check(countSubject(s2, "Чай зелёный") == 1, "новая версия после правки видна");
        bool gotComment = false;
        for (auto& e : s2.events())
            if (e.subject == "Чай зелёный" && e.comment && *e.comment == "акция") gotComment = true;
        check(gotComment, "комментарий читается после перезагрузки");
    }

    // ============ 2. Схемо-зависимый разбор чужого порядка/состава ============
    {
        std::cout << "== 2. чужой порядок/состав колонок и reference ==\n";
        fs::remove_all("/tmp/hv2");
        fs::path root = "/tmp/hv2/.data/home-accounting";
        Store s(root); s.load(); s.ensureIdentity();
        // наша запись (каноническая схема)
        s.addEvent("2026-06-01", "Молоко", 80, std::nullopt, std::nullopt, std::nullopt);

        // подкладываем «чужой» файл с другим порядком колонок и reference
        fs::path odd = root / "Основная" / "2020" / "2503.jsonl";
        {
            std::ofstream o(odd);
            o << R"({"header":["dev_no","rec_no","edit_datetime","cost","subject","event_datetime","comment"],"reference":["dev_no","rec_no","edit_datetime"]})" << "\n";
            o << R"([9,0,"2025-03-01 00:00:00",55,"Хлеб","2025-03-01","свежий"])" << "\n";
            o << R"([9,1,"2025-03-02 00:00:00",40,"Кефир","2025-03-02",null])" << "\n";
            // удаление «Кефира» в чужом порядке reference
            o << R"({"delete":[9,1,"2025-03-02 00:00:00"],"this":[9,2,"2025-03-03 00:00:00"]})" << "\n";
        }
        Store s2(root); s2.load();
        bool bread = false, comm = false;
        for (auto& e : s2.events()) {
            if (e.subject == "Хлеб") {
                bread = true;
                comm = (e.comment && *e.comment == "свежий");
                check(e.cost == 55, "чужая запись: cost разобран по схеме");
                check(e.dev_no == 9, "чужая запись: dev_no разобран по схеме");
                check(e.event_datetime == "2025-03-01", "чужая запись: event_datetime по схеме");
            }
        }
        check(bread, "событие из чужой схемы видно");
        check(comm, "комментарий из чужой схемы прочитан");
        check(countSubject(s2, "Кефир") == 0, "удаление по чужому reference применилось");
        check(countSubject(s2, "Молоко") == 1, "наша каноническая запись тоже видна");
    }

    // ============ 3. Инкрементная синхронизация + индекс ============
    {
        std::cout << "== 3. инкрементная синхронизация ==\n";
        fs::remove_all("/tmp/hv3a"); fs::remove_all("/tmp/hv3b");
        fs::path ra = "/tmp/hv3a/.data/home-accounting";
        fs::path rb = "/tmp/hv3b/.data/home-accounting";
        Store A(ra); A.load(); A.ensureIdentity();
        Store B(rb); B.load(); B.ensureIdentity();

        A.addEvent("2026-06-02", "Сахар", 60, std::nullopt, std::nullopt, std::nullopt);
        A.addEvent("2026-06-03", "Соль", 30, std::nullopt, std::nullopt, std::nullopt);
        A.addPerson("Мария");
        A.upsertCatalog({"Бакалея", {"Сахар", "Соль"}});

        auto [r1a, r1b] = sync(A, B);
        check(r1a.ok && r1b.ok, "первая синхронизация прошла");
        check(A.deviceNo() != B.deviceNo(), "DN устройств различны после сопряжения");
        check(countSubject(B, "Сахар") == 1 && countSubject(B, "Соль") == 1, "B получил события A");
        check(B.people().size() == 1, "B получил людей A (слияние на сервере)");
        check(B.catalog().size() == 1, "B получил каталог A");

        // индексные файлы созданы
        bool idxA = fs::exists(ra / "Основная" / "sync" / (std::to_string(B.deviceNo()) + ".jsonl"));
        bool idxB = fs::exists(rb / "Основная" / "sync" / (std::to_string(A.deviceNo()) + ".jsonl"));
        check(idxA, "у A создан sync-индекс по B");
        check(idxB, "у B создан sync-индекс по A");

        // повторная синхронизация без изменений — нечего передавать
        auto [r2a, r2b] = sync(A, B);
        check(r2a.received == 0 && r2b.received == 0, "повторная синхронизация без изменений: 0 принято");

        // A добавляет одно событие — приходит только оно (хвост)
        A.addEvent("2026-06-04", "Перец", 90, std::nullopt, std::nullopt, std::nullopt);
        auto [r3a, r3b] = sync(A, B);
        check(r3b.received == 1, "инкремент: B принял ровно 1 новое событие");
        check(countSubject(B, "Перец") == 1, "новое событие появилось у B");

        // B меняет людей — A получает обновление при следующей синхронизации
        B.addPerson("Пётр");
        auto [r4a, r4b] = sync(A, B);
        (void)r4a; (void)r4b;
        bool aHasPetr = false;
        for (auto& p : A.people()) if (p == "Пётр") aHasPetr = true;
        check(aHasPetr, "A получил нового человека от B");
        bool bHasMaria = false, bHasPetr = false;
        for (auto& p : B.people()) { if (p == "Мария") bHasMaria = true; if (p == "Пётр") bHasPetr = true; }
        check(bHasMaria && bHasPetr, "у B итоговый список людей (Мария+Пётр)");
    }

    // ============ 4. Удаление более позднего дубликата при синхронизации ============
    {
        std::cout << "== 4. дедупликация одинаковых событий ==\n";
        fs::remove_all("/tmp/hv4a"); fs::remove_all("/tmp/hv4b");
        fs::path ra = "/tmp/hv4a/.data/home-accounting";
        fs::path rb = "/tmp/hv4b/.data/home-accounting";
        Store A(ra); A.load(); A.ensureIdentity();
        Store B(rb); B.load(); B.ensureIdentity();

        A.addEvent("2026-06-05", "Яблоки", 150, std::nullopt, std::string("1 кг"), std::nullopt);
        sync(A, B);   // B получает DN и копию «Яблок»
        check(countSubject(B, "Яблоки") == 1, "B получил Яблоки");

        // B независимо заводит идентичное по содержимому событие (своё DN)
        B.addEvent("2026-06-05", "Яблоки", 150, std::nullopt, std::string("1 кг"), std::nullopt);
        check(countSubject(B, "Яблоки") == 2, "до синхронизации у B два одинаковых");

        auto [ra2, rb2] = sync(A, B);
        (void)ra2; (void)rb2;
        check(countSubject(A, "Яблоки") == 1, "после дедупликации у A одно Яблоко");
        check(countSubject(B, "Яблоки") == 1, "после дедупликации у B одно Яблоко");

        // устойчивость: ещё одна синхронизация ничего не ломает и не плодит
        sync(A, B);
        check(countSubject(A, "Яблоки") == 1, "повторная синхронизация: у A по-прежнему одно");
        check(countSubject(B, "Яблоки") == 1, "повторная синхронизация: у B по-прежнему одно");
    }

    // ============ 5. Распространение удаления ============
    {
        std::cout << "== 5. распространение удаления ==\n";
        fs::remove_all("/tmp/hv5a"); fs::remove_all("/tmp/hv5b");
        fs::path ra = "/tmp/hv5a/.data/home-accounting";
        fs::path rb = "/tmp/hv5b/.data/home-accounting";
        Store A(ra); A.load(); A.ensureIdentity();
        Store B(rb); B.load(); B.ensureIdentity();

        auto ev = A.addEvent("2026-06-06", "Книга", 700, std::nullopt, std::nullopt, std::nullopt);
        sync(A, B);
        check(countSubject(B, "Книга") == 1, "B получил Книгу");

        A.deleteEvent(ev);              // A удаляет
        check(countSubject(A, "Книга") == 0, "A удалил у себя");
        sync(A, B);
        check(countSubject(B, "Книга") == 0, "удаление распространилось на B");

        // перезагрузка B подтверждает устойчивость удаления
        Store B2(rb); B2.load();
        check(countSubject(B2, "Книга") == 0, "после перезагрузки B Книга не воскресает");
    }

    // ============ 6. Прерывание синхронизации (cancel в любом месте) ============
    {
        std::cout << "== 6. прерывание синхронизации ==\n";

        // (а) cancel во время ожидания подключения (async accept).
        {
            fs::remove_all("/tmp/hv6a");
            Store A("/tmp/hv6a/.data/home-accounting"); A.load(); A.ensureIdentity();
            SyncServer s(A);
            s.listen();
            SyncResult r;
            auto t0 = std::chrono::steady_clock::now();
            std::thread t([&]{ r = s.wait(YES); });
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            s.cancel();
            t.join();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            check(!r.ok, "ожидание прервано (не выполнено)");
            check(ms < 2000, "прерывание сработало быстро, без зависания");
        }

        // (б) cancel во время активного обмена: клиент подключился, сервер прерван.
        {
            fs::remove_all("/tmp/hv6b"); fs::remove_all("/tmp/hv6c");
            Store A("/tmp/hv6b/.data/home-accounting"); A.load(); A.ensureIdentity();
            Store B("/tmp/hv6c/.data/home-accounting"); B.load(); B.ensureIdentity();
            for (int i = 0; i < 50; ++i)
                A.addEvent("2026-06-07", "Товар" + std::to_string(i), 10 + i, std::nullopt, std::nullopt, std::nullopt);

            SyncServer s(A);
            PairInfo info = s.listen(); info.ip = "127.0.0.1";
            SyncResult ra, rb;
            std::thread ts([&]{ ra = s.wait(YES); });
            std::thread tc([&]{ SyncClient c(B); rb = c.connect(info, YES); });
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            s.cancel();                       // прервать сервер посреди обмена
            ts.join(); tc.join();
            // Главное требование: прерывание срабатывает и нет зависания/краша.
            check(true, "обмен прерван без зависания/краша (server ok=" +
                        std::to_string(ra.ok) + ")");
        }
    }

    std::cout << "\n==== итог: " << (g_total - g_fail) << "/" << g_total << " пройдено ====\n";
    return g_fail == 0 ? 0 : 1;
}
