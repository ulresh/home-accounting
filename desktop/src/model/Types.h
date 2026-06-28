#pragma once
#include <string>
#include <set>
#include <optional>
#include <cstdint>

namespace ha {

struct RecRef { std::string edit_datetime; int rec_no = 0; int dev_no = 0; };

// Одно событие (трата/покупка). Идентичность записи в системе:
// (edit_datetime, rec_no, dev_no). Эти три поля неизменны.
struct Event {
    std::string event_datetime;        // "YYYY-MM-DD" или "YYYY-MM-DD HH:MM"
    std::string subject;               // наименование (из каталога или произвольное)
    double      cost = 0.0;            // стоимость (может быть с копейками)
    std::string edit_datetime;         // "YYYY-MM-DD HH:MM:SS" — момент записи
    int         rec_no = 0;            // RN — добавочный номер в пределах секунды/устройства
    int         dev_no = 0;            // DN — номер устройства-автора
    std::string people;  // имя человека или пусто
    std::string volume;  // объём/количество, напр. "2 кг"
    std::string comment; // произвольный комментарий
    bool compare_delete(const RecRef &r) const {
	return edit_datetime == r.edit_datetime && rec_no == r.rec_no &&
	    dev_no == r.dev_no;
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
    std::set<std::string> items;
};

} // namespace ha
