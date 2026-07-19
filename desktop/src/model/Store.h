#pragma once
#include "Types.h"
#include "../shorts.h"
#include <map>
#include <set>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <boost/json.hpp>

namespace ha {

inline bool compareEvents(const std::shared_ptr<Event> &a,
		   const std::shared_ptr<Event> &b) {
    return a->event_datetime < b->event_datetime ||
	(a->event_datetime == b->event_datetime &&
	 (a->edit_datetime < b->edit_datetime ||
	  (a->edit_datetime == b->edit_datetime &&
	   (a->rec_no < b->rec_no ||
	    (a->rec_no == b->rec_no &&
	     a->dev_no < b->dev_no)))));
}
inline bool compareEventChar(const std::shared_ptr<Event> &a,
			     const char *b) {
    auto s = a->event_datetime.size();
    if(s < 7) {
	if(!s) return true;
	else return memcmp(a->event_datetime.data(), b, s) <= 0;
    }
    else return memcmp(a->event_datetime.data(), b, 7) < 0;
}
inline bool compareCharEvent(const char *a,
			     const std::shared_ptr<Event> &b) {
    auto s = b->event_datetime.size();
    if(s < 7) {
	if(!s) return false;
	else return memcmp(a, b->event_datetime.data(), s) < 0;
    }
    else return memcmp(a, b->event_datetime.data(), 7) < 0;
}

struct CompareEventsSet {
    using is_transparent = void;
    bool operator()(const std::shared_ptr<Event> &a,
		    const std::shared_ptr<Event> &b) const {
	return compareEvents(a, b);
    }
    bool operator()(const std::shared_ptr<Event> &a,
		    const char *b) const {
	return compareEventChar(a, b);
    }
    bool operator()(const char *a,
		    const std::shared_ptr<Event> &b) const {
	return compareCharEvent(a, b);
    }
};

struct CompareYyyyMm {
    bool operator()(const std::shared_ptr<Event> &a,
		    const char *b) const {
	return compareEventChar(a, b);
    }
    bool operator()(const char *a,
		    const std::shared_ptr<Event> &b) const {
	return compareCharEvent(a, b);
    }
};

// Состояние файла для инкрементной синхронизации: размер и контрольная сумма.
struct FileState {
    uint64_t size = 0;
    std::string sha1;
    bool operator != (const FileState &rhs) const {
	return size != rhs.size || sha1 != rhs.sha1;
    }
    auto serialize(std::string_view name) const {
	json::array a;
	a.emplace_back(name);
	a.emplace_back(size);
	a.emplace_back(sha1);
	return json::serialize(a);
    }
};

// Схема событийной строки: порядок/состав колонок и состав «ссылки» (reference),
// по которой строятся delete/this. Собеседник может прислать другой порядок —
// мы храним строки так, как получили (только с DN map), поэтому в одном файле
// могут оказаться строки с разными схемами; перед каждой схемой идёт header.
struct Schema {
    Schema(std::vector<std::string> &&columns,
	   std::vector<std::string> &&reference)
	: columns(columns), reference(reference)
    {}
    Schema(const json::object &o, bool add_defaults = true);
    Schema() = default;
    std::vector<std::string> columns;
    std::vector<std::string> reference;
    operator bool() const { return !columns.empty() && !reference.empty(); }
    bool operator==(const Schema& o) const {
        return columns == o.columns && reference == o.reference;
    }
    bool operator!=(const Schema& o) const { return !(*this == o); }
};

// Манифест справочников (для обмена «состоянием» в начале сессии).
struct ListManifest { // TODO +++ check usability
    FileState people, catalog, device;
};
struct MonthSyncData {
    /*MonthSyncData(uint64_t offset, Schema &&header)
	: offset(offset), header(header)
    {}
    MonthSyncData() = default;*/
    uint64_t offset;
    Schema header;
};
struct SyncIndex : ListManifest {
    bool empty = true;
    std::map<int, int> dnMap; // DN партнёра -> наш DN
    std::map<int, MonthSyncData> events;
};

// Центральное хранилище: события — только дозапись; справочники — атомарная
// перезапись. raw в памяти не держим: при загрузке удаления применяются на лету.
class Store {
public:
    explicit Store(std::filesystem::path root = {});

    void load();                       // прочитать config + текущую базу целиком

    // --- конфигурация / база ---
    const std::string& database() const { return db_; }
    int  deviceNo() const { return deviceNo_; }
    std::vector<std::string> databases() const;
    void switchDatabase(const std::string& name, bool create);

    int  fontSize() const { return fontSize_; }
    void setFontSize(int pt);

