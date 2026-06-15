#include "Store.h"
#include "Paths.h"
#include "Jsonl.h"
#include "../sync/Crypto.h"

#include <boost/json.hpp>
#include <ctime>
#include <cmath>
#include <algorithm>
#include <sstream>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace ha {

// JSON-значение из std::string (избегаем двойной пользовательской конверсии).
static json::value jv(const std::string& s) { return json::value(json::string_view(s)); }

// ---- утилиты времени ----
static std::string nowStamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

// Год/месяц из строки "YYYY-MM-...". Возвращает {year, month}.
static std::pair<int,int> ymOf(const std::string& dt) {
    int y = 0, m = 1;
    if (dt.size() >= 7) { y = std::atoi(dt.substr(0,4).c_str()); m = std::atoi(dt.substr(5,2).c_str()); }
    return {y, m};
}

// ---- сериализация события ----
static std::string costToStr(double c) {
    double r = std::llround(c * 100.0) / 100.0;
    if (std::fabs(r - std::llround(r)) < 1e-9) {
        return std::to_string((long long)std::llround(r));
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", r);
    return buf;
}

static std::string eventToLine(const Event& e) {
    std::ostringstream os;
    os << '[' << json::serialize(jv(e.event_datetime)) << ','
       << json::serialize(jv(e.subject)) << ','
       << costToStr(e.cost) << ','
       << json::serialize(jv(e.edit_datetime)) << ','
       << e.rec_no << ',' << e.dev_no;
    // people, volume — последние null не пишем.
    bool hasVol = e.volume && !e.volume->empty();
    bool hasPpl = e.people && !e.people->empty();
    if (hasPpl || hasVol) {
        os << ',' << (hasPpl ? json::serialize(jv(*e.people)) : std::string("null"));
        if (hasVol) os << ',' << json::serialize(jv(*e.volume));
    }
    os << ']';
    return os.str();
}

static std::string deleteToLine(const std::string& edit, int rn, int dn) {
    json::object o;
    json::array a;
    a.emplace_back(edit); a.emplace_back(rn); a.emplace_back(dn);
    o["delete"] = std::move(a);
    return json::serialize(o);
}

// Транслировать DN строки из пространства отправителя в наше (по карте dnMap).
// Возвращает каноническую строку в нашем формате; "" — для заголовка.
static std::string translateLine(const std::string& line, const std::map<int,int>& dnMap) {
    auto mapDn = [&](int dn) {
        auto it = dnMap.find(dn);
        return it != dnMap.end() ? it->second : dn;
    };
    try {
        json::value v = json::parse(line);
        if (v.is_object()) {
            auto& o = v.as_object();
            if (auto* del = o.if_contains("delete")) {
                auto& a = del->as_array();
                std::string edit = a[0].is_string() ? std::string(a[0].as_string()) : "";
                int rn = (int)a[1].as_int64();
                int dn = (int)a[2].as_int64();
                return deleteToLine(edit, rn, mapDn(dn));
            }
            if (o.if_contains("header")) return "";
            return line;
        }
        if (v.is_array()) {
            auto& a = v.as_array();
            if (a.size() >= 6) {
                Event e;
                e.event_datetime = std::string(a[0].as_string());
                e.subject        = std::string(a[1].as_string());
                e.cost           = a[2].is_double() ? a[2].as_double() : (double)a[2].as_int64();
                e.edit_datetime  = std::string(a[3].as_string());
                e.rec_no         = (int)a[4].as_int64();
                e.dev_no         = mapDn((int)a[5].as_int64());
                if (a.size() > 6 && a[6].is_string()) e.people = std::string(a[6].as_string());
                if (a.size() > 7 && a[7].is_string()) e.volume = std::string(a[7].as_string());
                return eventToLine(e);
            }
        }
    } catch (...) {}
    return line;
}

// ---- Store ----
Store::Store(fs::path root) : root_(root.empty() ? filesDir() : std::move(root)) {}

// Токен дедупликации по уже разобранному JSON-значению.
static std::string tokenForValue(const json::value& v) {
    if (v.is_object()) {
        auto& o = v.as_object();
        if (auto* d = o.if_contains("delete")) return "D|" + json::serialize(d->as_array());
        if (o.if_contains("header")) return "";          // заголовок — не запись
        return "X|" + json::serialize(v);
    }
    if (v.is_array()) {
        auto& a = v.as_array();
        if (a.size() >= 6) {
            std::string edit = a[3].is_string() ? std::string(a[3].as_string()) : "";
            long rn = a[4].is_int64() ? a[4].as_int64() : 0;
            long dn = a[5].is_int64() ? a[5].as_int64() : 0;
            return "E|" + edit + "|" + std::to_string(rn) + "|" + std::to_string(dn);
        }
    }
    return "X|" + json::serialize(v);
}

std::string Store::tokenForLine(const std::string& line) {
    try { return tokenForValue(json::parse(line)); }
    catch (...) { return "X|" + line; }
}

void Store::load() {
    loadConfig();
    loadDevices();
    loadPeople();
    loadCatalog();
    loadEvents();
    rebuildState();
}

void Store::loadConfig() {
    // database.jsonl — список баз (первое значение = текущая активная).
    std::vector<std::string> dbs;
    readValues(root_ / "database.jsonl", [&](const json::value& v, std::string_view){
        if (v.is_string()) dbs.push_back(std::string(v.as_string()));
    });
    // config.json — единственный объект
    readValues(root_ / "config.json", [&](const json::value& v, std::string_view){
        if (!v.is_object()) return;
        auto& o = v.as_object();
        if (auto* d = o.if_contains("current_database")) db_ = std::string(d->as_string());
        if (auto* n = o.if_contains("current_device_no")) deviceNo_ = (int)n->as_int64();
        if (auto* f = o.if_contains("font_size")) fontSize_ = (int)f->as_int64();
    });
    if (!dbs.empty() && std::find(dbs.begin(), dbs.end(), db_) == dbs.end()) {
        // config указывает на базу, которой нет в списке — берём первую из списка.
        db_ = dbs.front();
    }
    if (dbs.empty()) {
        // первый запуск — заводим текущую базу
        writeAtomic(root_ / "database.jsonl", json::serialize(jv(db_)) + "\n");
    }
}

void Store::saveConfig() {
    json::object o;
    o["current_database"] = db_;
    o["current_device_no"] = deviceNo_;
    if (fontSize_ > 0) o["font_size"] = fontSize_;
    writeAtomic(root_ / "config.json", json::serialize(o) + "\n");
}

void Store::setFontSize(int pt) {
    fontSize_ = pt;
    saveConfig();
}

std::vector<std::string> Store::databases() const {
    std::vector<std::string> dbs;
    readValues(root_ / "database.jsonl", [&](const json::value& v, std::string_view){
        if (v.is_string()) dbs.push_back(std::string(v.as_string()));
    });
    if (dbs.empty()) dbs.push_back(db_);
    return dbs;
}

void Store::switchDatabase(const std::string& name, bool create) {
    auto dbs = databases();
    bool exists = std::find(dbs.begin(), dbs.end(), name) != dbs.end();
    if (!exists) {
        if (!create) return;
        dbs.push_back(name);
        std::string content;
        for (auto& d : dbs) content += json::serialize(jv(d)) + "\n";
        writeAtomic(root_ / "database.jsonl", content);
    }
    db_ = name;
    deviceNo_ = 0;                  // переопределится ensureIdentity для новой базы
    people_.clear(); catalog_.clear(); devices_.clear();
    rawEvents_.clear(); tokens_.clear(); live_.clear(); deleted_.clear();
    saveConfig();
    loadDevices(); loadPeople(); loadCatalog(); loadEvents();
    ensureIdentity();
    rebuildState();
}

void Store::loadDevices() {
    devices_.clear();
    readValues(dbDir() / "device.jsonl", [&](const json::value& v, std::string_view){
        try {
            auto& a = v.as_array();
            Device d;
            d.no = (int)a[0].as_int64();
            d.pubkey = std::string(a[1].as_string());
            if (a.size() > 2 && a[2].is_string()) d.name = std::string(a[2].as_string());
            devices_.push_back(std::move(d));
        } catch (...) {}
    });
}

void Store::saveDevices() {
    std::string content;
    for (auto& d : devices_) {
        json::array a;
        a.emplace_back(d.no);
        a.emplace_back(d.pubkey);
        if (!d.name.empty()) a.emplace_back(d.name);
        content += json::serialize(a) + "\n";
    }
    writeAtomic(dbDir() / "device.jsonl", content);
}

void Store::loadPeople() {
    people_.clear();
    readValues(dbDir() / "people.jsonl", [&](const json::value& v, std::string_view){
        if (v.is_string()) people_.push_back(std::string(v.as_string()));
    });
}

void Store::savePeople() {
    std::string content;
    for (auto& p : people_) content += json::serialize(jv(p)) + "\n";
    writeAtomic(dbDir() / "people.jsonl", content);
}

void Store::loadCatalog() {
    catalog_.clear();
    readValues(dbDir() / "catalog.jsonl", [&](const json::value& v, std::string_view){
        try {
            auto& a = v.as_array();
            if (a.empty()) return;
            CatalogEntry e;
            e.category = std::string(a[0].as_string());
            for (size_t i = 1; i < a.size(); ++i) e.items.push_back(std::string(a[i].as_string()));
            catalog_.push_back(std::move(e));
        } catch (...) {}
    });
}

void Store::saveCatalog() {
    std::string content;
    for (auto& e : catalog_) {
        json::array a;
        a.emplace_back(e.category);
        for (auto& it : e.items) a.emplace_back(it);
        content += json::serialize(a) + "\n";
    }
    writeAtomic(dbDir() / "catalog.jsonl", content);
}

void Store::loadEvents() {
    rawEvents_.clear();
    tokens_.clear();
    fs::path base = dbDir();
    if (!fs::exists(base)) return;
    // Перебираем декадные папки -> месячные файлы.
    for (auto& decade : fs::directory_iterator(base)) {
        if (!decade.is_directory()) continue;
        std::string dn = decade.path().filename().string();
        // декада — это число (напр. "2020"); пропускаем sync и пр.
        if (dn.empty() || !std::all_of(dn.begin(), dn.end(), ::isdigit)) continue;
        for (auto& f : fs::directory_iterator(decade.path())) {
            if (!f.is_regular_file()) continue;
            if (f.path().extension() != ".jsonl") continue;
            std::string month = f.path().stem().string();    // "2606"
            readValues(f.path(), [&](const json::value& v, std::string_view raw){
                std::string tok = tokenForValue(v);
                if (tok.empty()) return;                      // заголовок
                rawEvents_.emplace_back(month, std::string(raw));
                tokens_.insert(tok);
            });
        }
    }
}

void Store::rebuildState() {
    live_.clear();
    deleted_.clear();
    // Сначала собрать удаления, затем записи.
    for (auto& [mf, line] : rawEvents_) {
        (void)mf;
        try {
            auto v = json::parse(line);
            if (v.is_object()) {
                if (auto* d = v.as_object().if_contains("delete")) {
                    auto& a = d->as_array();
                    std::string edit = a[0].is_string()? std::string(a[0].as_string()):"";
                    long rn = a[1].as_int64(); long dn = a[2].as_int64();
                    deleted_.insert(edit + "|" + std::to_string(rn) + "|" + std::to_string(dn));
                }
            }
        } catch (...) {}
    }
    for (auto& [mf, line] : rawEvents_) {
        (void)mf;
        try {
            auto v = json::parse(line);
            if (!v.is_array()) continue;
            auto& a = v.as_array();
            if (a.size() < 6) continue;
            Event e;
            e.event_datetime = std::string(a[0].as_string());
            e.subject        = std::string(a[1].as_string());
            e.cost           = a[2].is_double()? a[2].as_double() : (double)a[2].as_int64();
            e.edit_datetime  = std::string(a[3].as_string());
            e.rec_no         = (int)a[4].as_int64();
            e.dev_no         = (int)a[5].as_int64();
            if (a.size() > 6 && a[6].is_string()) e.people = std::string(a[6].as_string());
            if (a.size() > 7 && a[7].is_string()) e.volume = std::string(a[7].as_string());
            if (deleted_.count(e.key())) continue;
            live_[e.key()] = std::move(e);
        } catch (...) {}
    }
}

std::vector<Event> Store::events() const {
    std::vector<Event> out;
    out.reserve(live_.size());
    for (auto& [k, e] : live_) out.push_back(e);
    std::sort(out.begin(), out.end(), [](const Event& a, const Event& b){
        return a.event_datetime > b.event_datetime;   // свежие сверху
    });
    return out;
}

std::string Store::categoryOf(const std::string& subject) const {
    for (auto& e : catalog_)
        for (auto& it : e.items)
            if (it == subject) return e.category;
    return {};
}

int Store::nextRecNo(const std::string& edit_datetime) const {
    int rn = 0;
    for (auto& [mf, line] : rawEvents_) {
        (void)mf;
        try {
            auto v = json::parse(line);
            if (v.is_array()) {
                auto& a = v.as_array();
                if (a.size() >= 6 && a[3].is_string() &&
                    std::string(a[3].as_string()) == edit_datetime &&
                    (int)a[5].as_int64() == deviceNo_) {
                    rn = std::max(rn, (int)a[4].as_int64() + 1);
                }
            }
        } catch (...) {}
    }
    return rn;
}

void Store::appendEventLine(const std::string& monthFileName, const std::string& line) {
    // путь: dbDir / decade / monthFileName.jsonl ; декада из первых 2 цифр года.
    int yy = std::atoi(monthFileName.substr(0,2).c_str());
    int year = 2000 + yy;            // допущение: 21 век
    fs::path p = dbDir() / decadeFolder(year) / (monthFileName + ".jsonl");
    bool fresh = !fs::exists(p);
    if (fresh) {
        // записать заголовок при создании файла
        json::object h;
        json::array cols{"event_datetime","subject","cost","edit_datetime",
                         "rec_no","dev_no","people","volume"};
        h["header"] = std::move(cols);
        appendLine(p, json::serialize(h));
    }
    appendLine(p, line);
    rawEvents_.emplace_back(monthFileName, line);
    tokens_.insert(tokenForLine(line));
}

Event Store::addEvent(const std::string& event_datetime, const std::string& subject,
                      double cost, std::optional<std::string> people,
                      std::optional<std::string> volume) {
    Event e;
    e.event_datetime = event_datetime;
    e.subject = subject;
    e.cost = cost;
    e.edit_datetime = nowStamp();
    e.dev_no = deviceNo_;
    e.rec_no = nextRecNo(e.edit_datetime);
    e.people = std::move(people);
    e.volume = std::move(volume);
    auto [y, m] = ymOf(e.edit_datetime);
    appendEventLine(monthFile(y, m), eventToLine(e));
    live_[e.key()] = e;
    return e;
}

void Store::deleteEvent(const Event& e) {
    std::string line = deleteToLine(e.edit_datetime, e.rec_no, e.dev_no);
    auto now = nowStamp();
    auto [y, m] = ymOf(now);
    appendEventLine(monthFile(y, m), line);
    deleted_.insert(e.key());
    live_.erase(e.key());
}

Event Store::editEvent(const Event& oldEv, const std::string& event_datetime,
                       const std::string& subject, double cost,
                       std::optional<std::string> people, std::optional<std::string> volume) {
    deleteEvent(oldEv);
    return addEvent(event_datetime, subject, cost, std::move(people), std::move(volume));
}

void Store::addPerson(const std::string& name) {
    if (name.empty()) return;
    if (std::find(people_.begin(), people_.end(), name) != people_.end()) return;
    people_.push_back(name);
    savePeople();
}

void Store::removePerson(const std::string& name) {
    auto it = std::find(people_.begin(), people_.end(), name);
    if (it != people_.end()) { people_.erase(it); savePeople(); }
}

void Store::upsertCatalog(const CatalogEntry& e) {
    for (auto& c : catalog_) {
        if (c.category == e.category) {
            for (auto& it : e.items)
                if (std::find(c.items.begin(), c.items.end(), it) == c.items.end())
                    c.items.push_back(it);
            saveCatalog();
            return;
        }
    }
    catalog_.push_back(e);
    saveCatalog();
}

void Store::replaceCatalog(const std::vector<CatalogEntry>& list) {
    catalog_ = list;
    saveCatalog();
}

// UTF-8 нижний регистр для ASCII и кириллицы (А-Я, Ё).
static std::string utf8Lower(const std::string& s) {
    std::string out; out.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { out += (char)std::tolower(c); ++i; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
            unsigned cp = ((c & 0x1F) << 6) | ((unsigned char)s[i+1] & 0x3F);
            if (cp >= 0x0410 && cp <= 0x042F) cp += 0x20;   // А-Я -> а-я
            else if (cp == 0x0401) cp = 0x0451;             // Ё -> ё
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
            i += 2;
        } else { out += (char)c; ++i; }
    }
    return out;
}

