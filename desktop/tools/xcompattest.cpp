// Кросс-платформенная проверка совместимости desktop <-> Android.
//   produce <dir> : создать эталонную базу + wire-поток обмена (exchange.bin),
//                   которые затем читает unit-тест другой платформы.
//   verify  <dir> : прочитать базу и поток, созданные ДРУГОЙ платформой,
//                   и убедиться, что разбирается тот же канонический сценарий.
// Оба формата (файлы на диске и кадрирование блоков синхронизации) обязаны
// пониматься обеими платформами.
#include "../src/model/Store.h"
#include "../src/model/Paths.h"
#include <boost/json.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <string>

using namespace ha;
namespace fs = std::filesystem;
namespace json = boost::json;

static int fails = 0, total = 0;
static void check(bool ok, const std::string& m) {
    ++total;
    if (!ok) { ++fails; std::cout << "  [FAIL] " << m << "\n"; }
    else      std::cout << "  [ ok ] " << m << "\n";
}

// Единый сценарий, создаваемый обеими платформами при produce.
static void buildScenario(Store& s) {
    s.addPerson("Мария");
    s.addPerson("Пётр");
    s.upsertCatalog({"Бакалея", {"Сахар", "Соль"}});
    s.upsertCatalog({"Овощи", {"Помидоры"}});
    auto e1 = s.addEvent("2026-06-10", "Кофе", 250, std::nullopt, std::string("1 шт"), std::string("из кофейни"));
    s.addEvent("2026-06-11", "Молоко", 12.5, std::nullopt, std::string("1 л"), std::nullopt);
    auto e3 = s.addEvent("2026-06-12", "Сахар", 60, std::string("Мария"), std::nullopt, std::nullopt);
    s.editEvent(e3, "2026-06-12", "Сахар", 65, std::string("Мария"), std::nullopt, std::nullopt);
    s.deleteEvent(e1);
}

static const Event* findSubj(const std::vector<Event>& v, const std::string& subj) {
    for (auto& e : v) if (e.subject == subj) return &e;
    return nullptr;
}

static void assertScenario(Store& s, const std::string& tag) {
    auto evs = s.events();
    check((int)evs.size() == 2, tag + ": видимых событий 2");
    auto* milk = findSubj(evs, "Молоко");
    check(milk && std::fabs(milk->cost - 12.5) < 1e-9 && milk->volume && *milk->volume == "1 л",
          tag + ": Молоко 12.5 / «1 л» (дробная цена через границу платформ)");
    auto* sugar = findSubj(evs, "Сахар");
    check(sugar && std::fabs(sugar->cost - 65) < 1e-9 && sugar->people && *sugar->people == "Мария",
          tag + ": Сахар 65 / Мария (правка применилась)");
    check(findSubj(evs, "Кофе") == nullptr, tag + ": Кофе удалён");
    auto ppl = s.people();
    check(std::find(ppl.begin(), ppl.end(), "Мария") != ppl.end() &&
          std::find(ppl.begin(), ppl.end(), "Пётр") != ppl.end(), tag + ": люди Мария+Пётр");
    bool bak = false, ov = false;
    for (auto& c : s.catalog()) {
        if (c.category == "Бакалея")
            bak = std::find(c.items.begin(), c.items.end(), "Сахар") != c.items.end() &&
                  std::find(c.items.begin(), c.items.end(), "Соль") != c.items.end();
        if (c.category == "Овощи")
            ov = std::find(c.items.begin(), c.items.end(), "Помидоры") != c.items.end();
    }
    check(bak, tag + ": каталог Бакалея(Сахар,Соль)");
    check(ov, tag + ": каталог Овощи(Помидоры)");
}

// Сериализовать исходящие блоки в точном проводном кадрировании.
static void writeExchange(Store& s, const fs::path& file) {
    s.syncBegin(99);
    auto blobs = s.syncBuildOutgoing(true);
    std::ofstream o(file, std::ios::binary);
    for (auto& b : blobs) {
        json::array h;
        if (b.kind == "event-tail") {
            h.emplace_back("event-tail"); h.emplace_back(b.month);
            h.emplace_back((int64_t)b.offset); h.emplace_back((int64_t)b.data.size());
        } else {
            h.emplace_back(b.kind); h.emplace_back((int64_t)b.data.size());
        }
        o << json::serialize(h) << "\n" << b.data << "\n";
    }
    o << "[\"end\"]\n";
    s.syncEnd();
}

// Разобрать проводной поток ДРУГОЙ платформы и применить как при синхронизации.
static void applyExchange(Store& s, const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t pos = 0, n = all.size();
    s.syncBegin(99);
    while (pos < n) {
        size_t nl = all.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string line = all.substr(pos, nl - pos);
        pos = nl + 1;
        if (line.empty()) continue;
        auto v = json::parse(line);
        auto& a = v.as_array();
        std::string kind = std::string(a[0].as_string());
        if (kind == "end") break;
        SyncBlob b; b.kind = kind;
        long long size;
        if (kind == "event-tail") {
            b.month = (int)a[1].as_int64(); b.offset = a[2].as_int64(); size = a[3].as_int64();
        } else size = a[1].as_int64();
        b.data = all.substr(pos, (size_t)size);
        pos += (size_t)size;
        if (pos < n && all[pos] == '\n') pos++;
        s.syncApply(b, true, nullptr);
    }
    s.syncEnd();
}

int main(int argc, char** argv) {
    if (argc < 3) { std::cout << "usage: xcompattest produce|verify <dir>\n"; return 2; }
    std::string mode = argv[1];
    fs::path dir = argv[2];

    if (mode == "produce") {
        fs::remove_all(dir);
        Store s(dir); s.load(); s.ensureIdentity();
        buildScenario(s);
        writeExchange(s, dir / "exchange.bin");
        std::cout << "desktop-эталон создан в " << dir << "\n";
        return 0;
    }
    if (mode == "verify") {
        std::cout << "== desktop читает базу ДРУГОЙ платформы ==\n";
        Store s(dir); s.load();
        assertScenario(s, "db");

        std::cout << "== desktop принимает поток обмена ДРУГОЙ платформы ==\n";
        fs::path tmp = "/tmp/xc_desk_consumer";
        fs::remove_all(tmp);
        Store c(tmp); c.load(); c.ensureIdentity();
        applyExchange(c, dir / "exchange.bin");
        assertScenario(c, "exchange");

        std::cout << "\n=== итог: " << (total - fails) << "/" << total << " ===\n";
        return fails == 0 ? 0 : 1;
    }
    std::cout << "unknown mode\n";
    return 2;
}