    typedef std::map<std::string, std::string> People; // значение-время
    typedef std::map<std::string, CategoryItems> Catalog;
    typedef std::set<std::shared_ptr<Event>, CompareEventsSet> Events;
    typedef std::vector<std::shared_ptr<Event> > TempEvents;
    // --- доступ к данным (текущее видимое состояние) ---
    const Events &events() const { return events_; }
    const People &people() const { return people_; }
    const Catalog &catalog() const { return catalog_; }
    const std::vector<Device>&       devices() const { return devices_; }

    std::string categoryOf(const std::string& subject) const;

    // --- мутации ---
    Event &addEvent(const std::string& event_datetime,
		    const std::string& subject,
		    double cost, const std::string &people,
		    const std::string &volume,
		    const std::string &comment = {});
    void  deleteEvent(const std::shared_ptr<Event> &e);
    Event editEvent(const std::shared_ptr<Event> &oldEv,
		    const std::string& event_datetime,
                    const std::string& subject, double cost,
                    const std::string &people, const std::string &volume,
                    const std::string &comment = {});

    void addPerson(const std::string& name);
    void removePerson(const std::string& name);
    void upsertCatalog(const CatalogEntry& e);
    void categoryMembers(std::set<std::string> &result,
			 Catalog::const_reference category) const;
    TempEvents filter(const std::string& q) const;

    // --- идентичность устройства ---
    void ensureIdentity(bool forceSaveConfig = false);
    std::string myPubkey() const { return myPubkey_; }
    std::filesystem::path certPath() const;
    std::filesystem::path keyPath() const;

    int knowsDevice(const std::string &pubkey) const;
    bool hasData() const;
    int  maxDeviceNo() const;
    int addDevice(std::string_view pubkey);
    void addDevice(std::unique_ptr<std::ofstream> &outp,
		   int no, const std::string &pubkey);

    // --- синхронизация (файловая, инкрементная, потоковая) ---
    // Манифест наших справочников (для обмена в начале сессии).
    static FileState stateOf(const std::filesystem::path &p);
    void listManifest(ListManifest &m) const;

    // Индекс по партнёру: sync/<peerDn>.jsonl — СОСТОЯНИЕ СОБЕСЕДНИКА: сколько
    // байт каждого нашего месячного файла у него уже есть. [yyyymm, offset].
    void loadSyncIndex(int peerDn, SyncIndex &idx) const;
    void saveSyncIndex(int peerDn, const SyncIndex &idx) const;

    fs::path root() const { return root_; }
    fs::path pDevice() const { return dbDir()/"device.jsonl"s; }
    fs::path pPeople() const { return dbDir()/"people.jsonl"s; }
    fs::path pCatalog() const { return dbDir()/"catalog.jsonl"s; }

public:
    std::filesystem::path dbDir() const { return root_ / db_; }
    std::filesystem::path monthPath(int yyyymm) const;
    std::filesystem::path syncIndexPath(int peerDn) const;
    std::vector<std::pair<int,fs::path>> enumerateMonths() const;

    void loadConfig();
    void saveConfig();
    void loadDevices();
    void loadPeople();
    void loadCatalog();
    void loadEvents();
    void savePeople();
    void saveCatalog();
    void saveDevices();

    template<typename T> void read_last_edit(const T &d);
    // Низкоуровневая дозапись строки в месячный файл (+ учёт схемы/наличия).
    void appendToMonth(int yyyymm, const std::string& line);
    // Перед записью НАШЕЙ строки убедиться, что действует наша каноническая схема.
    void ensureCanonicalHeader(int yyyymm);
    // Записать строку удаления (target + this + флаг update). Уважает дедуп.
    bool writeDelete(const std::string& tgtEdit, int tgtRn, int tgtDn, bool update);

    int  allocRecNo(const std::string &stamp, int yyyymm);

    std::filesystem::path root_;
    std::string db_ = "Основная";
    int deviceNo_ = 0;
    int fontSize_ = 0;
    std::string myPubkey_;

    People people_, people_delete;
    Catalog catalog_;
    CategoryMap catalog_delete;
    std::vector<Device>       devices_;
    Events events_;

    std::set<int> canonicalSchemaMonths_;
    std::string lastEdit_;
    int lastEditSeq_ = 0;
};

struct CatalogLoader {
    void add(const json::value &v);
    Store::Catalog catalog_;
    CategoryMap catalog_delete;
    CategoryMap *cur = nullptr, *del = nullptr;
};

struct MonthEvents {
    MonthEvents(Store &store) : store(store) {}
    void add(const json::value &v);
    void commit(int yyyymm);
    Store &store;
    Schema header;
    Store::TempEvents monthEvents;
};

} // namespace ha
