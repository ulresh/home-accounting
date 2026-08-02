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

#define CE \
    return a->event_datetime < b->event_datetime || \
	(a->event_datetime == b->event_datetime && \
	 (a->edit_datetime < b->edit_datetime || \
	  (a->edit_datetime == b->edit_datetime && \
	   (a->rec_no < b->rec_no || \
	    (a->rec_no == b->rec_no && \
	     a->dev_no < b->dev_no)))));

inline bool compareEvents(const std::shared_ptr<Event> &a,
			  const std::shared_ptr<Event> &b) { CE }
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
		    const std::shared_ptr<Event> &b) const { CE }
    bool operator()(const std::shared_ptr<Event> &a,
		    const RecRefDel *b) const { CE }
    bool operator()(const RecRefDel *a,
		    const std::shared_ptr<Event> &b) const { CE }
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
    void sum(const std::string &content);
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
    std::string serialize() const;
};

struct MonthSyncData {
    int64_t offset;
    Schema header;
};
struct SyncIndex {
    bool empty = true;
    FileState people, catalog, device;
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
    // Имя базы ДО первой загрузки: на первом запуске его спрашивают у
    // пользователя, и создавать надо сразу его — «Основная» не должна
    // появиться ни в database.jsonl, ни отдельной папкой.
    void setInitialDatabase(const std::string& name) { if (!name.empty()) db_ = name; }
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
    // Удаление — перенос в раздел ["delete"] с отметкой времени: при слиянии
    // выигрывает более поздняя отметка (см. CatalogIncrementLoader, состояния 2/3).
    void removeCatalogItem(const std::string& category, const std::string& item);
    void removeCatalogCategory(const std::string& category);
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
    int addDevice(std::string_view pubkey, std::string_view name = {});
    void addDevice(std::unique_ptr<std::ofstream> &outp,
		   int no, const std::string &pubkey,
		   const std::string &name = {}, int nn = 0,
		   bool disabled = false);

    // --- имя устройства и признак «Отключено» ---
    // Имя своего устройства правит только пользователь: синхронизация его не
    // меняет. «Отключено» = с этим устройством нельзя синхронизироваться
    // напрямую; на своём устройстве признак не имеет смысла и не ставится.
    const std::string &deviceName() const;            // имя текущего устройства
    int deviceNn() const;                             // счётчик его изменений
    void setDeviceName(const std::string &name);
    void setDeviceDisabled(int no, bool disabled);    // кроме текущего
    const Device *findDevice(const std::string &pubkey) const;
    std::string deviceNameOf(const std::string &pubkey) const;
    bool deviceDisabled(const std::string &pubkey) const;
    // Первый запуск: конфигурации ещё нет (спросить имя устройства и базы).
    bool isFirstRun() const;

    // --- синхронизация (файловая, инкрементная, потоковая) ---
    // Манифест наших справочников (для обмена в начале сессии).
    static FileState stateOf(const std::filesystem::path &p);
    void listManifest(SyncIndex &m) const;

    // Индекс по партнёру: sync/<peerDn>.jsonl — СОСТОЯНИЕ СОБЕСЕДНИКА: сколько
    // байт каждого нашего месячного файла у него уже есть. [yyyymm, offset].
    void loadSyncIndex(int peerDn, SyncIndex &idx) const;
    void saveSyncIndex(int peerDn, const SyncIndex &idx) const;
    void checkCanonical(int yyyymm, const Schema &header);

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
    void savePeople(FileState *state = nullptr);
    void saveCatalog(FileState *state = nullptr);
    void saveDevices(FileState *state = nullptr);

    template<typename T> void read_last_edit(const T &d);
    // Низкоуровневая дозапись строки в месячный файл (+ учёт схемы/наличия).
    void appendToMonth(int yyyymm, const std::string& line);
    // Перед записью НАШЕЙ строки убедиться, что действует наша каноническая схема.
    void ensureCanonicalHeader(int yyyymm);
    void writeCanonicalHeader(int yyyymm, std::ofstream &out);
    // Записать строку удаления (target + this + update).
    void writeDelete(const std::string& tgtEvent, const std::string& tgtEdit,
		     int tgtRn, int tgtDn, const Event *update = nullptr);
    void writeDelete(std::ofstream &out, const RecRefDel &d,
		     const RecRef &t, const RecRefDel &u);

    int  allocRecNo(const std::string &stamp, int yyyymm);

    static std::string deviceLine(const Device &d);
    static std::string eventToLine(const Event& e);
    static Event *parseEventArray(const json::array& a, const Schema& s,
				  const std::map<int, int> *dnMap = nullptr);
    static RecRef parseRef(const json::array& a,
			   const std::vector<std::string>& ref,
			   const std::map<int, int> *dnMap = nullptr);
    static RecRefDel parseRefDel(const json::array& a,
			   const std::vector<std::string>& ref,
			   const std::map<int, int> *dnMap = nullptr);

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

struct CatalogIncrementLoader {
    CatalogIncrementLoader(Store &store) : store(store) {}
    void add(const json::value &v);
    Store &store;
    int state = 0;
    /* 0 - actual category
       1 - actual item
       2 - delete item
       3 - delete category
       4 - skip (in deleted category)
     */
    CategoryItems *current;
};

struct MonthEvents {
    MonthEvents(Store &store) : store(store) {}
    void add(const json::value &v);
    void commit(int yyyymm);
    Store &store;
    Schema header;
    Store::TempEvents monthEvents;
};

struct MonthDeletions {
    struct Op {
	RecRefDel del;
	RecRef ths;
	RecRefDel upd;
	bool operator < (const Op &rhs) const {
	    return del < rhs.del || (del == rhs.del &&
		(ths < rhs.ths || (ths == rhs.ths && upd < rhs.upd)));
	}
    };
    void read(fs::path p);
    struct CompareOps {
	using is_transparent = void;
	bool operator()(const Op &a, const Op &b) const { return a < b; }
	bool operator()(const Op &a, const Event &b) const {
	    return RecRef::less(a.del, b); }
	bool operator()(const Event &a, const Op &b) const {
	    return RecRef::less(a, b.del); }
    };
    std::set<Op, CompareOps> ops;
};

} // namespace ha
