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
    std::string error;       // пусто если успех; "db_mismatch" / "disabled" / текст
    std::string peerDb;      // имя базы у партнёра (при db_mismatch)
    std::string peerPubkey;  // идентификатор устройства-партнёра
    std::string peerName;    // имя устройства-партнёра (пусто, если ещё не знаем)
    int sent = 0;            // передано записей
    int received = 0;        // принято записей
};

// Подтверждение нового (неизвестного) устройства: вернуть true чтобы разрешить.
using ConfirmFn = std::function<bool(const std::string& pubkey)>;
// Собеседник опознан (по ходу сеанса): имя устройства и его открытый ключ.
// Имя пустое, пока устройство неизвестно — тогда вызов повторится, когда
// придёт список устройств партнёра.
using PeerFn = std::function<void(const std::string& name,
                                  const std::string& pubkey)>;
// Завершение сессии: вызывается РОВНО один раз в потоке главного цикла (app.exec).
using DoneFn = std::function<void(const SyncResult& res)>;

// Синхронизация целиком асинхронная и работает в главном цикле событий Qt:
// отдельного потока нет, ожидания нет — результат приходит в DoneFn.
// Транспорт — QSslSocket (TLS с самоподписанными сертификатами обеих сторон).

// Сервер: слушает входящие подключения (роль показывающего QR).
// До получения верного кода принимаются ЛЮБЫЕ подключения (посторонний клиент
// или сканер портов не должен занимать сервер): каждое — лишь кандидат.
// Партнёром становится тот, кто первым прислал верный код; в этот момент
// слушатель и остальные подключения закрываются. Наверх (done) сообщается
// только результат этого сеанса — отвалившиеся кандидаты не видны.
class SyncServer {
public:
    explicit SyncServer(Store& store);
    ~SyncServer();
    // Занять свободный порт, сгенерировать код и начать приём. Возвращает
    // реквизиты сопряжения (для QR) сразу, результат сеанса придёт в done.
    PairInfo start(ConfirmFn confirm, DoneFn done, PeerFn onPeer = {});
    void     cancel();
    struct Impl;
private:
    std::unique_ptr<Impl> d_;
};

// Клиент: подключается по реквизитам (роль сканирующего QR).
class SyncClient {
public:
    explicit SyncClient(Store& store);
    ~SyncClient();
    void start(const PairInfo& info, ConfirmFn confirm, DoneFn done,
               PeerFn onPeer = {});
    void cancel();                              // прервать синхронизацию в любом месте
    struct Impl;
private:
    std::unique_ptr<Impl> d_;
};

} // namespace ha
