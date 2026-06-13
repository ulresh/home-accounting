// Автономная проверка модели (без Qt): запись/чтение событий и формат файлов.
#include "../src/model/Store.h"
#include "../src/model/Paths.h"
#include <iostream>
#include <fstream>
#include <filesystem>

using namespace ha;
namespace fs = std::filesystem;

int main() {
    fs::path root = "/tmp/hamodel/.data/home-accounting";
    fs::remove_all("/tmp/hamodel");
    fs::create_directories(root);

    Store s(root);
    s.load();
    s.ensureIdentity();

    s.addPerson("Мария");
    s.upsertCatalog({"Фрукты", {"Вишня", "Черешня"}});
    s.upsertCatalog({"Мебель", {"Стол"}});

    auto e1 = s.addEvent("2026-06-12 10:00", "Черешня", 1200, std::nullopt, std::string("2 кг"));
    auto e2 = s.addEvent("2026-06-12 10:00", "Помидоры", 300, std::nullopt, std::string("1.3 кг"));
    auto e3 = s.addEvent("2026-06-05", "Джинсы", 3760, std::string("Мария"), std::nullopt);
    auto e4 = s.addEvent("2026-06-06", "Стол", 8200, std::nullopt, std::nullopt);
    (void)e2; (void)e4;

    // правка e3 (удаление + новое), удаление e1
    s.editEvent(e3, "2026-06-05", "Джинсы", 3990, std::string("Мария"), std::nullopt);
    s.deleteEvent(e1);

    // Проверка: границы значений определяет парсер, а не перевод строки.
    // Пишем файл, где одно значение разбито на строки, а несколько значений —
    // в одной строке и даже подряд без разделителя.
    {
        fs::path odd = root / "Основная" / "2020" / "2605.jsonl";
        std::ofstream o(odd);
        o << R"({"header":["event_datetime","subject","cost","edit_datetime","rec_no","dev_no","people","volume"]})" << "\n";
        o << R"(["2025-01-01","Хлеб",)" << "\n" << R"(50,"2025-01-01 00:00:00",0,9])" << "\n";   // одно значение на 2 строки
        o << R"(["2025-01-02","Молоко",80,"2025-01-02 00:00:00",1,9] ["2025-01-03","Яйца",120,"2025-01-03 00:00:00",2,9])"
          << R"(["2025-01-04","Соль",30,"2025-01-04 00:00:00",3,9])" << "\n"; // 3 значения в одной строке, два подряд без пробела
    }

    std::cout << "=== month file 2020/2606.jsonl ===\n";
    std::ifstream in(root / "Основная" / "2020" / "2606.jsonl");
    std::string line;
    while (std::getline(in, line)) std::cout << line << "\n";

    std::cout << "\n=== reload -> visible events ===\n";
    Store s2(root);
    s2.load();
    for (auto& e : s2.events())
        std::cout << e.event_datetime << " | " << e.subject << " | " << e.cost
                  << " | " << (e.people ? *e.people : "-")
                  << " | " << (e.volume ? *e.volume : "-")
                  << "  (cat=" << s2.categoryOf(e.subject) << ")\n";
    return 0;
}
