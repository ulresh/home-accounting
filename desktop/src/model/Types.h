#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace ha {

// Одно событие (трата/покупка). Идентичность записи в системе:
// (edit_datetime, rec_no, dev_no). Эти три поля неизменны.
struct Event {
    std::string event_datetime;        // "YYYY-MM-DD" или "YYYY-MM-DD HH:MM"
    std::string subject;               // наименование (из каталога или произвольное)
    double      cost = 0.0;            // стоимость (может быть с копейками)
    std::string edit_datetime;         // "YYYY-MM-DD HH:MM:SS" — момент записи
    int         rec_no = 0;            // RN — добавочный номер в пределах секунды/устройства
    int         dev_no = 0;            // DN — номер устройства-автора
    std::optional<std::string> people;  // имя человека или пусто
    std::optional<std::string> volume;  // объём/количество, напр. "2 кг"
    std::optional<std::string> comment; // произвольный комментарий

    // Глобальный ключ записи.
    std::string key() const {
        return edit_datetime + "|" + std::to_string(rec_no) + "|" + std::to_string(dev_no);
    }
};

// Устройство сети: [DN, "<публичный ключ>"].
struct Device {
    int         no = 0;        // DN — порядковый номер
    std::string pubkey;        // полный публичный ключ (PEM SPKI, base64 одной строкой)
    std::string name;          // отображаемое имя (DN из сертификата), необязательно
};

// Строка каталога: первый элемент — категория, остальные — позиции.
struct CatalogEntry {
    std::string category;
    std::vector<std::string> items;
};

} // namespace ha