static bool icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    return utf8Lower(hay).find(utf8Lower(needle)) != std::string::npos;
}

std::vector<Event> Store::filter(const std::string& q) const {
    auto all = events();
    if (q.empty()) return all;

    // категории, чьё имя совпало с q -> их наименования (с раскрытием вложенных)
    std::set<std::string> memberSet;
    for (auto& c : catalog_)
        if (icontains(c.category, q)) {
            auto m = categoryMembers(c.category);
            memberSet.insert(m.begin(), m.end());
        }

    std::vector<Event> out;
    for (auto& e : all) {
        bool bySubject = icontains(e.subject, q);
        bool byPerson  = e.people && icontains(*e.people, q);
        bool byCategory = memberSet.count(e.subject) > 0;
        if (bySubject || byPerson || byCategory) out.push_back(e);
    }
    return out;
}

std::set<std::string> Store::categoryMembers(const std::string& category) const {
    std::set<std::string> result, visited;
    std::vector<std::string> stack{category};
    while (!stack.empty()) {
        std::string cat = stack.back(); stack.pop_back();
        if (visited.count(cat)) continue;
        visited.insert(cat);
        for (auto& e : catalog_) {
            if (e.category != cat) continue;
            for (auto& it : e.items) {
                result.insert(it);
                // если элемент сам является категорией — раскрыть вложенно
                for (auto& e2 : catalog_)
                    if (e2.category == it) { stack.push_back(it); break; }
            }
        }
    }
    return result;
}

