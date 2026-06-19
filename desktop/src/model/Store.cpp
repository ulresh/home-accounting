#include "Store.h"
#include "Paths.h"
#include "Jsonl.h"
#include "../sync/Crypto.h"

#include <boost/json.hpp>
#include <openssl/evp.h>
#include <ctime>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
namespace json = boost::json;

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

static std::string sha1hex(const std::string& data) {
    unsigned char md[EVP_MAX_MD_SIZE]; unsigned int n = 0;
    EVP_Digest(data.data(), data.size(), md, &n, EVP_sha1(), nullptr);
    static const char* H = "0123456789abcdef";
    std::string out; out.reserve(n * 2);
    for (unsigned i = 0; i < n; ++i) { out += H[md[i] >> 4]; out += H[md[i] & 0xF]; }
    return out;
}

// ---- файловые помощники ----
static std::string readWhole(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
static std::string readSlice(const fs::path& p, long long off, long long len) {
    std::ifstream in(p, std::ios::binary);
    if (!in || len <= 0) return {};
    in.seekg(off);
    std::string s; s.resize((size_t)len);
    in.read(&s[0], len);
    s.resize((size_t)in.gcount());
    return s;
}
static FileState stateOf(const fs::path& p) {
    FileState st;
    std::string c = readWhole(p);
    st.size = (long long)c.size();
    if (!c.empty()) st.sha1 = sha1hex(c);
    return st;
}

// Разобрать строковое содержимое как последовательность JSON-значений
// (границы — парсером, как в readValues, но из памяти).
static void forEachValue(const std::string& content,
                         const std::function<void(const json::value&, std::string_view)>& cb) {
    const char* data = content.data();
    std::size_t n = content.size(), pos = 0;
    json::stream_parser sp;
    while (pos < n) {
        while (pos < n && (unsigned char)data[pos] <= ' ') ++pos;
        if (pos >= n) break;
        sp.reset();
        boost::system::error_code ec;
        std::size_t consumed = sp.write_some(data + pos, n - pos, ec);
        if (ec) break;
        std::size_t start = pos;
        pos += consumed;
        if (!sp.done()) break;
        json::value v = sp.release();
        std::size_t end = pos;
        while (end > start && (unsigned char)data[end - 1] <= ' ') --end;
        cb(v, std::string_view(data + start, end - start));
    }
}

// ---- схемы ----
static Schema canonicalSchema() {
    return Schema{
        {"event_datetime","subject","cost","edit_datetime","rec_no","dev_no","people","volume","comment"},
        {"edit_datetime","rec_no","dev_no"}};
}
static std::string canonicalHeaderLine() {
    Schema s = canonicalSchema();
    json::object o;
    json::array cols, ref;
    for (auto& c : s.columns) cols.emplace_back(c);
    for (auto& r : s.reference) ref.emplace_back(r);
    o["header"] = std::move(cols);
    o["reference"] = std::move(ref);
    return json::serialize(o);
}
static Schema schemaFromHeader(const json::object& o) {
    Schema s;
    if (auto* h = o.if_contains("header"))
        for (auto& c : h->as_array()) if (c.is_string()) s.columns.push_back(std::string(c.as_string()));
    if (auto* r = o.if_contains("reference"))
        for (auto& c : r->as_array()) if (c.is_string()) s.reference.push_back(std::string(c.as_string()));
    if (s.columns.empty())   s.columns = canonicalSchema().columns;
    if (s.reference.empty()) s.reference = canonicalSchema().reference;
    return s;
}

static Event parseEventArray(const json::array& a, const Schema& s) {
    Event e;
    for (size_t i = 0; i < s.columns.size() && i < a.size(); ++i) {
        const std::string& c = s.columns[i];
        const json::value& v = a[i];
        if      (c == "event_datetime") e.event_datetime = asStr(v);
        else if (c == "subject")        e.subject = asStr(v);
        else if (c == "cost")           e.cost = asNum(v);
        else if (c == "edit_datetime")  e.edit_datetime = asStr(v);
        else if (c == "rec_no")         e.rec_no = asInt(v);
        else if (c == "dev_no")         e.dev_no = asInt(v);
        else if (c == "people")  { if (v.is_string()) e.people  = asStr(v); }
        else if (c == "volume")  { if (v.is_string()) e.volume  = asStr(v); }
        else if (c == "comment") { if (v.is_string()) e.comment = asStr(v); }
    }
    return e;
}

struct RecRef { std::string edit; int rn = 0; int dn = 0; };
static RecRef parseRef(const json::array& a, const std::vector<std::string>& ref) {
    RecRef r;
    for (size_t i = 0; i < ref.size() && i < a.size(); ++i) {
        if      (ref[i] == "edit_datetime") r.edit = asStr(a[i]);
        else if (ref[i] == "rec_no")        r.rn = asInt(a[i]);
        else if (ref[i] == "dev_no")        r.dn = asInt(a[i]);
    }
    return r;
}
static int refIndexOfDn(const std::vector<std::string>& ref) {
    for (size_t i = 0; i < ref.size(); ++i) if (ref[i] == "dev_no") return (int)i;
    return -1;
}

static std::string keyStr(const std::string& edit, int rn, int dn) {
    return edit + "|" + std::to_string(rn) + "|" + std::to_string(dn);
}
// разбор ключа edit|rn|dn (edit не содержит '|').
static RecRef splitKey(const std::string& k) {
    RecRef r;
    auto p2 = k.rfind('|');
    auto p1 = (p2 == std::string::npos || p2 == 0) ? std::string::npos : k.rfind('|', p2 - 1);
    if (p1 == std::string::npos || p2 == std::string::npos) return r;
    r.edit = k.substr(0, p1);
    r.rn = std::atoi(k.substr(p1 + 1, p2 - p1 - 1).c_str());
    r.dn = std::atoi(k.substr(p2 + 1).c_str());
    return r;
}

// каноническая сериализация НАШЕГО события (с обрезкой хвостовых null).
static std::string eventToLine(const Event& e) {
    std::ostringstream os;
    os << '[' << json::serialize(jv(e.event_datetime)) << ','
       << json::serialize(jv(e.subject)) << ','
       << costToStr(e.cost) << ','
       << json::serialize(jv(e.edit_datetime)) << ','
       << e.rec_no << ',' << e.dev_no;
    bool hasC = e.comment && !e.comment->empty();
    bool hasV = e.volume  && !e.volume->empty();
    bool hasP = e.people  && !e.people->empty();
    if (hasP || hasV || hasC) {
        os << ',' << (hasP ? json::serialize(jv(*e.people)) : std::string("null"));
        if (hasV || hasC) {
            os << ',' << (hasV ? json::serialize(jv(*e.volume)) : std::string("null"));
            if (hasC) os << ',' << json::serialize(jv(*e.comment));
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
static std::vector<std::pair<int,fs::path>> enumerateMonths(const fs::path& dbDir) {
    std::vector<std::pair<int,fs::path>> out;
    if (!fs::exists(dbDir)) return out;
    for (auto& decade : fs::directory_iterator(dbDir)) {
        if (!decade.is_directory()) continue;
        std::string dn = decade.path().filename().string();
        if (dn.empty() || !std::all_of(dn.begin(), dn.end(), ::isdigit)) continue;  // не sync/identity
        for (auto& f : fs::directory_iterator(decade.path())) {
            if (!f.is_regular_file() || f.path().extension() != ".jsonl") continue;
            std::string stem = f.path().stem().string();   // "2606"
            if (stem.size() != 4 || !std::all_of(stem.begin(), stem.end(), ::isdigit)) continue;
            int yymm = std::atoi(stem.c_str());
            int yyyymm = (2000 + yymm / 100) * 100 + (yymm % 100);
            out.emplace_back(yyyymm, f.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void Store::load() {
    live_.clear(); deletedTargets_.clear(); monthSchema_.clear();
    seqAtStamp_.clear(); anyEventLines_ = false;
    loadConfig();
    loadDevices();
    loadPeople();
    loadCatalog();
    loadEvents();
}

void Store::loadConfig() {
    std::vector<std::string> dbs;
    readValues(root_ / "database.jsonl", [&](const json::value& v, std::string_view){
        if (v.is_string()) dbs.push_back(std::string(v.as_string()));
    });
    readValues(root_ / "config.json", [&](const json::value& v, std::string_view){
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
    deviceNo_ = 0;
    people_.clear(); catalog_.clear(); devices_.clear();
    live_.clear(); deletedTargets_.clear(); monthSchema_.clear();
    seqAtStamp_.clear(); anyEventLines_ = false;
    saveConfig();
    loadDevices(); loadPeople(); loadCatalog(); loadEvents();
    ensureIdentity();
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

// ---- загрузка событий: по месяцам, удаления применяются на лету ----
void Store::loadEvents() {
    for (auto& [yyyymm, path] : enumerateMonths(dbDir())) {
        Schema cur = canonicalSchema();
        bool sawHeader = false;
        readValues(path, [&](const json::value& v, std::string_view raw){
            (void)raw;
            if (v.is_object()) {
                auto& o = v.as_object();
                if (o.if_contains("header")) { cur = schemaFromHeader(o); monthSchema_[yyyymm] = cur; sawHeader = true; }
                else if (auto* del = o.if_contains("delete")) {
                    RecRef t = parseRef(del->as_array(), cur.reference);
                    applyDeleteToState(keyStr(t.edit, t.rn, t.dn));
                    anyEventLines_ = true;
                }
            }
            else if (v.is_array()) {
                Event e = parseEventArray(v.as_array(), cur);
                anyEventLines_ = true;
                applyEventToState(e);
            }
        });
        if (!sawHeader && !monthSchema_.count(yyyymm)) monthSchema_[yyyymm] = canonicalSchema();
    }
}

void Store::applyEventToState(const Event& e) {
    std::string key = e.key();
    live_[key] = e;
}
void Store::applyDeleteToState(const std::string& targetKey) {
    live_.erase(targetKey);
    deletedTargets_.insert(targetKey);
}

bool Store::knownEvent(const std::string& key) const {
    return live_.count(key) || deletedTargets_.count(key);
}

std::vector<Event> Store::events() const {
    std::vector<Event> out;
    out.reserve(live_.size());
    for (auto& [k, e] : live_) out.push_back(e);
    std::sort(out.begin(), out.end(), [](const Event& a, const Event& b){
        return a.event_datetime > b.event_datetime;
    });
    return out;
}

std::string Store::categoryOf(const std::string& subject) const {
    for (auto& e : catalog_)
        for (auto& it : e.items)
            if (it == subject) return e.category;
    return {};
}

int Store::scanMaxOwnRn(const std::string& stamp) const {
    int m = -1;
    for (auto& [k, e] : live_)
        if (e.dev_no == deviceNo_ && e.edit_datetime == stamp) m = std::max(m, e.rec_no);
    for (auto& key : deletedTargets_) {
        RecRef r = splitKey(key);
        if (r.dn == deviceNo_ && r.edit == stamp) m = std::max(m, r.rn);
    }
    return m;
}
int Store::allocRecNo(const std::string& stamp) {
    auto it = seqAtStamp_.find(stamp);
    int next = (it != seqAtStamp_.end()) ? it->second : (scanMaxOwnRn(stamp) + 1);
    seqAtStamp_[stamp] = next + 1;
    return next;
}

void Store::appendToMonth(int yyyymm, const std::string& line) {
    appendLine(monthPath(yyyymm), line);
}
void Store::ensureCanonicalHeader(int yyyymm) {
    Schema can = canonicalSchema();
    auto &cur = monthSchema_[yyyymm];
    if(cur != can) {
        appendToMonth(yyyymm, canonicalHeaderLine());
	cur = can;
    }
}

bool Store::writeDelete(const std::string& tgtEdit, int tgtRn, int tgtDn, bool update) {
    std::string dkey = keyStr(tgtEdit, tgtRn, tgtDn) + "|" + (update ? "1" : "0");
    if (sync_) { ensureDeleteKeysLoaded(); if (sync_->deleteKeys.count(dkey)) return false; }
    std::string stamp = nowStamp();
    int rn = allocRecNo(stamp);
    int ym = yyyymmOf(tgtEdit);
    ensureCanonicalHeader(ym);
    json::object o;
    json::array del; del.emplace_back(jv(tgtEdit)); del.emplace_back(tgtRn); del.emplace_back(tgtDn);
    json::array ths; ths.emplace_back(jv(stamp)); ths.emplace_back(rn); ths.emplace_back(deviceNo_);
    o["delete"] = std::move(del);
    o["this"]   = std::move(ths);
    if (update) o["update"] = true;
    appendToMonth(ym, json::serialize(o));
    if (sync_) sync_->deleteKeys.insert(dkey);
    return true;
}

Event Store::addEvent(const std::string& event_datetime, const std::string& subject,
                      double cost, std::optional<std::string> people,
                      std::optional<std::string> volume, std::optional<std::string> comment) {
    Event e;
    e.event_datetime = event_datetime;
    e.subject = subject;
    e.cost = cost;
    e.edit_datetime = nowStamp();
    e.dev_no = deviceNo_;
    e.rec_no = allocRecNo(e.edit_datetime);
    e.people = std::move(people);
    e.volume = std::move(volume);
    e.comment = std::move(comment);
    int ym = yyyymmOf(e.event_datetime);
    ensureCanonicalHeader(ym);
    appendToMonth(ym, eventToLine(e));
    applyEventToState(e);
    return e;
}

void Store::deleteEvent(const Event& e) {
    writeDelete(e.edit_datetime, e.rec_no, e.dev_no, false);
    applyDeleteToState(e.key());
}

Event Store::editEvent(const Event& oldEv, const std::string& event_datetime,
                       const std::string& subject, double cost,
                       std::optional<std::string> people, std::optional<std::string> volume,
                       std::optional<std::string> comment) {
    writeDelete(oldEv.edit_datetime, oldEv.rec_no, oldEv.dev_no, true);
    applyDeleteToState(oldEv.key());
    return addEvent(event_datetime, subject, cost, std::move(people), std::move(volume), std::move(comment));
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

std::vector<Event> Store::filter(const std::string& q) const {
    auto all = events();
    if (q.empty()) return all;
    std::set<std::string> memberSet;
    for (auto& c : catalog_)
        if (icontains(c.category, q)) {
            auto m = categoryMembers(c.category);
            memberSet.insert(m.begin(), m.end());
        }
    std::vector<Event> out;
    for (auto& e : all) {
        bool bySubject  = icontains(e.subject, q);
        bool byPerson   = e.people && icontains(*e.people, q);
        bool byComment  = e.comment && icontains(*e.comment, q);
        bool byCategory = memberSet.count(e.subject) > 0;
        if (bySubject || byPerson || byComment || byCategory) out.push_back(e);
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

    int maxNo = 0;
    for (auto& d : devices_)
        if (d.pubkey == myPubkey_) {
            if (deviceNo_ != d.no) { deviceNo_ = d.no; saveConfig(); }
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
    if (anyEventLines_) return true;
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
void Store::renumberSelf(int newNo) {
    for (auto& d : devices_) if (d.pubkey == myPubkey_) d.no = newNo;
    deviceNo_ = newNo;
    saveDevices();
    saveConfig();
}

// =====================================================================
//                      Инкрементная синхронизация
// =====================================================================

std::map<std::string, FileState> Store::fileStates() const {
    std::map<std::string, FileState> st;
    st["device"]  = stateOf(dbDir() / "device.jsonl");
    st["people"]  = stateOf(dbDir() / "people.jsonl");
    st["catalog"] = stateOf(dbDir() / "catalog.jsonl");
    for (auto& [yyyymm, path] : enumerateMonths(dbDir()))
        st[std::to_string(yyyymm)] = stateOf(path);
    return st;
}

std::map<std::string, FileState> Store::loadSyncIndex(int peerDn) const {
    std::map<std::string, FileState> st;
    readValues(syncIndexPath(peerDn), [&](const json::value& v, std::string_view){
        if (!v.is_array()) return;
        auto& a = v.as_array();
        if (a.size() < 2) return;
        std::string key = a[0].is_string() ? std::string(a[0].as_string())
                                           : std::to_string(asInt(a[0]));
        FileState fsr;
        fsr.size = (long long)asInt(a[1]);
        if (a.size() > 2 && a[2].is_string()) fsr.sha1 = std::string(a[2].as_string());
        st[key] = fsr;
    });
    return st;
}
void Store::saveSyncIndex(int peerDn, const std::map<std::string, FileState>& st) const {
    std::string content;
    for (auto& [key, fsr] : st) {
        json::array a;
        bool digits = !key.empty() && std::all_of(key.begin(), key.end(), ::isdigit);
        if (digits) a.emplace_back((int64_t)std::atoll(key.c_str()));
        else        a.emplace_back(key);
        a.emplace_back((int64_t)fsr.size);
        a.emplace_back(fsr.sha1);
        content += json::serialize(a) + "\n";
    }
    writeAtomic(syncIndexPath(peerDn), content);
}

void Store::syncBegin(int peerDn) {
    sync_ = std::make_unique<SyncSession>();
    sync_->peerDn = peerDn;
    sync_->baseline = loadSyncIndex(peerDn);
}
void Store::syncEnd() { sync_.reset(); }

void Store::ensureDeleteKeysLoaded() {
    if (!sync_ || sync_->deleteKeysLoaded) return;
    for (auto& [yyyymm, path] : enumerateMonths(dbDir())) {
        (void)yyyymm;
        Schema cur = canonicalSchema();
        readValues(path, [&](const json::value& v, std::string_view){
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
}

// действующий заголовок в файле на смещении offset (последний header до offset).
static std::string inEffectHeaderAt(const fs::path& path, long long offset) {
    std::string prefix = readSlice(path, 0, offset);
    std::string last;
    std::size_t pos = 0;
    while (pos < prefix.size()) {
        std::size_t nl = prefix.find('\n', pos);
        std::string line = prefix.substr(pos, (nl == std::string::npos ? prefix.size() : nl) - pos);
        if (line.find("\"header\"") != std::string::npos) {
            try { auto v = json::parse(line); if (v.is_object() && v.as_object().if_contains("header")) last = line; }
            catch (...) {}
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return last.empty() ? canonicalHeaderLine() : last;
}

static bool stateChanged(const std::map<std::string, FileState>& base,
                         const std::map<std::string, FileState>& cur, const std::string& key) {
    auto c = cur.find(key);
    FileState cv = (c != cur.end()) ? c->second : FileState{};
    auto b = base.find(key);
    if (b == base.end()) return cv.size > 0;
    return b->second.size != cv.size || b->second.sha1 != cv.sha1;
}

std::vector<SyncBlob> Store::syncBuildOutgoing(bool forceLists) const {
    std::vector<SyncBlob> out;
    auto cur = fileStates();
    std::map<std::string, FileState> empty;
    const auto& base = sync_ ? sync_->baseline : empty;

    // device-data всегда (нужно для DN-map у получателя)
    out.push_back(SyncBlob{"device-data", 0, 0, readWhole(dbDir() / "device.jsonl")});

    if (forceLists || stateChanged(base, cur, "people"))
        out.push_back(SyncBlob{"people-data", 0, 0, readWhole(dbDir() / "people.jsonl")});
    if (forceLists || stateChanged(base, cur, "catalog"))
        out.push_back(SyncBlob{"catalog-data", 0, 0, readWhole(dbDir() / "catalog.jsonl")});

    for (auto& [yyyymm, path] : enumerateMonths(dbDir())) {
        std::string key = std::to_string(yyyymm);
        long long baseSize = 0;
        auto it = base.find(key);
        if (it != base.end()) baseSize = it->second.size;
        long long size = (long long)fs::file_size(path);
        if (size <= baseSize) continue;
        std::string data;
        if (baseSize > 0) data = inEffectHeaderAt(path, baseSize) + "\n";
        data += readSlice(path, baseSize, size - baseSize);
        out.push_back(SyncBlob{"event-tail", yyyymm, baseSize, std::move(data)});
    }
    return out;
}

// --- применение входящих блоков ---
void Store::syncApply(const SyncBlob& b, bool replaceLists, std::set<std::string>* addedDevices) {
    if (b.kind == "device-data") {
        forEachValue(b.data, [&](const json::value& v, std::string_view){
            if (!v.is_array()) return;
            auto& a = v.as_array();
            if (a.size() < 2 || !a[1].is_string()) return;
            int peerNo = asInt(a[0]);
            std::string pub = std::string(a[1].as_string());
            std::string name = (a.size() > 2 && a[2].is_string()) ? std::string(a[2].as_string()) : "peer";
            bool isNew = !knowsDevice(pub);
            int localNo = reserveDeviceNo(pub, peerNo, name);
            if (isNew && addedDevices) addedDevices->insert(pub);
            if (sync_) sync_->dnMap[peerNo] = localNo;
        });
        return;
    }
    if (b.kind == "people-data") {
        std::vector<std::string> incoming;
        forEachValue(b.data, [&](const json::value& v, std::string_view){
            if (v.is_string()) incoming.push_back(std::string(v.as_string()));
        });
        if (replaceLists) { people_ = incoming; savePeople(); }
        else {
            bool ch = false;
            for (auto& p : incoming)
                if (std::find(people_.begin(), people_.end(), p) == people_.end()) { people_.push_back(p); ch = true; }
            if (ch) savePeople();
        }
        return;
    }
    if (b.kind == "catalog-data") {
        std::vector<CatalogEntry> incoming;
        forEachValue(b.data, [&](const json::value& v, std::string_view){
            if (!v.is_array() || v.as_array().empty()) return;
            auto& a = v.as_array();
            CatalogEntry e; e.category = asStr(a[0]);
            for (size_t i = 1; i < a.size(); ++i) e.items.push_back(asStr(a[i]));
            incoming.push_back(std::move(e));
        });
        if (replaceLists) { catalog_ = incoming; saveCatalog(); }
        else for (auto& e : incoming) upsertCatalog(e);
        return;
    }
    if (b.kind == "event-tail") {
        if (!sync_) return;
        int month = b.month;
	bool header_received = false;
        Schema cur;
        forEachValue(b.data, [&](const json::value& v, std::string_view raw){
            if (v.is_object()) {
                auto& o = v.as_object();
                if (o.if_contains("header")) {
                    Schema s = schemaFromHeader(o);
                    cur = s; header_received = true;
		    auto &last = monthSchema_[month];
		    if (last != s) {
                        appendToMonth(month, std::string(raw));
			last = s;
		    }
                    return;
                }
                if (o.if_contains("delete")) {
		    if (!header_received) {
			cur = canonicalSchema();
			auto &last = monthSchema_[month];
			if (last != cur) {
			    appendToMonth(month, canonicalHeaderLine());
			    last = cur;
			}
		    }
                    RecRef tgt = parseRef(o.at("delete").as_array(), cur.reference);
                    auto mt = sync_->dnMap.find(tgt.dn);
                    if (mt == sync_->dnMap.end()) return;               // DN не в map
                    int tdn = mt->second;
                    bool upd = o.if_contains("update") && o.at("update").is_bool() && o.at("update").as_bool();
                    std::string dkey = keyStr(tgt.edit, tgt.rn, tdn) + "|" + (upd ? "1" : "0");
                    ensureDeleteKeysLoaded();
                    if (sync_->deleteKeys.count(dkey)) return;          // повтор не вставляем
                    sync_->deleteKeys.insert(dkey);
                    int rIdx = refIndexOfDn(cur.reference);
                    json::object out = o;
                    if (rIdx >= 0) {
                        out.at("delete").as_array()[rIdx] = tdn;
                        if (out.if_contains("this")) {
                            RecRef a = parseRef(o.at("this").as_array(), cur.reference);
                            auto ma = sync_->dnMap.find(a.dn);
                            out.at("this").as_array()[rIdx] = (ma != sync_->dnMap.end()) ? ma->second : a.dn;
                        }
                    }
                    appendToMonth(month, json::serialize(out));
                    applyDeleteToState(keyStr(tgt.edit, tgt.rn, tdn));
                    sync_->received++;
                    return;
                }
                return;
            }
            if (v.is_array()) {
		if (!header_received) {
		    cur = canonicalSchema();
		    auto &last = monthSchema_[month];
		    if (last != cur) {
			appendToMonth(month, canonicalHeaderLine());
			last = cur;
		    }
		}
                int dnIdx = -1;
                for (size_t i = 0; i < cur.columns.size(); ++i) if (cur.columns[i] == "dev_no") dnIdx = (int)i;
                if (dnIdx < 0) return;
                Event e = parseEventArray(v.as_array(), cur);
                auto m = sync_->dnMap.find(e.dev_no);
                if (m == sync_->dnMap.end()) return;                    // DN не в map
                int md = m->second;
                if (md == deviceNo_) return;                            // эхо своих
                e.dev_no = md;
                if (knownEvent(e.key())) return;                        // уже есть/удалено
                json::array outA = v.as_array();
                if (dnIdx < (int)outA.size()) outA[dnIdx] = md;
                appendToMonth(month, json::serialize(outA));
                applyEventToState(e);
                sync_->received++;
            }
        });
        return;
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
    saveSyncIndex(peerDn, fileStates());
}

} // namespace ha
