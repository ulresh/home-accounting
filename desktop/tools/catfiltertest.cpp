// Проверка умного фильтра, раскрытия вложенных категорий и перезаписи каталога.
#include "../src/model/Store.h"
#include <iostream>
#include <filesystem>

using namespace ha;
namespace fs = std::filesystem;

static void show(Store& s, const std::string& q) {
    std::cout << "filter(\"" << q << "\") -> ";
    for (auto& e : s.filter(q)) std::cout << e.subject << " ";
    std::cout << "\n";
}

int main() {
    fs::remove_all("/tmp/hacat");
    Store s(fs::path("/tmp/hacat/.data/home-accounting"));
    s.load(); s.ensureIdentity();

    s.addPerson("Мария");
    s.replaceCatalog({
        {"Фрукты", {"Вишня", "Черешня"}},
        {"Мебель", {"Стол"}},
        {"Продукты", {"Фрукты", "Помидоры"}},   // вложенная категория Фрукты + Помидоры
    });

    s.addEvent("2026-06-01", "Помидоры", 300, std::nullopt, std::nullopt);
    s.addEvent("2026-06-02", "Стол", 8200, std::nullopt, std::nullopt);
    s.addEvent("2026-06-03", "Джинсы", 3760, std::string("Мария"), std::nullopt);
    s.addEvent("2026-06-04", "Вишня", 120, std::nullopt, std::string("1 кг"));

    std::cout << "categoryMembers(Продукты) = ";
    for (auto& m : s.categoryMembers("Продукты")) std::cout << m << " ";
    std::cout << "\n\n";

    show(s, "");           // все
    show(s, "Продукты");   // категория с вложением -> Помидоры, Вишня
    show(s, "Мебель");     // категория -> Стол
    show(s, "Мария");      // человек -> Джинсы
    show(s, "виш");        // подстрока наименования -> Вишня
    show(s, "ВИШ");        // регистронезависимо (кириллица) -> Вишня
    show(s, "Фрукты");     // категория Фрукты -> Вишня (Черешня нет среди событий)

    std::cout << "\n-- перезагрузка (replaceCatalog сохранён) --\n";
    Store s2(fs::path("/tmp/hacat/.data/home-accounting"));
    s2.load();
    show(s2, "Продукты");
    return 0;
}
