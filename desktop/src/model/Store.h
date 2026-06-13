#pragma once
#include "Types.h"
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <optional>

namespace ha {

// Дамп всех данных базы для обмена при синхронизации.
struct SyncDump {
    std::string db;
    std::vector<std::string>                       people;   // имена
    std::vector<CatalogEntry>                      catalog;
    std::vector<Device>                            devices;
    std::vector<std::pair<std::string,std::string>> events;  // (monthfile, raw jsonl line)
};

// Центральное хранилище: читает все файлы базы в память, отдаёт текущее
// (с учётом удалений) состояние и выполняет запись (события — только дозапись).
class Store {
public:
    explicit Store(std::filesystem::path root = {});

    void load();                       // прочитать config + текущую базу целиком

    // --- конфигурация / база ---
    const std::string& database() const { return db_; }
    int  deviceNo() const { return deviceNo_; }
    std::vector<std::string> databases() const;          // содержимое database.jsonl
    void switchDatabase(const std::string& name, bool create);

    // Размер шрифта приложения (pt; 0 — по умолчанию системы). Хранится в config.json.
    int  fontSize() const { return fontSize_; }
    void setFontSize(int pt);

    // --- доступ к данным (текущее видимое состояние) ---
    std::vector<Event>        events() const;            // без удалённых
    const std::vector<std::string>&  people()  const { return people_; }
    const std::vector<CatalogEntry>& catalog() const { return catalog_; }
    const std::vector<Device>&       devices() const { return devices_; }

    // Категория для наименования (по каталогу), если найдена.
    std::string categoryOf(const std::string& subject) const;

    // --- мутации ---
    Event addEvent(const std::string& event_datetime, const std::string& subject,
                   double cost, std::optional<std::string> people,
                   std::optional<std::string> volume);
    void  deleteEvent(const Event& e);
    Event editEvent(const Event& oldEv, const std::string& event_datetime,
                    const std::string& subject, double cost,
                    std::optional<std::string> people, std::optional<std::string> volume);

    void addPerson(const std::string& name);
    void removePerson(const std::string& name);
    void upsertCatalog(const CatalogEntry& e);           // объединение по категории

    // --- идентичность устройства ---
    void ensureIdentity();                               // ключ/сертификат + device_no
    std::string myPubkey() const { return myPubkey_; }
    std::filesystem::path certPath() const;
    std::filesystem::path keyPath() const;

    // --- синхронизация ---
    SyncDump dump() const;
    // Слить чужой дамп; вернуть число реально добавленных событий (принято K).
    int mergeDump(const SyncDump& d, std::set<std::string>* addedDeviceKeys = nullptr);
    bool knowsDevice(const std::string& pubkey) const;
    int  reserveDeviceNo(const std::string& pubkey, int preferredNo, const std::string& name);

    // Есть ли в базе данные: строки в любом из списков (события, люди, каталог),
    // для устройств — наличие устройств кроме текущего. Список баз не учитывается.
    bool hasData() const;
    int  maxDeviceNo() const;
    // Сменить собственный номер устройства (безопасно только без своих событий).
    void renumberSelf(int newNo);

    std::filesystem::path root() const { return root_; }

private:
    std::filesystem::path dbDir() const { return root_ / db_; }
    void loadConfig();
    void saveConfig();
    void loadDevices();
    void loadPeople();
    void loadCatalog();
    void loadEvents();
    void savePeople();
    void saveCatalog();
    void saveDevices();
    void appendEventLine(const std::string& monthFileName, const std::string& line);
    static std::string tokenForLine(const std::string& line); // для дедупликации

    std::filesystem::path root_;
    std::string db_ = "Основная";
    int deviceNo_ = 0;
    int fontSize_ = 0;
    std::string myPubkey_;

    std::vector<std::string>  people_;
    std::vector<CatalogEntry> catalog_;
    std::vector<Device>       devices_;

    // Сырые события: (monthfile, line) + индекс присутствия для дедупликации.
    std::vector<std::pair<std::string,std::string>> rawEvents_;
    std::set<std::string> tokens_;
    // Применённое состояние: ключ записи -> Event; удалённые ключи.
    std::map<std::string, Event> live_;
    std::set<std::string> deleted_;

    void rebuildState();
    int nextRecNo(const std::string& edit_datetime) const;
};

} // namespace ha
