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
inline std::string canonicalHeaderLine() {
    return headerLineFor(canonicalSchema());
}
Schema::Schema(const json::object &o) {
    if (auto* h = o.if_contains("header"))
        for (auto& c : h->as_array())
	    if (c.is_string())
		columns.push_back(std::string(c.as_string()));
    if (auto* r = o.if_contains("reference"))
        for (auto& c : r->as_array())
	    if (c.is_string())
		reference.push_back(std::string(c.as_string()));
    if (columns.empty())   columns = canonicalSchema().columns;
    if (reference.empty()) reference = canonicalSchema().reference;
}
static Schema schemaFromHeader(const json::object& o) { return Schema(o); }

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
    readValues(dbDir() / "people.jsonl", [&](const json::value& v){
        if (v.is_string())
	    people_.insert(people_.end(), std::string(v.as_string()));
    });
}
void Store::savePeople() {
    std::string content;
    for (auto& p : people_) content += json::serialize(jv(p)) + "\n";
    writeAtomic(dbDir() / "people.jsonl", content);
}

void Store::appendCatalog(Catalog &catalog_, const json::value &v) {
    auto& a = v.as_array();
    if (a.empty()) return;
    auto p = catalog_.try_emplace(catalog_.end(),
				  std::string(a[0].as_string()));
    for (size_t i = 1; i < a.size(); ++i)
	p->second.insert(p->second.end(),
			 std::string(a[i].as_string()));
}

