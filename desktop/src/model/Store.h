#pragma once
#include "Types.h"
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <boost/json/stream_parser.hpp>
#include <boost/json/value.hpp>

namespace ha {

// Состояние файла для инкрементной синхронизации: размер и контрольная сумма.
struct FileState {
    long long   size = 0;
    std::string sha1;
};

// План отправки одного блока БЕЗ данных в памяти: заголовок-строка + (опц.)
// prepend + содержимое файла [fileFrom, fileFrom+fileLen). Сеть-уровень читает
// файл блоками и сразу шлёт. Кадры:
//   kind = "event-tail"   -> [event-tail, yyyymm, offset, size]\n<данные>\n
//   kind = "device-data"  -> [device-data, size]\n<данные>\n  (people/catalog — аналогично)
struct SyncSendItem {
    std::string           kind;
    int                   month  = 0;     // yyyymm (event-tail)
    long long             offset = 0;     // смещение в кадре (event-tail)
    std::string           prepend;        // байты перед содержимым файла (заголовок хвоста)
    std::filesystem::path path;           // источник
    long long             fileFrom = 0;
    long long             fileLen  = 0;
    long long frameSize() const { return (long long)prepend.size() + fileLen; }
};

// Манифест справочников (для обмена «состоянием» в начале сессии).
struct ListManifest {
    FileState people, catalog, device;
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

    // --- синхронизация (файловая, инкрементная, потоковая) ---
    // Манифест наших справочников (для обмена в начале сессии).
    ListManifest listManifest() const;

    // Индекс по партнёру: sync/<peerDn>.jsonl — СОСТОЯНИЕ СОБЕСЕДНИКА: сколько
    // байт каждого нашего месячного файла у него уже есть. [yyyymm, offset].
    std::map<int, long long> loadSyncIndex(int peerDn) const;
    void saveSyncIndex(int peerDn, const std::map<int, long long>& off) const;

    // Начать/закончить сессию синхронизации с партнёром (peerDn — его номер у нас).
    void syncBegin(int peerDn);
    void syncEnd();

    // Что отправить партнёру (без данных — только план: путь/смещение/длина).
    // Решение принимается по СОСТОЯНИЮ СОБЕСЕДНИКА: справочники шлём, только если
    // наша версия отличается от того, что у партнёра (peer); хвосты — от offset,
    // который партнёр уже получил.
    std::vector<SyncSendItem> syncPlanOutgoing(const ListManifest& peer) const;

    // Потоковый приём блока: begin -> feed(блоки сети) -> finish. Применяется
    // сразу по мере поступления, без накопления всего блока в памяти.
    void syncRecvBegin(const std::string& kind, int month, bool replaceLists);
    void syncRecvFeed(const char* data, std::size_t n);
    void syncRecvFinish();

    // Удаление более поздних дубликатов (совпадение всех полей кроме служебных).
    int  syncDedup();

    // Зафиксировать состояние собеседника (после успешного обмена).
    void syncCommit(int peerDn);

    int  syncReceived() const { return sync_ ? sync_->received : 0; }

    // Действующий заголовок (схема) месяца — для дозаписи заголовка перед хвостом.
    std::string inEffectHeader(int yyyymm) const;

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

    // Потоковый приём одного блока: инкрементный разбор по мере поступления байт.
    struct RecvState {
        std::string kind;
        int month = 0;
        bool replaceLists = false;
        boost::json::stream_parser sp;
        bool atStart = true;
        Schema cur;
        bool headerReceived = false;
        std::vector<std::string>  people;        // накопление для replace/merge
        std::vector<CatalogEntry> catalog;
    };

    // Состояние активной сессии синхронизации (между syncBegin/syncEnd).
    struct SyncSession {
        int peerDn = 0;
        std::map<int, long long> offsets;        // СОСТОЯНИЕ СОБЕСЕДНИКА: yyyymm -> сколько наших байт у него
        std::map<int, int> dnMap;                // DN партнёра -> наш DN
        std::set<std::string> deleteKeys;        // дедуп строк удаления (лениво)
        bool deleteKeysLoaded = false;
        int received = 0;
        std::unique_ptr<RecvState> recv;         // текущий принимаемый блок
    };
    std::unique_ptr<SyncSession> sync_;
    void ensureDeleteKeysLoaded();
    void handleRecvValue(const boost::json::value& v);
};

} // namespace ha
