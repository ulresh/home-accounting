#include "Store.h"
#include "Paths.h"
#include "Jsonl.h"
#include "../sync/Crypto.h"

#include <openssl/evp.h>
#include <ctime>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <list>
#include <iterator>
#include <vector>

namespace ha {

// JSON-значение из std::string (избегаем двойной пользовательской конверсии).
static json::value jv(const std::string& s) { return json::value(json::string_view(s)); }

// ---- утилиты времени/чисел ----
static std::string nowStamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

static int yyyymmOf(const std::string& dt) {
    int y = 0, m = 1;
    if (dt.size() >= 7) { y = std::atoi(dt.substr(0,4).c_str()); m = std::atoi(dt.substr(5,2).c_str()); }
    return y * 100 + m;
}

static std::string asStr(const json::value& v) { return v.is_string() ? std::string(v.as_string()) : std::string(); }
static double asNum(const json::value& v) {
    if (v.is_double()) return v.as_double();
    if (v.is_int64())  return (double)v.as_int64();
    if (v.is_uint64()) return (double)v.as_uint64();
    return 0.0;
}
static int asInt(const json::value& v) {
    if (v.is_int64())  return (int)v.as_int64();
    if (v.is_uint64()) return (int)v.as_uint64();
    if (v.is_double()) return (int)v.as_double();
    return 0;
}

static std::string costToStr(double c) {
    double r = std::llround(c * 100.0) / 100.0;
    if (std::fabs(r - std::llround(r)) < 1e-9) return std::to_string((long long)std::llround(r));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", r);
    return buf;
}

static std::string toHex(const unsigned char* md, unsigned n) {
    static const char* H = "0123456789abcdef";
    std::string out; out.reserve(n * 2);
    for (unsigned i = 0; i < n; ++i) { out += H[md[i] >> 4]; out += H[md[i] & 0xF]; }
    return out;
}

// ---- файловые помощники (потоковые: файл целиком в память не грузим) ----
// Размер и SHA1 файла, считая его блоками фиксированного размера.
FileState Store::stateOf(const fs::path& p) {
    FileState st;
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return st;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    std::vector<char> block(64 * 1024);
    uint64_t total = 0; bool any = false;
    while (in) {
        in.read(block.data(), (std::streamsize)block.size());
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        EVP_DigestUpdate(ctx, block.data(), (size_t)got);
        total += got; any = true;
    }
    st.size = total;
    if (any) {
        unsigned char md[EVP_MAX_MD_SIZE]; unsigned int n = 0;
        EVP_DigestFinal_ex(ctx, md, &n);
        st.sha1 = toHex(md, n);
    }
    EVP_MD_CTX_free(ctx);
    return st;
}

// ---- схемы ----
static Schema canonicalSchema() { // TODO +++ constexpr?
    return Schema{
        {"event_datetime","subject","cost","edit_datetime","rec_no","dev_no","people","volume","comment"},
        {"edit_datetime","rec_no","dev_no"}};
}
inline json::object headerObjectFor(const Schema& s) {
    json::object o;
    json::array cols, ref;
    for (auto& c : s.columns) cols.emplace_back(c);
    for (auto& r : s.reference) ref.emplace_back(r);
    o["header"] = std::move(cols);
    o["reference"] = std::move(ref);
    return o;
}
inline std::string headerLineFor(const Schema& s) {
    return json::serialize(headerObjectFor(s));
}
std::string Schema::serialize() const { return headerLineFor(*this); }
inline std::string canonicalHeaderLine() {
    return headerLineFor(canonicalSchema());
}
Schema::Schema(const json::object &o, bool add_defaults) {
    if (auto* h = o.if_contains("header"))
        for (auto& c : h->as_array())
	    if (c.is_string())
		columns.push_back(std::string(c.as_string()));
    if (auto* r = o.if_contains("reference"))
        for (auto& c : r->as_array())
	    if (c.is_string())
		reference.push_back(std::string(c.as_string()));
    if(add_defaults) {
	if (columns.empty())   columns = canonicalSchema().columns;
	if (reference.empty()) reference = canonicalSchema().reference;
    }
}
static Schema schemaFromHeader(const json::object& o) { return Schema(o); }
static Schema schemaFromHeaderNodefault(const json::object& o) {
    return Schema(o, false); }

static Event *parseEventArray(const json::array& a, const Schema& s) {
    Event* ep;
    std::unique_ptr<Event> eh(ep = new Event);
    for (size_t i = 0; i < s.columns.size() && i < a.size(); ++i) {
        const std::string& c = s.columns[i];
        const json::value& v = a[i];
        if      (c == "event_datetime") ep->event_datetime = asStr(v);
        else if (c == "subject")        ep->subject = asStr(v);
        else if (c == "cost")           ep->cost = asNum(v);
        else if (c == "edit_datetime")  ep->edit_datetime = asStr(v);
        else if (c == "rec_no")         ep->rec_no = asInt(v);
        else if (c == "dev_no")         ep->dev_no = asInt(v);
        else if (c == "people")  ep->people  = asStr(v);
        else if (c == "volume")  ep->volume  = asStr(v);
        else if (c == "comment") ep->comment = asStr(v);
    }
    return eh.release();
}

static RecRef parseRef(const json::array& a, const std::vector<std::string>& ref) {
    RecRef r;
    for (size_t i = 0; i < ref.size() && i < a.size(); ++i) {
        if      (ref[i] == "edit_datetime") r.edit_datetime = asStr(a[i]);
        else if (ref[i] == "rec_no")        r.rec_no = asInt(a[i]);
        else if (ref[i] == "dev_no")        r.dev_no = asInt(a[i]);
    }
    return r;
}
static int refIndexOfDn(const std::vector<std::string>& ref) {
    for (size_t i = 0; i < ref.size(); ++i) if (ref[i] == "dev_no") return (int)i;
    return -1;
}

// каноническая сериализация НАШЕГО события (с обрезкой хвостовых null).
static std::string eventToLine(const Event& e) {
    std::ostringstream os;
    os << '[' << json::serialize(jv(e.event_datetime)) << ','
       << json::serialize(jv(e.subject)) << ','
       << costToStr(e.cost) << ','
       << json::serialize(jv(e.edit_datetime)) << ','
       << e.rec_no << ',' << e.dev_no;
    bool hasC = !e.comment.empty();
    bool hasV = !e.volume.empty();
    bool hasP = !e.people.empty();
    if (hasP || hasV || hasC) {
        os << ',' << (hasP ? json::serialize(jv(e.people)) : std::string("null"));
        if (hasV || hasC) {
            os << ',' << (hasV ? json::serialize(jv(e.volume)) : std::string("null"));
            if (hasC) os << ',' << json::serialize(jv(e.comment));
        }
    }
    os << ']';
    return os.str();
}

// ---- Store ----
Store::Store(fs::path root) : root_(root.empty() ? filesDir() : std::move(root)) {}

fs::path Store::monthPath(int yyyymm) const {
    int year = yyyymm / 100, month = yyyymm % 100;
    return dbDir() / decadeFolder(year) / (monthFile(year, month) + ".jsonl");
}
fs::path Store::syncIndexPath(int peerDn) const {
    return dbDir() / "sync" / (std::to_string(peerDn) + ".jsonl");
}

// перечислить все месячные файлы базы (yyyymm, путь), по возрастанию.
std::vector<std::pair<int,fs::path>> Store::enumerateMonths() const {
    std::vector<std::pair<int,fs::path>> out;
    auto dbDir = this->dbDir();
    if (!fs::exists(dbDir)) return out;
    for (auto& decade : fs::directory_iterator(dbDir)) {
        if (!decade.is_directory()) continue;
        std::string dn = decade.path().filename().string();
        if (dn.empty() || dn.size() != 4 || !std::all_of(dn.begin(), dn.end(), ::isdigit)) continue;  // не sync/identity
	int yy00 = (dn[0] - '0') * 1000 + (dn[1] - '0') * 100;
        for (auto& f : fs::directory_iterator(decade.path())) {
            if (!f.is_regular_file() || f.path().extension() != ".jsonl") continue;
            std::string stem = f.path().stem().string();   // "2606"
            if (stem.size() != 4 || !std::all_of(stem.begin(), stem.end(), ::isdigit)) continue;
            int yymm = std::atoi(stem.c_str());
            int yyyymm = (yy00 + yymm / 100) * 100 + (yymm % 100);
            out.emplace_back(yyyymm, f.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void Store::load() {
    loadConfig();
    loadDevices();
    loadPeople();
    loadCatalog();
    loadEvents();
}

void Store::loadConfig() {
    std::vector<std::string> dbs;
    readValues(root_ / "database.jsonl", [&](const json::value& v){
        if (v.is_string()) dbs.push_back(std::string(v.as_string()));
    });
    readValues(root_ / "config.json", [&](const json::value& v){
        if (!v.is_object()) return;
        auto& o = v.as_object();
        if (auto* d = o.if_contains("current_database")) db_ = std::string(d->as_string());
        if (auto* n = o.if_contains("current_device_no")) deviceNo_ = (int)n->as_int64();
        if (auto* f = o.if_contains("font_size")) fontSize_ = (int)f->as_int64();
    });
    if (dbs.empty())
	writeAtomic(root_ / "database.jsonl",
		    json::serialize(jv(db_)) + "\n");
    else if (std::find(dbs.begin(), dbs.end(), db_) == dbs.end())
	db_ = dbs.front();
}

void Store::saveConfig() {
    json::object o;
    o["current_database"] = db_;
    o["current_device_no"] = deviceNo_;
    if (fontSize_ > 0) o["font_size"] = fontSize_;
    writeAtomic(root_ / "config.json", json::serialize(o) + "\n");
}

void Store::setFontSize(int pt) { fontSize_ = pt; saveConfig(); }

std::vector<std::string> Store::databases() const {
    std::vector<std::string> dbs;
    readValues(root_ / "database.jsonl", [&](const json::value& v){
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
    deviceNo_ = 0;
    loadDevices();
    ensureIdentity(true);
    loadPeople(); loadCatalog(); loadEvents();
}

void Store::loadDevices() {
    devices_.clear();
    readValues(pDevice(), [&](const json::value& v){
        try { devices_.push_back(Device(v)); } catch (...) {}
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
    writeAtomic(pDevice(), content);
}

void Store::loadPeople() {
    people_.clear();
    people_delete.clear();
    People *p = &people_;
    readValues(dbDir() / "people.jsonl", [&](const json::value& v){
        if(v.is_object())
	    for(auto &[value,time] : v.as_object())
		if(time.is_string())
		    p->emplace_hint(p->end(), std::string(value),
				    std::string(time.as_string()));
	else if(v.is_array()) {
	    auto a = v.as_array();
	    if(a.size() == 1 && a[0].is_string() &&
	       a[0].as_string() == "delete"s)
		p = &people_delete;
	}
    });
}

void Store::savePeople() {
    std::string content;
    for (auto& p : people_) {
	// json::array a;
	// a.emplace_back(p.first);
	// a.emplace_back(p.second);
	json::object a;
	a[p.first] = p.second;
	content += json::serialize(a) + "\n";
    }
    if(!people_delete.empty()) {
	content += R"(["delete"])" "\n"sv;
	for (auto& p : people_delete) {
	    json::object a;
	    a[p.first] = p.second;
	    content += json::serialize(a) + "\n";
	}
    }
    writeAtomic(dbDir() / "people.jsonl", content);
}

void CatalogLoader::add(const json::value &v) {
        if(v.is_object()) {
	    for(auto &[jn,jt] : v.as_object()) if(jt.is_string()) {
		std::string sn(jn), st(jt.as_string());
		if(cur) cur->at(sn) = st;
		else {
		    auto &cat = catalog_[sn];
		    cat.addtime = st;
		    cur = &cat.items;
		    del = &cat.deleted;
		}
	    }
	}
	else if(v.is_array()) {
	    auto &a = v.as_array();
	    if(a.size() == 1 && a[0].is_string()) {
		std::string c(a[0].as_string());
		if(c == "end"s) cur = del = nullptr;
		else if(c == "delete"s) {
		    if(cur) cur = del;
		    else cur = &catalog_delete;
		}
	    }
	}
}

void CatalogIncrementLoader::add(const json::value &v) {
        if(v.is_object()) {
	    for(auto &[jn,jt] : v.as_object()) if(jt.is_string()) {
		std::string sn(jn), st(jt.as_string());
		switch(state) {
		case 0: {
		    auto del = store.catalog_delete.find(sn);
		    if(del == store.catalog_delete.end()) ;
		    else if(del->second > st) { state = 4; break; }
		    else store.catalog_delete.erase(del);
		    auto &act = store.catalog_[sn];
		    if(act.addtime < st) act.addtime = st;
		    state = 1;
		    current = &act;
		    break; }
		case 1: {
		    auto del = current->deleted.find(sn);
		    if(del == current->deleted.end()) ;
		    else if(del->second > st) break;
		    else current->deleted.erase(del);
		    auto &act = current->items[sn];
		    if(act < st) act = st;
		    break; }
		case 2: {
		    auto act = current->items.find(sn);
		    if(act == current->items.end()) ;
		    else if(act->second >= st) break;
		    else current->items.erase(act);
		    auto &del = current->deleted[sn];
		    if(del < st) del = st;
		    break; }
		case 3: {
		    auto act = store.catalog_.find(sn);
		    if(act == store.catalog_.end()) ;
		    else if(act->second.addtime >= st) break;
		    else store.catalog_.erase(act);
		    auto &del = store.catalog_delete[sn];
		    if(del < st) del = st;
		    break; }
		case 4:
		    break;
		}
	    }
	}
	else if(v.is_array()) {
	    auto &a = v.as_array();
	    if(a.size() == 1 && a[0].is_string()) {
		std::string c(a[0].as_string());
		if(c == "end"s) state = 0;
		else if(c == "delete"s) {
		    if(state == 1 || state == 2) state = 2;
		    else if(state == 4) ;
		    else state = 3;
		}
	    }
	}
}

void Store::loadCatalog() {
    CatalogLoader loader;
    readValues(dbDir() / "catalog.jsonl", [&loader](const json::value &v){
	loader.add(v);
    });
    catalog_.swap(loader.catalog_);
    catalog_delete.swap(loader.catalog_delete);
}
void Store::saveCatalog() {
    std::string content;
    for(auto &[category_name,items] : catalog_) {
	{   json::object c;
	    c[category_name] = items.addtime;
	    content += json::serialize(c) + "\n";
	}
	for(auto &[item_name,item_addtime] : items.items) {
	    json::object i;
	    i[item_name] = item_addtime;
	    content += json::serialize(i) + "\n";
	}
	if(!items.deleted.empty()) {
	    content += R"(["delete"])" "\n"s;
	    for(auto &[item_name,item_addtime] : items.deleted) {
		json::object i;
		i[item_name] = item_addtime;
		content += json::serialize(i) + "\n";
	    }
	}
	content += R"(["end"])" "\n"s;
    }
    if(!catalog_delete.empty()) {
	content += R"(["delete"])" "\n"s;
	for(auto &[item_name,item_addtime] : catalog_delete) {
	    json::object i;
	    i[item_name] = item_addtime;
	    content += json::serialize(i) + "\n";
	}
    }
    writeAtomic(dbDir() / "catalog.jsonl", content);
}

template<typename T>
void Store::read_last_edit(const T &d) {
    if(d.dev_no == deviceNo_) {
	if(d.edit_datetime > lastEdit_) {
	    lastEdit_ = d.edit_datetime;
	    lastEditSeq_ = d.rec_no;
	}
	else if(d.edit_datetime == lastEdit_ &&
		d.rec_no > lastEditSeq_)
	    lastEditSeq_ = d.rec_no;
    }
}

// ---- загрузка событий: по месяцам, удаления применяются на лету ----
void Store::loadEvents() {
    events_.clear(); canonicalSchemaMonths_.clear();
    lastEdit_.clear(); lastEditSeq_ = 0;
    for (auto& [yyyymm, path] : enumerateMonths()) {
	MonthEvents m(*this);
        readValues(path, [&m](const json::value& v){ m.add(v); });
	m.commit(yyyymm);
    }
}

namespace {
void applyDeleteFromLoad(Store::TempEvents monthEvents, const RecRef &r) {
    for(auto &&p : monthEvents)
	if(p->compare_delete(r)) {
	    // будет сортировка, поэтому порядок не важен
	    auto &b = monthEvents.back();
	    if(p.get() != b.get()) p = b;
	    monthEvents.resize(monthEvents.size() - 1);
	    break;
	}
}
}

void MonthEvents::add(const json::value &v) {
    if (v.is_object()) {
	auto& o = v.as_object();
	if (o.if_contains("header")) header = schemaFromHeader(o);
	else if (!header) ;
	else if (auto* del = o.if_contains("delete")) {
	    RecRef t = parseRef(del->as_array(), header.reference);
	    applyDeleteFromLoad(monthEvents, t);
	    if(auto *edit = o.if_contains("this"))
		store.read_last_edit(parseRef(edit->as_array(),
					header.reference));
	}
    }
    else if (!header) ;
    else if (v.is_array()) {
	Event *ep;
	monthEvents.emplace_back(ep = parseEventArray(
				v.as_array(), header));
	store.read_last_edit(*ep);
    }
}

void MonthEvents::commit(int yyyymm) {
    if(header == canonicalSchema())
	store.canonicalSchemaMonths_.insert(yyyymm);
    std::sort(monthEvents.begin(), monthEvents.end(),
	      compareEvents);
    for(auto &&p : monthEvents) store.events_.insert(store.events_.end(), p);
}

std::string Store::categoryOf(const std::string& subject) const {
    for (auto& e : catalog_)
	if(e.second.items.contains(subject)) return e.first;
    return {};
}

int Store::allocRecNo(const std::string &stamp, int yyyymm) {
    if(stamp > lastEdit_) {
	lastEdit_ = stamp;
	lastEditSeq_ = 0;
	return 0;
    }
    else if(stamp == lastEdit_) return ++lastEditSeq_;
    else {
	int seq = 0;
	char yyyy_mm_s[8];
	std::snprintf(yyyy_mm_s, sizeof(yyyy_mm_s), "%04d-%02d",
		      yyyymm / 100, yyyymm % 100);
	for(auto p = events_.lower_bound(yyyy_mm_s),
		e = events_.end(); p != e; ++p) {
	    auto v = **p;
	    if(v.event_datetime.size() < 7 ||
	       memcmp(yyyy_mm_s, v.event_datetime.data(), 7)) break;
	    if(v.dev_no == deviceNo_ && v.edit_datetime == stamp &&
	       v.rec_no >= seq)
		seq = v.rec_no + 1;
	}
	return seq;
    }
}

void Store::appendToMonth(int yyyymm, const std::string& line) {
    appendLine(monthPath(yyyymm), line);
}
void Store::ensureCanonicalHeader(int yyyymm) {
    if(!canonicalSchemaMonths_.contains(yyyymm)) {
        appendToMonth(yyyymm, canonicalHeaderLine());
	canonicalSchemaMonths_.insert(yyyymm);
    }
}

bool Store::writeDelete(const std::string& tgtEdit, int tgtRn, int tgtDn, bool update) {
    std::string stamp = nowStamp();
    int ym = yyyymmOf(tgtEdit);
    int rn = allocRecNo(stamp, ym);
    ensureCanonicalHeader(ym);
    json::object o;
    json::array del; del.emplace_back(jv(tgtEdit)); del.emplace_back(tgtRn); del.emplace_back(tgtDn);
    json::array ths; ths.emplace_back(jv(stamp)); ths.emplace_back(rn); ths.emplace_back(deviceNo_);
    o["delete"] = std::move(del);
    o["this"]   = std::move(ths);
    if (update) o["update"] = true;
    appendToMonth(ym, json::serialize(o));
    return true;
}

Event &Store::addEvent(const std::string& event_datetime,
		      const std::string& subject,
                      double cost, const std::string &people,
                      const std::string &volume,
		      const std::string &comment) {
    Event *ep;
    std::shared_ptr<Event> eh(ep = new Event);
    ep->event_datetime = event_datetime;
    int ym = yyyymmOf(ep->event_datetime);
    ep->subject = subject;
    ep->cost = cost;
    ep->edit_datetime = nowStamp();
    ep->dev_no = deviceNo_;
    ep->rec_no = allocRecNo(ep->edit_datetime, ym);
    ep->people = people;
    ep->volume = volume;
    ep->comment = comment;
    ensureCanonicalHeader(ym);
    appendToMonth(ym, eventToLine(*ep));
    events_.insert(eh);
    return *ep;
}

void Store::deleteEvent(const std::shared_ptr<Event> &e) {
    writeDelete(e->edit_datetime, e->rec_no, e->dev_no, false);
    events_.erase(e);
}

Event Store::editEvent(const std::shared_ptr<Event> &oldEv,
		       const std::string& event_datetime,
                       const std::string& subject, double cost,
                       const std::string &people, const std::string &volume,
                       const std::string &comment) {
    writeDelete(oldEv->edit_datetime, oldEv->rec_no, oldEv->dev_no, true);
    events_.erase(oldEv);
    return addEvent(event_datetime, subject, cost, people, volume, comment);
}

void Store::addPerson(const std::string& name) {
    if(name.empty()) return;
    people_delete.erase(name);
    people_[name] = nowStamp();
    savePeople();
}
void Store::removePerson(const std::string& name) {
    if(people_.erase(name)) {
	people_delete[name] = nowStamp();
	savePeople();
    }
}

void Store::upsertCatalog(const CatalogEntry &e) {
    // TODO +++ нам скорее всего будет нужен не столько upsert, сколько insert+replace
    auto now = nowStamp();
    catalog_delete.erase(e.category);
    auto &cat = catalog_[e.category];
    if(cat.addtime.empty()) cat.addtime = now;
    for(auto &item : e.items) {
	cat.deleted.erase(item);
	auto &t = cat.items[item];
	if(t.empty()) t = now;
    }
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
            if (cp >= 0x0410 && cp <= 0x042F) cp += 0x20;
            else if (cp == 0x0401) cp = 0x0451;
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

Store::TempEvents Store::filter(const std::string& q) const {
    TempEvents out;
    if (q.empty()) {
	out.reserve(events_.size());
	out.insert(out.end(), events_.begin(), events_.end());
    }
    else {
	std::set<std::string> memberSet;
	for (auto& c : catalog_)
	    if (icontains(c.first, q))
		categoryMembers(memberSet, c);
	for(auto &&e : events_)
	    if(icontains(e->subject, q) ||
	       icontains(e->people, q) ||
	       icontains(e->comment, q) ||
	       icontains(e->subject, q) ||
	       memberSet.contains(e->subject)) out.push_back(e);
    }
    return out;
}

void Store::categoryMembers(std::set<std::string> &result,
			    Catalog::const_reference category) const {
    std::list<const Catalog::mapped_type *> stack;
    stack.push_back(&category.second);
    while (!stack.empty()) {
        auto cat = stack.back(); stack.pop_back();
	for(auto &[item,ignore] : cat->items) {
	    auto [p,added] = result.insert(item);
	    if(added) {
		auto child = catalog_.find(item);
		if(child != catalog_.end())
		    stack.push_back(&child->second);
	    }
	}
    }
}

// ---- идентичность ----
fs::path Store::certPath() const { return root_ / "identity" / "cert.pem"; }
fs::path Store::keyPath()  const { return root_ / "identity" / "key.pem"; }

void Store::ensureIdentity(bool forceSaveConfig) {
    if (!fs::exists(certPath()) || !fs::exists(keyPath())) {
        fs::create_directories(root_ / "identity");
        crypto::generateSelfSigned("DomUchet-Device", keyPath().string(), certPath().string());
    }
    myPubkey_ = crypto::publicKeyFromCert(certPath().string());

    int maxNo = 0;
    for (auto& d : devices_)
        if (d.pubkey == myPubkey_) {
            if (deviceNo_ != d.no) { deviceNo_ = d.no; saveConfig(); }
	    else if(forceSaveConfig) saveConfig();
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

int Store::knowsDevice(const std::string& pubkey) const {
    for (auto& d : devices_) if (d.pubkey == pubkey) return d.no;
    return 0;
}
bool Store::hasData() const {
    if (!events_.empty()) return true;
    if (!people_.empty()) return true;
    if (!catalog_.empty()) return true;
    for (auto& d : devices_) if (d.pubkey != myPubkey_) return true;
    return false;
}
int Store::maxDeviceNo() const {
    int m = 0;
    for (auto& d : devices_) m = std::max(m, d.no);
    return m;
}

int Store::addDevice(std::string_view pubkey) {
    int m = 0;
    for(auto& d : devices_) {
	// могла быть незавершённая синхронизация, при которой мы добавили собеседника, но собеседник у себя синхронизацию не записал
	if(d.pubkey == pubkey) return d.no;
	if(m < d.no) m = d.no;
    }
    if(m == std::numeric_limits<int>::max())
	throw std::runtime_error("too big device no"s);
    devices_.push_back(Device{++m, std::string(pubkey)});
    // saveDevices();
    json::array a;
    a.emplace_back(m);
    a.emplace_back(pubkey);
    appendLine(pDevice(), json::serialize(a)); // TODO +++ аналогично сделать addPeople
    return m;
}

void Store::addDevice(std::unique_ptr<std::ofstream> &outp,
		      int no, const std::string &pubkey) {
    devices_.emplace_back(no, pubkey);
    auto op = outp.get();
    if(!op) outp.reset(op = new std::ofstream(pDevice(),
			std::ios::binary | std::ios::app));
    json::array a;
    a.emplace_back(no);
    a.emplace_back(pubkey);
    *op << json::serialize(a) << std::endl;
}

// =====================================================================
//                      Инкрементная синхронизация
// =====================================================================

void Store::listManifest(ListManifest &m) const {
    m.people  = stateOf(dbDir() / "people.jsonl");
    m.catalog = stateOf(dbDir() / "catalog.jsonl");
    m.device  = stateOf(pDevice());
}

// Индекс = состояние собеседника: [yyyymm, offset] — сколько байт нашего
// месячного файла у него уже есть.
void Store::loadSyncIndex(int peerDn, SyncIndex &idx) const {
    Schema header;
    readValues(syncIndexPath(peerDn), [&header,&idx](const json::value& v){
	if(v.is_object())
	    header = schemaFromHeaderNodefault(v.as_object());
	else if(!header) ;
        else if(v.is_array()) {
	    auto& a = v.as_array();
	    if(a.size() >= 3 && a[0].is_string() && a[1].is_uint64() &&
	       a[2].is_string()) {
		auto s = a[0].as_string();
		FileState *p;
		if(s == "device") p = &idx.device;
		else if(s == "device-map") {
		    for(int r = 1, l = 2; l < a.size(); ++r, ++l)
			if(a[r].is_uint64() && a[l].is_uint64())
			    idx.dnMap[a[r].as_uint64()] = a[l].as_uint64();
		    idx.empty = false;
		    return;
		}
		else if(s == "people") p = &idx.people;
		else if(s == "catalog") p = &idx.catalog;
		else return;
		idx.empty = false;
		p->size = a[1].as_int64();
		p->sha1 = a[2].as_string();
	    }
	    else if (a.size() >= 2 && a[0].is_uint64() && a[1].is_uint64()) {
		idx.empty = false;
		if(a.size() >= 3 && a[2].is_object())
		    idx.events[a[0].as_uint64()] = { a[1].as_uint64(),
			schemaFromHeader(a[2].as_object()) };
		else idx.events[a[0].as_uint64()] = { a[1].as_uint64(),
			// header здесь именно копируется, std::move нельзя
						      header };
	    }
	}
    });
}
void Store::saveSyncIndex(int peerDn, const SyncIndex &idx) const {
    std::stringstream content;
    content << canonicalHeaderLine() << "\n";
    if(!idx.dnMap.empty()) {
	content << "[\"device-map\"";
	for(auto &[r,l] : idx.dnMap) content << ',' << r << ',' << l;
	content << "]\n";
    }
    content << idx.device.serialize("device"sv) << "\n";
    content << idx.people.serialize("people"sv) << "\n";
    content << idx.catalog.serialize("catalog"sv) << "\n";
    auto can = canonicalSchema();
    for (auto& [month, o] : idx.events) {
        json::array a;
	a.emplace_back(month);
	a.emplace_back(o.offset);
	if(o.header != can) a.emplace_back(headerObjectFor(o.header));
        content << json::serialize(a) << "\n";
    }
    writeAtomic(syncIndexPath(peerDn), content.str());
}

} // namespace ha
