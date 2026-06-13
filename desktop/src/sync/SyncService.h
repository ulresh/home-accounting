#pragma once
#include <string>
#include <functional>
#include <memory>

namespace ha {

class Store;

// Реквизиты сопряжения, передаются через QR / вручную.
struct PairInfo {
    std::string ip;
    int         port = 0;
    std::string code;   // 8-символьный код подключения
    std::string db;     // имя текущей базы

    std::string toJson() const;
    static PairInfo fromJson(const std::string& s);
};

struct SyncResult {
    bool        ok = false;
    std::string error;       // пусто если успех; "db_mismatch" / "rejected" / текст
    std::string peerDb;      // имя базы у партнёра (при db_mismatch)
    std::string peerPubkey;  // идентификатор устройства-партнёра
    int sent = 0;            // передано записей
    int received = 0;        // принято записей
};

// Подтверждение нового (неизвестного) устройства: вернуть true чтобы разрешить.
using ConfirmFn = std::function<bool(const std::string& pubkey)>;

// Сервер: слушает входящее подключение (роль показывающего QR).
class SyncServer {
public:
    explicit SyncServer(Store& store);
    ~SyncServer();
    PairInfo   listen();                 // занять свободный порт, сгенерировать код
    SyncResult wait(ConfirmFn confirm);  // блокирующе: принять и синхронизировать
    void       cancel();
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Клиент: подключается по реквизитам (роль сканирующего QR).
class SyncClient {
public:
    explicit SyncClient(Store& store);
    SyncResult connect(const PairInfo& info, ConfirmFn confirm);
private:
    Store& store_;
};

} // namespace ha
