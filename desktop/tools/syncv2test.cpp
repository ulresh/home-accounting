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

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <iostream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <iterator>
#include <set>
#include <string>
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

// Прокрутить главный цикл, пока не выполнится условие (или не выйдет время).
static void spin(const std::function<bool()>& ready, int limitMs = 20000) {
    QEventLoop loop;
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, [&] {
        if (ready()) loop.quit();
    });
    tick.start(5);
    QTimer::singleShot(limitMs, &loop, &QEventLoop::quit);
    if (!ready()) loop.exec();
}

// Полная синхронизация: A — сервер, B — клиент. Потоков нет: обе стороны
// работают в одном цикле событий, как в приложении (app.exec).
static std::pair<SyncResult,SyncResult> sync(Store& A, Store& B) {
    SyncServer s(A);
    SyncResult ra, rb;
    int done = 0;
    PairInfo info = s.start(YES, [&](const SyncResult& r) { ra = r; ++done; });
    info.ip = "127.0.0.1";
    SyncClient c(B);
    c.start(info, YES, [&](const SyncResult& r) { rb = r; ++done; });
    spin([&] { return done == 2; });
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

// Какие месячные файлы существуют в базе (например {"2606"}).
static std::set<std::string> monthFiles(const fs::path& root, const std::string& db) {
    std::set<std::string> out;
    fs::path base = root / db;
    if (!fs::exists(base)) return out;
    for (auto& dec : fs::directory_iterator(base)) {
        if (!dec.is_directory()) continue;
        std::string n = dec.path().filename().string();
        if (n.size() != 4 || !std::all_of(n.begin(), n.end(), ::isdigit)) continue;
        for (auto& f : fs::directory_iterator(dec.path()))
            if (f.path().extension() == ".jsonl")
                out.insert(f.path().stem().string());
    }
    return out;
}

static std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

static int countSubject(Store& s, const std::string& subj) {
    int n = 0;
    for (auto& e : s.events()) if (e->subject == subj) ++n;
    return n;
}

// Модель отдаёт события как shared_ptr — их же принимают deleteEvent/editEvent.
static std::shared_ptr<Event> findEvent(Store& s, const std::string& subj) {
    for (auto& e : s.events()) if (e->subject == subj) return e;
    return {};
}

int main(int argc, char** argv) {
    alarm(60);
    QCoreApplication app(argc, argv);

    // ============ 1. Поле comment, формат удаления/редактирования ============
    {
        std::cout << "== 1. comment + delete/this/update ==\n";
        fs::remove_all("/tmp/hv1");
        fs::path root = "/tmp/hv1/.data/home-accounting";
        Store s(root); s.load(); s.ensureIdentity();

        s.addEvent("2026-06-10", "Кофе", 250, "", "1 шт", "из кофейни");
        s.addEvent("2026-06-11", "Чай", 120, "", "", "");
        auto e1 = findEvent(s, "Кофе");
        auto e2 = findEvent(s, "Чай");

        // редактирование e2 и удаление e1
        s.editEvent(e2, "2026-06-11", "Чай зелёный", 130, "", "", "акция");
        s.deleteEvent(e1);

        std::string content = readAllMonths(root, "Основная");
        check(content.find("\"comment\"") != std::string::npos, "header содержит comment");
        check(content.find("\"reference\"") != std::string::npos, "header содержит reference");
        check(content.find("\"из кофейни\"") != std::string::npos, "комментарий записан в строку");
        check(content.find("\"this\"") != std::string::npos, "удаление содержит this");
        // update = ссылка на НОВУЮ запись (data.txt): {"update":["<edit>",RN,DN,"<event>"]}
        check(content.find("\"update\":[") != std::string::npos,
              "редактирование помечено ссылкой update");

        // Запись удаления обязана лежать в файле УДАЛЯЕМОГО события (месяц по
        // event_datetime), а не там, куда указывает edit_datetime («сегодня»).
        auto files = monthFiles(root, "Основная");
        check(files == (std::set<std::string>{"2606"}),
              "все записи, включая удаления, попали в файл события 2606.jsonl");
        std::string june = readFile(root / "Основная" / "2020" / "2606.jsonl");
        check(june.find("\"delete\":[") != std::string::npos,
              "запись удаления лежит в файле удаляемого события");

        // перезагрузка: e1 удалён, e2 заменён
        Store s2(root); s2.load();
        check(countSubject(s2, "Кофе") == 0, "удалённое не видно после перезагрузки");
        check(countSubject(s2, "Чай") == 0, "старая версия после правки не видна");
        check(countSubject(s2, "Чай зелёный") == 1, "новая версия после правки видна");
        bool gotComment = false;
        for (auto& e : s2.events())
            if (e->subject == "Чай зелёный" && e->comment == "акция") gotComment = true;
        check(gotComment, "комментарий читается после перезагрузки");
    }

    // ====== 1б. Правка с переносом события в другой месяц ======
    {
        std::cout << "== 1б. правка переносит событие в другой месяц ==\n";
        fs::remove_all("/tmp/hv1b");
        fs::path root = "/tmp/hv1b/.data/home-accounting";
        Store s(root); s.load(); s.ensureIdentity();

        s.addEvent("2026-06-20", "Билет", 500, "", "", "");
        // Дата события переезжает в май. Новое событие уходит в майский файл,
        // а удаление старого остаётся в июньском — рядом с удаляемой записью.
        s.editEvent(findEvent(s, "Билет"), "2026-05-20", "Билет", 500, "", "", "");

        auto files = monthFiles(root, "Основная");
        check(files == (std::set<std::string>{"2605", "2606"}),
              "новое событие ушло в файл своего месяца (2605 + 2606)");
        std::string june = readFile(root / "Основная" / "2020" / "2606.jsonl");
        std::string may  = readFile(root / "Основная" / "2020" / "2605.jsonl");
        check(june.find("\"delete\":[") != std::string::npos,
              "удаление старой версии — в файле старого события (2606)");
        check(may.find("\"delete\":[") == std::string::npos,
              "в файле нового события записи удаления нет (2605)");

        Store s2(root); s2.load();
        check(countSubject(s2, "Билет") == 1, "после перезагрузки ровно одна версия");
        auto b = findEvent(s2, "Билет");
        check(b && b->event_datetime == "2026-05-20", "видна новая дата события");
    }

    // ====== 1в. Поиск удалений в MonthDeletions::ops ======
    {
        std::cout << "== 1в. поиск удалений в MonthDeletions::ops ==\n";
        fs::remove_all("/tmp/hv1c");
        fs::path root = "/tmp/hv1c/.data/home-accounting";
        Store s(root); s.load(); s.ensureIdentity();
        s.addEvent("2026-06-10", "Кофе", 250, "", "", "");
        s.addEvent("2026-06-11", "Чай", 120, "", "", "");
        s.addEvent("2026-07-05", "Хлеб", 40, "", "", "");
        auto coffee = findEvent(s, "Кофе");            // не удаляем
        auto tea    = findEvent(s, "Чай");
        auto bread  = findEvent(s, "Хлеб");
        s.deleteEvent(tea);
        s.deleteEvent(bread);

        fs::path june = root / "Основная" / "2020" / "2606.jsonl";
        fs::path july = root / "Основная" / "2020" / "2607.jsonl";
        MonthDeletions md;
        md.read(june);
        check(md.ops.size() == 1, "из июньского файла прочитано одно удаление");
        check(md.ops.contains(*tea),
              "удалённая запись находится по своей идентичности");
        check(!md.ops.contains(*coffee), "неудалённая запись не находится");
        // ключ поиска — (edit_datetime, rec_no, dev_no), а не дата события
        Event twin = *tea;
        twin.rec_no = tea->rec_no + 1;
        check(!md.ops.contains(twin),
              "другая запись с той же датой события не находится");

        MonthDeletions all;
        all.read(june);
        all.read(july);
        check(all.ops.size() == 2, "удаления двух месяцев собраны в один ops");
        check(all.ops.contains(*bread), "июльское удаление найдено");
        check(all.ops.contains(*tea), "июньское удаление тоже на месте");
        auto r = all.ops.equal_range(*tea);
        check(std::distance(r.first, r.second) == 1 &&
              r.first->del.event_datetime == "2026-06-11",
              "equal_range даёт ровно одно удаление и это «Чай»");
    }

    // ============ 2. Схемо-зависимый разбор чужого порядка/состава ============
    {
        std::cout << "== 2. чужой порядок/состав колонок и reference ==\n";
        fs::remove_all("/tmp/hv2");
        fs::path root = "/tmp/hv2/.data/home-accounting";
        Store s(root); s.load(); s.ensureIdentity();
        // наша запись (каноническая схема)
        s.addEvent("2026-06-01", "Молоко", 80, "", "", "");

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
            if (e->subject == "Хлеб") {
                bread = true;
                comm = (e->comment == "свежий");
                check(e->cost == 55, "чужая запись: cost разобран по схеме");
                check(e->dev_no == 9, "чужая запись: dev_no разобран по схеме");
                check(e->event_datetime == "2025-03-01", "чужая запись: event_datetime по схеме");
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

        A.addEvent("2026-06-02", "Сахар", 60, "", "", "");
        A.addEvent("2026-06-03", "Соль", 30, "", "", "");
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
        A.addEvent("2026-06-04", "Перец", 90, "", "", "");
        auto [r3a, r3b] = sync(A, B);
        check(r3b.received == 1, "инкремент: B принял ровно 1 новое событие"
              " (принято " + std::to_string(r3b.received) + ")");
        check(countSubject(B, "Перец") == 1, "новое событие появилось у B");

        // B меняет людей — A получает обновление при следующей синхронизации
        B.addPerson("Пётр");
        auto [r4a, r4b] = sync(A, B);
        (void)r4a; (void)r4b;
        bool aHasPetr = false;
        for (auto& p : A.people()) if (p.first == "Пётр") aHasPetr = true;
        check(aHasPetr, "A получил нового человека от B");
        bool bHasMaria = false, bHasPetr = false;
        for (auto& p : B.people()) { if (p.first == "Мария") bHasMaria = true; if (p.first == "Пётр") bHasPetr = true; }
        check(bHasMaria && bHasPetr, "у B итоговый список людей (Мария+Пётр)");
    }

    // ====== 3б. dnMap: dev_no переписывается при записи в файл ======
    {
        std::cout << "== 3б. dnMap при записи события/удаления ==\n";
        fs::remove_all("/tmp/hv3ba"); fs::remove_all("/tmp/hv3bc");
        fs::path ra = "/tmp/hv3ba/.data/home-accounting";
        fs::path rc = "/tmp/hv3bc/.data/home-accounting";
        Store A(ra); A.load(); A.ensureIdentity();
        Store C(rc); C.load(); C.ensureIdentity();
        // У C своя нумерация: DN 1 занят им самим, 2 и 3 — посторонними,
        // поэтому DN устройства A обязан смениться при сопряжении.
        C.addDevice("PUBKEY-X");
        C.addDevice("PUBKEY-Y");
        C.addEvent("2026-08-01", "Хлеб", 40, "", "", "");   // C не пустой

        A.addEvent("2026-08-01", "Молоко", 80, "", "1 л", "");
        sync(A, C);

        int dnAinC = C.knowsDevice(A.myPubkey());
        check(dnAinC > 0, "C знает устройство A");
        check(dnAinC != C.deviceNo(), "DN устройства A у C не совпадает с его собственным");
        auto milk = findEvent(C, "Молоко");
        check(milk && milk->dev_no == dnAinC, "в памяти C у события DN автора в системе C");

        // Главное: то же самое должно быть ЗАПИСАНО в файл C.
        Store C2(rc); C2.load();
        auto milk2 = findEvent(C2, "Молоко");
        check(milk2 != nullptr, "после перезагрузки C событие на месте");
        check(milk2 && milk2->dev_no == dnAinC,
              "в файле C dev_no заменён по dnMap на DN системы C");
        check(milk2 && milk2->dev_no != C2.deviceNo(),
              "событие не выглядит как своё собственное у C");

        // Удаление тоже должно приехать с переписанными ссылками, иначе после
        // перезагрузки оно не найдёт своё событие.
        A.deleteEvent(findEvent(A, "Молоко"));
        sync(A, C);
        check(countSubject(C, "Молоко") == 0, "удаление дошло до C");
        Store C3(rc); C3.load();
        check(countSubject(C3, "Молоко") == 0,
              "в файле C ссылки удаления переписаны: событие не воскресает");
        check(countSubject(C3, "Хлеб") == 1, "собственное событие C не задето");
    }

    // ====== 3в. Чужая схема проходит транзитом без потерь ======
    {
        std::cout << "== 3в. чужая схема: состав и порядок полей сохраняются ==\n";
        fs::remove_all("/tmp/hv3ca"); fs::remove_all("/tmp/hv3cb");
        fs::path ra = "/tmp/hv3ca/.data/home-accounting";
        fs::path rb = "/tmp/hv3cb/.data/home-accounting";
        Store A(ra); A.load(); A.ensureIdentity();
        Store B(rb); B.load(); B.ensureIdentity();

        A.addEvent("2026-09-01", "Молоко", 80, "", "", "");
        sync(A, B);                       // сопряжение: дальше пойдёт инкремент

        // Дописываем в файл A блок «чужой» схемы: другой порядок колонок и
        // неизвестная нам колонка tags.
        int dnFar = A.addDevice("PUBKEY-FAR");
        {
            std::ofstream o(ra / "Основная" / "2020" / "2609.jsonl",
                            std::ios::binary | std::ios::app);
            o << R"({"header":["dev_no","rec_no","edit_datetime","cost","subject",)"
                 R"("event_datetime","comment","tags"],)"
                 R"("reference":["dev_no","rec_no","edit_datetime"]})" << "\n";
            o << "[" << dnFar << R"(,0,"2026-09-02 00:00:00",55,"Хлеб",)"
                 R"("2026-09-02","свежий",["акция","хлеб"]])" << "\n";
        }
        sync(A, B);

        int dnFarInB = B.knowsDevice("PUBKEY-FAR");
        check(dnFarInB > 0, "B узнал о дальнем устройстве из списка A");
        std::string got = readFile(rb / "Основная" / "2020" / "2609.jsonl");
        std::string want = "[" + std::to_string(dnFarInB) +
            R"(,0,"2026-09-02 00:00:00",55,"Хлеб","2026-09-02","свежий",)"
            R"(["акция","хлеб"]])";
        check(got.find(want) != std::string::npos,
              "строка записана как пришла, заменён только dev_no");
        check(got.find(R"("tags")") != std::string::npos,
              "заголовок чужой схемы записан вместе со строкой");
        check(countSubject(B, "Хлеб") == 1, "событие чужой схемы видно у B");
        Store B2(rb); B2.load();
        auto bread = findEvent(B2, "Хлеб");
        check(bread && bread->dev_no == dnFarInB,
              "после перезагрузки B у события DN дальнего устройства в системе B");
        check(bread && bread->comment == "свежий",
              "поля чужой схемы читаются после перезагрузки");
    }

    // ============ 4. Удаление более позднего дубликата при синхронизации ============
    {
        std::cout << "== 4. дедупликация одинаковых событий ==\n";
        fs::remove_all("/tmp/hv4a"); fs::remove_all("/tmp/hv4b");
        fs::path ra = "/tmp/hv4a/.data/home-accounting";
        fs::path rb = "/tmp/hv4b/.data/home-accounting";
        Store A(ra); A.load(); A.ensureIdentity();
        Store B(rb); B.load(); B.ensureIdentity();

        A.addEvent("2026-06-05", "Яблоки", 150, "", "1 кг", "");
        sync(A, B);   // B получает DN и копию «Яблок»
        check(countSubject(B, "Яблоки") == 1, "B получил Яблоки");

        // B независимо заводит идентичное по содержимому событие (своё DN)
        B.addEvent("2026-06-05", "Яблоки", 150, "", "1 кг", "");
        check(countSubject(B, "Яблоки") == 2, "до синхронизации у B два одинаковых");

        auto [ra2, rb2] = sync(A, B);
        (void)ra2; (void)rb2;
        check(countSubject(A, "Яблоки") == 1, "после дедупликации у A одно Яблоко");
        check(countSubject(B, "Яблоки") == 1, "после дедупликации у B одно Яблоко");
        {   // отдельно: что лежит на диске (память и файлы могут разойтись)
            Store B2(rb); B2.load();
            check(countSubject(B2, "Яблоки") == 1,
                  "после дедупликации в файлах B одно Яблоко");
        }

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

        A.addEvent("2026-06-06", "Книга", 700, "", "", "");
        auto ev = findEvent(A, "Книга");
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

    // ====== 5б. Новое событие с датой ранее удалённого ======
    {
        std::cout << "== 5б. новое событие с датой ранее удалённого ==\n";
        fs::remove_all("/tmp/hv5c"); fs::remove_all("/tmp/hv5d");
        fs::path ra = "/tmp/hv5c/.data/home-accounting";
        fs::path rb = "/tmp/hv5d/.data/home-accounting";
        Store A(ra); A.load(); A.ensureIdentity();
        Store B(rb); B.load(); B.ensureIdentity();

        A.addEvent("2026-06-11", "Чай", 120, "", "", "");
        sync(A, B);
        check(countSubject(B, "Чай") == 1, "B получил «Чай»");

        A.deleteEvent(findEvent(A, "Чай"));
        sync(A, B);
        check(countSubject(B, "Чай") == 0, "удаление «Чая» дошло до B");

        // Другая запись (свои edit_datetime/rec_no), но та же дата события.
        A.addEvent("2026-06-11", "Кефир", 55, "", "", "");
        sync(A, B);
        check(countSubject(B, "Кефир") == 1,
              "новое событие с датой удалённого не потеряно у B");
        Store B2(rb); B2.load();
        check(countSubject(B2, "Кефир") == 1,
              "оно же есть в файлах B после перезагрузки");
    }

    // ============ 6. Прерывание синхронизации (cancel в любом месте) ============
    {
        std::cout << "== 6. прерывание синхронизации ==\n";

        // (а) cancel во время ожидания подключения.
        {
            fs::remove_all("/tmp/hv6a");
            Store A("/tmp/hv6a/.data/home-accounting"); A.load(); A.ensureIdentity();
            SyncServer s(A);
            SyncResult r;
            bool done = false;
            QObject ctx;                  // умрёт вместе с областью — снимет таймер
            auto t0 = std::chrono::steady_clock::now();
            s.start(YES, [&](const SyncResult& x) { r = x; done = true; });
            QTimer::singleShot(150, &ctx, [&] { s.cancel(); });
            spin([&] { return done; }, 3000);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            check(done && !r.ok, "ожидание прервано (не выполнено)");
            check(ms < 2000, "прерывание сработало быстро, без зависания");
        }

        // (б) cancel во время активного обмена: клиент подключился, сервер прерван.
        {
            fs::remove_all("/tmp/hv6b"); fs::remove_all("/tmp/hv6c");
            Store A("/tmp/hv6b/.data/home-accounting"); A.load(); A.ensureIdentity();
            Store B("/tmp/hv6c/.data/home-accounting"); B.load(); B.ensureIdentity();
            for (int i = 0; i < 50; ++i)
                A.addEvent("2026-06-07", "Товар" + std::to_string(i), 10 + i, "", "", "");

            SyncServer s(A);
            SyncResult ra, rb;
            int done = 0;
            PairInfo info = s.start(YES, [&](const SyncResult& r) { ra = r; ++done; });
            info.ip = "127.0.0.1";
            SyncClient c(B);
            c.start(info, YES, [&](const SyncResult& r) { rb = r; ++done; });
            QObject ctx;                  // умрёт вместе с областью — снимет таймер
            QTimer::singleShot(60, &ctx, [&] { s.cancel(); });  // прервать посреди обмена
            spin([&] { return done == 2; }, 5000);
            // Главное требование: прерывание срабатывает и нет зависания/краша.
            check(done == 2, "обмен прерван без зависания/краша (server ok=" +
                        std::to_string(ra.ok) + ")");
        }
    }

    // ====== 7. Большой объём: многоблочная потоковая передача ======
    {
        std::cout << "== 7. большой объём (многоблочный поток) ==\n";
        fs::remove_all("/tmp/hv7a"); fs::remove_all("/tmp/hv7b");
        fs::path ra = "/tmp/hv7a/.data/home-accounting";
        fs::path rb = "/tmp/hv7b/.data/home-accounting";
        Store A(ra); A.load(); A.ensureIdentity();
        Store B(rb); B.load(); B.ensureIdentity();

        // Заведомо больше одного блока (64 КБ) и больше порога отправки (256 КБ),
        // чтобы задействовать и дробление файла при записи в сокет, и разбор
        // значения, разорванного между блоками приёма.
        const int N = 20000;
        for (int i = 0; i < N; ++i)
            A.addEvent("2026-10-05", "Позиция" + std::to_string(i), 1 + i % 97,
                       "", "", "комментарий подлиннее для объёма " + std::to_string(i));
        auto size = fs::file_size(ra / "Основная" / "2020" / "2610.jsonl");
        check(size > 512 * 1024, "файл событий больше 512 КБ (" +
              std::to_string(size / 1024) + " КБ)");

        auto [r7a, r7b] = sync(A, B);
        check(r7a.ok && r7b.ok, "синхронизация большого объёма прошла");
        check((int)B.events().size() == N,
              "B получил все события (" + std::to_string(B.events().size()) +
              " из " + std::to_string(N) + ")");
        check(fs::file_size(rb / "Основная" / "2020" / "2610.jsonl") == size,
              "месячный файл B побайтно того же размера");
        Store B2(rb); B2.load();
        check((int)B2.events().size() == N, "после перезагрузки B события на месте");

        // Инкремент поверх большого файла: одно новое событие.
        A.addEvent("2026-10-06", "Хвост", 5, "", "", "");
        auto [r7c, r7d] = sync(A, B);
        (void)r7c;
        // Передаётся только хвост, а не весь файл заново (в счётчик попадает
        // ещё и строка заголовка схемы — см. известные красные тесты п.3).
        check(r7d.received > 0 && r7d.received <= 2,
              "инкремент поверх большого файла: принято " +
              std::to_string(r7d.received) + " (не весь файл)");
        check(countSubject(B, "Хвост") == 1, "хвостовое событие дошло до B");
    }

    // ====== 8. Отказы сопряжения: неверный код и разные базы ======
    // Заодно проверка того, что последнее сообщение сервера успевает уйти
    // до закрытия соединения.
    {
        std::cout << "== 8. неверный код и разные базы ==\n";
        fs::remove_all("/tmp/hv8a"); fs::remove_all("/tmp/hv8b");
        fs::path ra = "/tmp/hv8a/.data/home-accounting";
        fs::path rb = "/tmp/hv8b/.data/home-accounting";
        Store A(ra); A.load(); A.ensureIdentity();
        A.addEvent("2026-11-01", "Соль", 20, "", "", "");
        Store B(rb); B.load(); B.ensureIdentity();
        B.addEvent("2026-11-02", "Перец", 30, "", "", "");

        {   // (а) клиент пришёл с неверным кодом
            SyncServer s(A);
            SyncResult r1, r2;
            int done = 0;
            PairInfo info = s.start(YES, [&](const SyncResult& r) { r1 = r; ++done; });
            info.ip = "127.0.0.1";
            info.code = "WRONGCOD";
            SyncClient c(B);
            c.start(info, YES, [&](const SyncResult& r) { r2 = r; ++done; });
            spin([&] { return done == 2; }, 5000);
            check(done == 2, "обе стороны завершились");
            check(r2.error == "bad_code",
                  "клиент получил отказ по коду (" + r2.error + ")");
            check(!r1.ok && !r2.ok, "синхронизация не выполнена");
        }
        {   // (б) у сторон разные базы
            B.switchDatabase("Другая", true);
            SyncServer s(A);
            SyncResult r1, r2;
            int done = 0;
            PairInfo info = s.start(YES, [&](const SyncResult& r) { r1 = r; ++done; });
            info.ip = "127.0.0.1";
            SyncClient c(B);
            c.start(info, YES, [&](const SyncResult& r) { r2 = r; ++done; });
            spin([&] { return done == 2; }, 5000);
            check(done == 2, "обе стороны завершились");
            check(r2.error == "db_mismatch",
                  "клиент получил db_mismatch (" + r2.error + ")");
            check(r2.peerDb == "Основная",
                  "вместе с отказом пришло имя базы партнёра (" + r2.peerDb + ")");
            check(countSubject(B, "Соль") == 0, "события чужой базы не приняты");
        }
    }

    std::cout << "\n==== итог: " << (g_total - g_fail) << "/" << g_total << " пройдено ====\n";
    return g_fail == 0 ? 0 : 1;
}
