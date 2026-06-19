#pragma once
#include "Types.h"
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <optional>
#include <memory>

namespace ha {

// Состояние файла для инкрементной синхронизации: размер и контрольная сумма.
struct FileState {
    long long   size = 0;
    std::string sha1;
};

// Блок данных, передаваемый при синхронизации «без разбора» (хвост файла).
//   kind = "event-tail"      -> [event-tail, yyyymm, offset, size]\n<данные>\n
//   kind = "device-data"     -> [device-data, size]\n<данные>\n
//   kind = "people-data"     -> [people-data, size]\n<данные>\n
//   kind = "catalog-data"    -> [catalog-data, size]\n<данные>\n
struct SyncBlob {
    std::string kind;
    int         month  = 0;   // yyyymm, только для event-tail
    long long   offset = 0;   // только для event-tail
    std::string data;         // сырое содержимое
};

// Схема событийной строки: порядок/состав колонок и состав «ссылки» (reference),
// по которой строятся delete/this. Собеседник может прислать другой порядок —
// мы храним строки так, как получили (только с DN map), поэтому в одном файле
// могут оказаться строки с разными схемами; перед каждой схемой идёт header.
struct Schema {
    std::vector<std::string> columns;
    std::vector<std::string> reference;
    bool operator==(const Schema& o) const {
        return columns == o.columns && reference == o.reference;
    }
    bool operator!=(const Schema& o) const { return !(*this == o); }
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

    // --- доступ к данным (текущее видимое состояние) ---
    std::vector<Event>        events() const;            // без удалённых
    const std::vector<std::string>&  people()  const { return people_; }
    const std::vector<CatalogEntry>& catalog() const { return catalog_; }
    const std::vector<Device>&       devices() const { return devices_; }

    std::string categoryOf(const std::string& subject) const;

    // --- мутации ---
    Event addEvent(const std::string& event_datetime, const std::string& subject,
                   double cost, std::optional<std::string> people,
                   std::optional<std::string> volume,
                   std::optional<std::string> comment = std::nullopt);
    void  deleteEvent(const Event& e);
    Event editEvent(const Event& oldEv, const std::string& event_datetime,
                    const std::string& subject, double cost,
                    std::optional<std::string> people, std::optional<std::string> volume,
                    std::optional<std::string> comment = std::nullopt);

    void addPerson(const std::string& name);
    void removePerson(const std::string& name);
    void upsertCatalog(const CatalogEntry& e);
    void replaceCatalog(const std::vector<CatalogEntry>& list);

    std::set<std::string> categoryMembers(const std::string& category) const;
    std::vector<Event> filter(const std::string& q) const;

    // --- идентичность устройства ---
    void ensureIdentity();
    std::string myPubkey() const { return myPubkey_; }
    std::filesystem::path certPath() const;
    std::filesystem::path keyPath() const;

    bool knowsDevice(const std::string& pubkey) const;
    int  reserveDeviceNo(const std::string& pubkey, int preferredNo, const std::string& name);
    bool hasData() const;
    int  maxDeviceNo() const;
    void renumberSelf(int newNo);

    // --- синхронизация (файловая, инкрементная) ---
    // Снимок состояния всех синхронизируемых файлов (device/people/catalog + месяцы).
    std::map<std::string, FileState> fileStates() const;

    // Индекс по партнёру: sync/<peerDn>.jsonl  (что мы уже передавали).
    std::map<std::string, FileState> loadSyncIndex(int peerDn) const;
    void saveSyncIndex(int peerDn, const std::map<std::string, FileState>& st) const;

    // Начать/закончить сессию синхронизации с партнёром (peerDn — его номер у нас).
    void syncBegin(int peerDn);
    void syncEnd();

    // Сформировать исходящие блоки относительно базовой точки сессии.
    // forceLists=true — отдать people/catalog целиком (итог слияния), независимо
    // от изменений (для стороны, которая слила и отдаёт результат).
    std::vector<SyncBlob> syncBuildOutgoing(bool forceLists) const;

    // Применить входящий блок. replaceLists=true — принять people/catalog как
    // получили (другая сторона уже слила). addedDevices — новые pubkey (опц.).
    void syncApply(const SyncBlob& b, bool replaceLists,
                   std::set<std::string>* addedDevices = nullptr);

    // Удаление более поздних дубликатов (совпадение всех полей кроме служебных).
    int  syncDedup();

    // Зафиксировать итоговое состояние индекса партнёра (после вливания).
    void syncCommit(int peerDn);

    int  syncReceived() const { return sync_ ? sync_->received : 0; }

    std::filesystem::path root() const { return root_; }

private:
    std::filesystem::path dbDir() const { return root_ / db_; }
    std::filesystem::path monthPath(int yyyymm) const;
    std::filesystem::path syncIndexPath(int peerDn) const;

    void loadConfig();
    void saveConfig();
    void loadDevices();
    void loadPeople();
    void loadCatalog();
    void loadEvents();
    void savePeople();
    void saveCatalog();
    void saveDevices();

    // Низкоуровневая дозапись строки в месячный файл (+ учёт схемы/наличия).
    void appendToMonth(int yyyymm, const std::string& line);
    // Перед записью НАШЕЙ строки убедиться, что действует наша каноническая схема.
    void ensureCanonicalHeader(int yyyymm);
    // Записать строку удаления (target + this + флаг update). Уважает дедуп.
    bool writeDelete(const std::string& tgtEdit, int tgtRn, int tgtDn, bool update);

    int  allocRecNo(const std::string& stamp);
    int  scanMaxOwnRn(const std::string& stamp) const;
    bool knownEvent(const std::string& key) const;

    // Применить событие/удаление к видимому состоянию (live_/deletedTargets_).
    void applyEventToState(const Event& e);
    void applyDeleteToState(const std::string& targetKey);

    std::filesystem::path root_;
    std::string db_ = "Основная";
    int deviceNo_ = 0;
    int fontSize_ = 0;
    std::string myPubkey_;

    std::vector<std::string>  people_;
    std::vector<CatalogEntry> catalog_;
    std::vector<Device>       devices_;

    std::map<std::string, Event> live_;          // видимые события: key -> Event
    std::set<std::string> deletedTargets_;       // ключи удалённых событий
    std::map<int, Schema> monthSchema_;          // действующая схема по месяцу
    std::map<std::string, int> seqAtStamp_;      // счётчик RN на штамп (наши записи)
    bool anyEventLines_ = false;                 // были ли вообще строки событий

    // Состояние активной сессии синхронизации (между syncBegin/syncEnd).
    struct SyncSession {
        int peerDn = 0;
        std::map<std::string, FileState> baseline;
        std::map<int, int> dnMap;                // DN партнёра -> наш DN
        std::set<std::string> deleteKeys;        // дедуп строк удаления (лениво)
        bool deleteKeysLoaded = false;
        int received = 0;
    };
    std::unique_ptr<SyncSession> sync_;
    void ensureDeleteKeysLoaded();
};

} // namespace ha