// ---- идентичность ----
fs::path Store::certPath() const { return root_ / "identity" / "cert.pem"; }
fs::path Store::keyPath()  const { return root_ / "identity" / "key.pem"; }

void Store::ensureIdentity() {
    if (!fs::exists(certPath()) || !fs::exists(keyPath())) {
        fs::create_directories(root_ / "identity");
        crypto::generateSelfSigned("DomUchet-Device", keyPath().string(), certPath().string());
    }
    myPubkey_ = crypto::publicKeyFromCert(certPath().string());

    // зарегистрировать себя в device.jsonl текущей базы
    int maxNo = 0;
    for (auto& d : devices_)
	if (d.pubkey == myPubkey_) {
	    if (deviceNo_ != d.no) {
		deviceNo_ = d.no;
		saveConfig();
	    }
	    return;
	}
	else {
	    if (d.no == deviceNo_) deviceNo_ = 0;
	    if (d.no > maxNo) maxNo = d.no;
	}
    if (deviceNo_ <= 0) deviceNo_ = maxNo + 1;
    Device self{deviceNo_, myPubkey_, "this"};
    devices_.push_back(self);
    saveDevices();
    saveConfig();
}

bool Store::knowsDevice(const std::string& pubkey) const {
    for (auto& d : devices_) if (d.pubkey == pubkey) return true;
    return false;
}

