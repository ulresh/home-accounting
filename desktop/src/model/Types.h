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

// Устройство сети: [DN, "<публичный ключ>", "<имя>", NN, "disabled"].
// Имя задаёт владелец устройства; NN — сколько раз это имя менялось (0 при
// создании, +1 на каждое изменение): по нему разрешается гонка, когда одно и
// то же имя пришло с разных сторон. Дальше необязательные строковые признаки;
// сейчас единственный — "disabled": с этим устройством нельзя проводить
// прямую синхронизацию.
struct Device {
    Device(int no, std::string_view pubkey, std::string_view name = {},
	   int nn = 0, bool disabled = false)
	: no(no), pubkey(pubkey), name(name), nn(nn), disabled(disabled)
    {}
    Device(const json::value &v) {
	auto& a = v.as_array();
	no = jsonAsDevNo(a[0]);
	pubkey = std::string(a[1].as_string());
	if(a.size() > 2 && a[2].is_string()) name = std::string(a[2].as_string());
	// boost::json кладёт неотрицательное целое в int64 (см. памятку §7)
	if(a.size() > 3 && a[3].is_int64()) nn = (int)a[3].as_int64();
	for(std::size_t i = 4; i < a.size(); ++i)
	    if(a[i].is_string() && a[i].as_string() == "disabled"sv)
		disabled = true;
    }
    int         no = 0; // DN — порядковый номер
    std::string pubkey; // полный публичный ключ (PEM SPKI, base64 одной строкой)
    std::string name;   // имя устройства (для показа пользователю)
    int         nn = 0; // сколько раз имя менялось
    bool disabled = false;  // прямая синхронизация с ним запрещена
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
