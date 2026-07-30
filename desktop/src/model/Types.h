#pragma once
#include <string>
#include <set>
#include <map>
#include <optional>
#include <cstdint>
#include "../shorts.h"

namespace ha {

inline int jsonAsDevNo(const json::value &v) {
    // boost::json разбирает неотрицательное целое как int64
    // uint64 — только если не влезает в int64
    auto n = v.as_int64();
    if(n <= 0 || n > std::numeric_limits<int>::max())
	throw std::runtime_error("bad DN value"s);
    return n;
}

struct RecRef {
    template<typename A, typename B>
    static bool less(const A &a, const B &rhs) {
	return a.edit_datetime < rhs.edit_datetime ||
	    (a.edit_datetime == rhs.edit_datetime &&
	     (a.rec_no < rhs.rec_no || (a.rec_no == rhs.rec_no &&
		a.dev_no < rhs.dev_no)));
    }
    bool operator < (const RecRef &rhs) const {
	return less(*this, rhs);
    }
    bool operator == (const RecRef &rhs) const {
	return edit_datetime == rhs.edit_datetime &&
	    rec_no == rhs.rec_no && dev_no == rhs.dev_no;
    }
    std::string edit_datetime; int rec_no = 0; int dev_no = 0;
};
struct RecRefDel : RecRef {
    std::string event_datetime;
};

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
    bool eq_edit(const Event &r) const {
	return edit_datetime == r.edit_datetime && rec_no == r.rec_no &&
	    dev_no == r.dev_no;
    }
    bool eq_data(const Event &r) const {
	return subject == r.subject && cost == r.cost &&
	    people == r.people && volume == r.volume && comment == r.comment;
    }
};

// Устройство сети: [DN, "<публичный ключ>"].
struct Device {
    Device(int no, std::string_view pubkey, std::string_view name = {})
	: no(no), pubkey(pubkey), name(name)
    {}
    Device(const json::value &v, bool add_name = true) {
	auto& a = v.as_array();
	no = jsonAsDevNo(a[0]);
	pubkey = std::string(a[1].as_string());
	if(add_name && a.size() > 2 && a[2].is_string())
	    name = std::string(a[2].as_string());
    }
    int         no = 0; // DN — порядковый номер
    std::string pubkey; // полный публичный ключ (PEM SPKI, base64 одной строкой)
    std::string name;   // отмета this
};

// Строка каталога: первый элемент — категория, остальные — позиции.
struct CatalogEntry {
    std::string category;
    std::set<std::string> items;
};

typedef std::map<std::string, std::string> CategoryMap;
struct CategoryItems {
    std::string addtime;
    CategoryMap items, deleted;
};

} // namespace ha