int Store::reserveDeviceNo(const std::string& pubkey, int preferredNo, const std::string& name) {
    for (auto& d : devices_) if (d.pubkey == pubkey) return d.no;
    int maxNo = 0; bool taken = false;
    for (auto& d : devices_) { maxNo = std::max(maxNo, d.no); if (d.no == preferredNo) taken = true; }
    int no = (preferredNo > 0 && !taken) ? preferredNo : maxNo + 1;
    devices_.push_back(Device{no, pubkey, name});
    saveDevices();
    return no;
}

bool Store::hasData() const {
    if (!rawEvents_.empty()) return true;          // события (любые строки)
    if (!people_.empty()) return true;             // люди
    if (!catalog_.empty()) return true;            // каталог
    for (auto& d : devices_)                       // устройства кроме текущего
        if (d.pubkey != myPubkey_) return true;
    return false;                                  // список баз не учитывается
}

int Store::maxDeviceNo() const {
    int m = 0;
    for (auto& d : devices_) m = std::max(m, d.no);
    return m;
}

void Store::renumberSelf(int newNo) {
    for (auto& d : devices_) if (d.pubkey == myPubkey_) d.no = newNo;
    deviceNo_ = newNo;
    saveDevices();
    saveConfig();
}

// ---- синхронизация ----
SyncDump Store::dump() const {
    SyncDump d;
    d.db = db_;
    d.people = people_;
    d.catalog = catalog_;
    d.devices = devices_;
    d.events = rawEvents_;
    return d;
}