void Store::loadCatalog() {
    catalog_.clear();
    readValues(dbDir() / "catalog.jsonl", [&](const json::value &v){
        try { appendCatalog(v); } catch (...) {}
    });
}
void Store::saveCatalog() {
    std::string content;
    for (auto& e : catalog_) {
        json::array a;
        a.emplace_back(e.first);
        for (auto& it : e.second) a.emplace_back(it);
        content += json::serialize(a) + "\n";
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
	if(e.second.contains(subject)) return e.first;
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
    if (sync_) ; // TODO +++ { ensureDeleteKeysLoaded(); if (sync_->deleteKeys.count(dkey)) return false; }
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
    if (sync_) ; // TODO +++ sync_->deleteKeys.insert(dkey);
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
    if (name.empty()) return;
    if(people_.insert(name).second)
	savePeople();
}
void Store::removePerson(const std::string& name) {
    if(people_.erase(name)) savePeople();
}

void Store::upsertCatalog(const CatalogEntry& e) {
    auto [p,a] = catalog_.try_emplace(e.category, e.items);
    if(!a) p->second.insert(e.items.begin(), e.items.end());
    saveCatalog();
}
void Store::replaceCatalog(const Catalog &list) {
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
	for(auto &item : *cat) {
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
/* TODO +++ int Store::reserveDeviceNo(const std::string& pubkey, int preferredNo, const std::string& name) {
    for (auto& d : devices_) if (d.pubkey == pubkey) return d.no;
    int maxNo = 0; bool taken = false;
    for (auto& d : devices_) { maxNo = std::max(maxNo, d.no); if (d.no == preferredNo) taken = true; }
    int no = (preferredNo > 0 && !taken) ? preferredNo : maxNo + 1;
    devices_.push_back(Device{no, pubkey, name});
    saveDevices();
    return no;
}*/
bool Store::hasData() const {
    if (!events_.empty()) return true;
    if (!people_.empty()) return true;
    if (!catalog_.empty()) return true;
    for (auto& d : devices_) if (d.pubkey != myPubkey_) return true;
    return false;
}
/*int Store::maxDeviceNo() const {
    int m = 0;
    for (auto& d : devices_) m = std::max(m, d.no);
    return m;
}*/
/*void Store::renumberSelf(int newNo) {
    for (auto& d : devices_) if (d.pubkey == myPubkey_) d.no = newNo;
    deviceNo_ = newNo;
    saveDevices();
    saveConfig();
}*/

int Store::addDevice(std::string_view pubkey) {
    int m = 0;
    for(auto& d : devices_) {
	// могла быть незавершённая синхронизация, при которой мы добавили собеседника, но собеседник у себя синхронизацию не записал
	if(d.pubkey == pubkey) return d.no;
	if(m < d.no) m = d.no;
    }
    devices_.push_back(Device{++m, std::string(pubkey)});
    // saveDevices();
    json::array a;
    a.emplace_back(m);
    a.emplace_back(pubkey);
    appendLine(pDevice(), json::serialize(a)); // TODO +++ аналогично сделать addPeople
    return m;
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
SyncIndex Store::loadSyncIndex(int peerDn) const {
    SyncIndex idx;
    Schema header = canonicalSchema();
    readValues(syncIndexPath(peerDn), [&](const json::value& v){
	if(v.is_object()) header = schemaFromHeader(v.as_object());
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
		    return;
		}
		else if(s == "people") p = &idx.people;
		else if(s == "catalog") p = &idx.catalog;
		else return;
		p->size = a[1].as_int64();
		p->sha1 = a[2].as_string();
	    }
	    else if (a.size() >= 2 && a[0].is_uint64() && a[1].is_uint64()) {
		if(a.size() >= 3 && a[2].is_object())
		    idx.events[a[0].as_uint64()] = { a[1].as_uint64(),
			schemaFromHeader(a[2].as_object()) };
		else idx.events[a[0].as_uint64()] = { a[1].as_uint64(),
			// header здесь именно копируется, std::move нельзя
						      header };
	    }
	}
    });
    return idx;
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

void Store::syncBegin(int peerDn) {
    sync_ = std::make_unique<SyncSession>(*this, peerDn);
}
void Store::syncEnd() { sync_.reset(); }

/* TODO +++ void Store::ensureDeleteKeysLoaded() {
    if (!sync_ || sync_->deleteKeysLoaded) return;
    for (auto& [yyyymm, path] : enumerateMonths(dbDir())) {
        (void)yyyymm;
        Schema cur = canonicalSchema();
        readValues(path, [&](const json::value& v){
            if (!v.is_object()) return;
            auto& o = v.as_object();
            if (o.if_contains("header")) { cur = schemaFromHeader(o); return; }
            if (auto* del = o.if_contains("delete")) {
                RecRef t = parseRef(del->as_array(), cur.reference);
                bool upd = o.if_contains("update") && o.at("update").is_bool() && o.at("update").as_bool();
                sync_->deleteKeys.insert(keyStr(t.edit, t.rn, t.dn) + "|" + (upd ? "1" : "0"));
            }
        });
    }
    sync_->deleteKeysLoaded = true;
}*/

/* TODO +++ std::string Store::inEffectHeader(int yyyymm) const {
    auto it = monthSchema_.find(yyyymm);
    if (it == monthSchema_.end()) return canonicalHeaderLine();
    return headerLineFor(it->second);
}*/

// Что отправить партнёру (только планы — данные в память не накапливаем).
// Справочники шлём, только если у партнёра другая версия (его манифест);
// события — хвостом от offset, который у партнёра уже есть.
/* TODO +++ std::vector<SyncSendItem> Store::syncPlanOutgoing(const ListManifest& peer) const {
    std::vector<SyncSendItem> out;
    // device-data — всегда (получателю нужен наш список устройств для DN-map).
    out.push_back(SyncSendItem{"device-data", 0, 0, "", pDevice(), 0,
                               stateOf(pDevice()).size});
    {
        FileState me = stateOf(dbDir() / "people.jsonl");
        if (me.size != peer.people.size || me.sha1 != peer.people.sha1)
            out.push_back(SyncSendItem{"people-data", 0, 0, "", dbDir() / "people.jsonl", 0, me.size});
    }
    {
        FileState me = stateOf(dbDir() / "catalog.jsonl");
        if (me.size != peer.catalog.size || me.sha1 != peer.catalog.sha1)
            out.push_back(SyncSendItem{"catalog-data", 0, 0, "", dbDir() / "catalog.jsonl", 0, me.size});
    }
    for (auto& [yyyymm, path] : enumerateMonths(dbDir())) {
        long long off = 0;
        if (sync_) { auto it = sync_->offsets.find(yyyymm); if (it != sync_->offsets.end()) off = it->second; }
        long long size = (long long)fs::file_size(path);
        if (size <= off) continue;
        SyncSendItem it;
        it.kind = "event-tail"; it.month = yyyymm; it.offset = off;
        it.path = path; it.fileFrom = off; it.fileLen = size - off;
        if (off > 0) it.prepend = inEffectHeader(yyyymm) + "\n";   // самоописываемый хвост
        out.push_back(std::move(it));
    }
    return out;
}*/

// TODO +++ revision mark
#if 0
// ---- потоковый приём блока (инкрементно, без накопления всего блока) ----
void Store::syncRecvBegin(const std::string& kind, int month, bool replaceLists) {
    if (!sync_) return;
    sync_->recv = std::make_unique<RecvState>();
    auto& r = *sync_->recv;
    r.kind = kind; r.month = month; r.replaceLists = replaceLists;
    r.cur = canonicalSchema();
}

void Store::syncRecvFeed(const char* data, std::size_t n) {
    if (!sync_ || !sync_->recv) return;
    auto& r = *sync_->recv;
    const char* p = data; std::size_t rem = n;
    while (rem > 0) {
        if (r.atStart) {
            while (rem > 0 && (unsigned char)*p <= ' ') { ++p; --rem; }
            if (rem == 0) break;
            r.atStart = false;
        }
        boost::system::error_code ec;
        std::size_t consumed = r.sp.write_some(p, rem, ec);
        p += consumed; rem -= consumed;
        if (ec) { r.sp.reset(); r.atStart = true; break; }
        if (r.sp.done()) { json::value v = r.sp.release(); handleRecvValue(v); r.sp.reset(); r.atStart = true; }
        else break;
    }
}

void Store::syncRecvFinish() {
    if (!sync_ || !sync_->recv) return;
    auto& r = *sync_->recv;
    if (r.kind == "people-data") {
        if (r.replaceLists) { people_ = r.people; savePeople(); }
        else {
            bool ch = false;
            for (auto& p : r.people)
                if (std::find(people_.begin(), people_.end(), p) == people_.end()) { people_.push_back(p); ch = true; }
            if (ch) savePeople();
        }
    } else if (r.kind == "catalog-data") {
        if (r.replaceLists) { catalog_ = r.catalog; saveCatalog(); }
        else for (auto& e : r.catalog) upsertCatalog(e);
    }
    sync_->recv.reset();
}

void Store::handleRecvValue(const json::value& v) {
    auto& r = *sync_->recv;
    if (r.kind == "device-data") {
        if (!v.is_array()) return;
        auto& a = v.as_array();
        if (a.size() < 2 || !a[1].is_string()) return;
        int peerNo = asInt(a[0]);
        std::string pub = std::string(a[1].as_string());
        std::string name = (a.size() > 2 && a[2].is_string()) ? std::string(a[2].as_string()) : "peer";
        int localNo = reserveDeviceNo(pub, peerNo, name);
        sync_->dnMap[peerNo] = localNo;
        return;
    }
    if (r.kind == "people-data") {
        if (v.is_string()) r.people.push_back(std::string(v.as_string()));
        return;
    }
    if (r.kind == "catalog-data") {
        if (v.is_array() && !v.as_array().empty()) {
            auto& a = v.as_array();
            CatalogEntry e; e.category = asStr(a[0]);
            for (size_t i = 1; i < a.size(); ++i) e.items.push_back(asStr(a[i]));
            r.catalog.push_back(std::move(e));
        }
        return;
    }
    if (r.kind != "event-tail") return;
    int month = r.month;
    if (v.is_object()) {
        auto& o = v.as_object();
        if (o.if_contains("header")) {
            Schema s = schemaFromHeader(o);
            r.cur = s; r.headerReceived = true;
            auto& last = monthSchema_[month];
            if (last != s) { appendToMonth(month, headerLineFor(s)); last = s; }
            return;
        }
        if (o.if_contains("delete")) {
            if (!r.headerReceived) {
                r.cur = canonicalSchema();
                auto& last = monthSchema_[month];
                if (last != r.cur) { appendToMonth(month, canonicalHeaderLine()); last = r.cur; }
            }
            RecRef tgt = parseRef(o.at("delete").as_array(), r.cur.reference);
            auto mt = sync_->dnMap.find(tgt.dn);
            if (mt == sync_->dnMap.end()) return;
            int tdn = mt->second;
            bool upd = o.if_contains("update") && o.at("update").is_bool() && o.at("update").as_bool();
            std::string dkey = keyStr(tgt.edit, tgt.rn, tdn) + "|" + (upd ? "1" : "0");
            ensureDeleteKeysLoaded();
            if (sync_->deleteKeys.count(dkey)) return;
            sync_->deleteKeys.insert(dkey);
            int rIdx = refIndexOfDn(r.cur.reference);
            json::object out = o;
            if (rIdx >= 0) {
                out.at("delete").as_array()[rIdx] = tdn;
                if (out.if_contains("this")) {
                    RecRef a = parseRef(o.at("this").as_array(), r.cur.reference);
                    auto ma = sync_->dnMap.find(a.dn);
                    out.at("this").as_array()[rIdx] = (ma != sync_->dnMap.end()) ? ma->second : a.dn;
                }
            }
            appendToMonth(month, json::serialize(out));
            applyDeleteToState(keyStr(tgt.edit, tgt.rn, tdn));
            sync_->received++;
        }
        return;
    }
    if (v.is_array()) {
        if (!r.headerReceived) {
            r.cur = canonicalSchema();
            auto& last = monthSchema_[month];
            if (last != r.cur) { appendToMonth(month, canonicalHeaderLine()); last = r.cur; }
        }
        int dnIdx = -1;
        for (size_t i = 0; i < r.cur.columns.size(); ++i) if (r.cur.columns[i] == "dev_no") dnIdx = (int)i;
        if (dnIdx < 0) return;
        Event e = parseEventArray(v.as_array(), r.cur);
        auto m = sync_->dnMap.find(e.dev_no);
        if (m == sync_->dnMap.end()) return;
        int md = m->second;
        if (md == deviceNo_) return;                            // эхо своих
        e.dev_no = md;
        if (knownEvent(e.key())) return;
        json::array outA = v.as_array();
        if (dnIdx < (int)outA.size()) outA[dnIdx] = md;
        appendToMonth(month, json::serialize(outA));
        applyEventToState(e);
        sync_->received++;
    }
}

int Store::syncDedup() {
    std::map<std::string, std::vector<Event>> groups;
    for (auto& [k, e] : live_) {
        std::string sig = e.event_datetime + "\x01" + e.subject + "\x01" + costToStr(e.cost) + "\x01"
                        + (e.people  ? *e.people  : "") + "\x01"
                        + (e.volume  ? *e.volume  : "") + "\x01"
                        + (e.comment ? *e.comment : "");
        groups[sig].push_back(e);
    }
    int n = 0;
    for (auto& [sig, vec] : groups) {
        if (vec.size() < 2) continue;
        std::sort(vec.begin(), vec.end(), [](const Event& a, const Event& b){
            if (a.edit_datetime != b.edit_datetime) return a.edit_datetime < b.edit_datetime;
            if (a.dev_no != b.dev_no) return a.dev_no < b.dev_no;
            return a.rec_no < b.rec_no;
        });
        for (size_t i = 1; i < vec.size(); ++i) {           // оставить самую раннюю
            const Event& d = vec[i];
            if (writeDelete(d.edit_datetime, d.rec_no, d.dev_no, false)) {
                applyDeleteToState(d.key());
                ++n;
            }
        }
    }
    return n;
}

void Store::syncCommit(int peerDn) {
    // Состояние собеседника: после успешного обмена у него есть все наши месячные
    // файлы целиком — фиксируем их текущие размеры как его offset.
    std::map<int, long long> off;
    for (auto& [yyyymm, path] : enumerateMonths())
        off[yyyymm] = (long long)fs::file_size(path);
    saveSyncIndex(peerDn, off);
}
#endif

} // namespace ha