int Store::mergeDump(const SyncDump& d, std::set<std::string>* addedDeviceKeys) {
    // люди
    bool peopleChanged = false;
    for (auto& p : d.people)
        if (std::find(people_.begin(), people_.end(), p) == people_.end()) {
            people_.push_back(p); peopleChanged = true;
        }
    if (peopleChanged) savePeople();

    // каталог (объединение по категории)
    for (auto& e : d.catalog) upsertCatalog(e);

    // устройства: строим карту DN из пространства отправителя в наше (по pubkey).
    std::map<int,int> dnMap;
    for (auto& dev : d.devices) {
        int localNo = -1;
        for (auto& my : devices_) if (my.pubkey == dev.pubkey) { localNo = my.no; break; }
        if (localNo < 0) {
            localNo = reserveDeviceNo(dev.pubkey, dev.no, dev.name);  // сохраняем номер, если свободен
            if (addedDeviceKeys) addedDeviceKeys->insert(dev.pubkey);
        }
        dnMap[dev.no] = localNo;
    }

    // события: транслируем DN в наше пространство, затем дозаписываем только новые.
    int added = 0;
    for (auto& [mf, line] : d.events) {
        std::string tline = translateLine(line, dnMap);
        if (tline.empty()) continue;                 // заголовок
        std::string tok = tokenForLine(tline);
        if (tok.empty() || tokens_.count(tok)) continue;
        appendEventLine(mf, tline);
        ++added;
    }
    if (added) rebuildState();
    return added;
}

} // namespace ha
