// Синхронизация на Qt: QSslSocket + главный цикл событий (app.exec).
// Корутин и отдельных потоков нет — весь протокол выражен продолжениями
// (callback'ами): каждый шаг заканчивается вызовом Cont, который запускает
// следующий. Обмен по-прежнему потоковый: ни файлы, ни блоки не накапливаются
// в памяти, а cancel() рвёт соединение в любой точке.
#include "SyncService.h"
#include "Crypto.h"
#include "../model/Store.h"

#include <QSslSocket>
#include <QSslConfiguration>
#include <QSslCertificate>
#include <QSslError>
#include <QSslKey>
#include <QTcpServer>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QFile>
#include <QTimer>

#include <boost/json.hpp>

#include <unistd.h>
#include <random>
#include <stdexcept>
#include <memory>
#include <fstream>
#include <algorithm>
#include <functional>
#include <vector>
#include <deque>
#include <list>

using namespace std::literals::string_view_literals;
using namespace std::string_literals;
namespace json = boost::json;
namespace fs   = std::filesystem;

namespace ha {

// ---------- PairInfo ----------
std::string PairInfo::toJson() const {
    json::object o;
    o["ip"] = ip; o["port"] = port; o["code"] = code; o["db"] = db;
    return json::serialize(o);
}
PairInfo PairInfo::fromJson(const std::string& s) {
    PairInfo p;
    try {
        auto v = json::parse(s);
        auto& o = v.as_object();
        p.ip   = std::string(o.at("ip").as_string());
        p.port = (int)o.at("port").as_int64();
        p.code = std::string(o.at("code").as_string());
        if (auto* d = o.if_contains("db")) p.db = std::string(d->as_string());
    } catch (...) {}
    return p;
}

namespace {

using Cont = std::function<void()>;

// Сколько байт разрешаем держать в буфере сокета: выше — ждём bytesWritten.
constexpr qint64 kHighWater = 256 * 1024;
// Сколько ждать вытеснения хвоста после завершения сессии.
constexpr int kLingerMs = 10000;

std::string randomCode(int n) {
    static const char* A = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 30);
    std::string s;
    for (int i = 0; i < n; ++i) s += A[dist(rd)];
    return s;
}

std::string localIPv4() {
    for (const QHostAddress& a : QNetworkInterface::allAddresses()) {
        if (a.isLoopback()) continue;
        if (a.protocol() != QAbstractSocket::IPv4Protocol) continue;
        return a.toString().toStdString();
    }
    return "127.0.0.1";
}

QString qstr(const fs::path& p) { return QString::fromStdString(p.string()); }

// Наша TLS-личность: самоподписанный сертификат + ключ EC P-256.
// Чужой сертификат не проверяем (QueryPeer) — доверие даёт код сопряжения,
// а идентификатором устройства служит открытый ключ из сертификата.
QSslConfiguration makeConfig(Store& store) {
    QSslConfiguration c = QSslConfiguration::defaultConfiguration();
    QFile cf(qstr(store.certPath()));
    if (!cf.open(QIODevice::ReadOnly))
        throw std::runtime_error("open cert: "s + store.certPath().string());
    QSslCertificate cert(&cf, QSsl::Pem);
    if (cert.isNull()) throw std::runtime_error("parse cert"s);
    QFile kf(qstr(store.keyPath()));
    if (!kf.open(QIODevice::ReadOnly))
        throw std::runtime_error("open key: "s + store.keyPath().string());
    QSslKey key(&kf, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
    if (key.isNull()) throw std::runtime_error("parse key"s);
    c.setLocalCertificate(cert);
    c.setPrivateKey(key);
    c.setPeerVerifyMode(QSslSocket::QueryPeer);   // сертификат просим, но не поверяем
    c.setProtocol(QSsl::TlsV1_2OrLater);
    return c;
}

// Открытый ключ партнёра (base64 DER SubjectPublicKeyInfo) — идентификатор устройства.
std::string peerPubkeyOf(QSslSocket* s) {
    QByteArray pem = s->peerCertificate().toPem();
    if (pem.isEmpty()) return {};
    return crypto::publicKeyFromCertPem(std::string(pem.constData(), pem.size()));
}

// ---- разбор диапазона месячного файла (без накопления в памяти) ----
// Нужен, чтобы ЗАРАНЕЕ узнать: начинается ли отправляемый кусок с header'а
// (иначе придётся предпослать действующий у партнёра) и какая схема окажется
// действующей в конце куска.
struct RangeScan {
    Schema lastHeader;
    bool   firstIsHeader = false;
};

    // TODO +++ проверить, пока просто пропустил
RangeScan scanRange(const fs::path& path, uint64_t from, uint64_t to) {
    RangeScan r;
    if (to <= from) return r;
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("bad data"s);
    if (from) in.seekg((std::streamoff)from);
    uint64_t rest = to - from;
    MallocPtr<char> block(block_size());
    json::stream_parser sp;
    bool first = true;
    while (rest && in) {
        std::size_t want = (std::size_t)std::min<uint64_t>(rest, block_size());
        in.read(block.get(), (std::streamsize)want);
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        rest -= (uint64_t)got;
        const char* p = block.get();
        std::size_t n = (std::size_t)got;
        while (n) {
            auto consumed = sp.write_some(p, n);
            p += consumed; n -= consumed;
            if (!sp.done()) break;
            auto v = sp.release();
            sp.reset();
            if (v.is_object()) {
                auto& o = v.as_object();
                if (o.if_contains("header")) {
                    r.lastHeader = Schema(o);
                    if (first) r.firstIsHeader = true;
                }
            }
            first = false;
        }
    }
    return r;
}

// ============================================================
//                          Session
// ============================================================
// Одна сессия синхронизации поверх одного QSslSocket. Роли (сервер/клиент)
// отличаются только точкой входа: serverGo()/clientGo().
struct Session : std::enable_shared_from_this<Session> {
    explicit Session(Store& s) : store(s), wbuf(block_size()) {}
    ~Session() { detach(true); }

    Store&      store;
    SyncResult  res;
    ConfirmFn   confirm;
    DoneFn      done;
    QSslSocket* sock = nullptr;
    std::vector<QMetaObject::Connection> conns;
    bool cancelled = false;
    bool completed = false;

    // ---------- приём ----------
    QByteArray rbuf;
    qsizetype rbuf_pos = 0;
    enum Pending { PendNone, PendLine, PendBlock };
    Pending pending = PendNone;
    Cont lineCb;
    std::size_t blockLeft = 0;
    json::stream_parser sp;
    std::function<void(const json::value&)> blockSink;
    Cont blockCb;
    bool inPump = false;

    // ---------- отправка ----------
    // Очередь маленькая: литеральные строки протокола + ссылки на диапазоны
    // файлов; сами файлы читаются блоками прямо в сокет.
    struct Out { std::string data; fs::path path; uint64_t from = 0, to = 0; };
    std::deque<Out> outq;
    std::ifstream outFile;
    uint64_t outPos = 0;
    MallocPtr<char> wbuf;
    Cont writeCb;
    bool inWrite = false;

    // ---------- состояние протокола ----------
    std::string peer;          // открытый ключ партнёра
    int  peerDeviceNo = 0;
    bool storeEmpty = false;
    json::value  av;           // текущая команда (живёт, пока её читают)
    json::array* ao = nullptr;
    std::string  cmd;
    SyncIndex idx, idxCur, idxNew;

    // -------------------------------------------------- подключение сокета
    // Любой обработчик может (через продолжения) завершить сеанс, а вместе с ним
    // снять последнюю ссылку на нас — например, когда кандидат вычёркивается из
    // списка ожидающих. Поэтому каждая точка входа из Qt держит self: объект
    // доживёт до возврата в цикл событий.
    void attach(QSslSocket* s, Cont onReady) {
        sock = s;
	conns.reserve(6);
        conns.push_back(QObject::connect(s, &QSslSocket::encrypted, s,
            [this, onReady] {
                auto self = shared_from_this();
                if (completed) return;
                peer = peerPubkeyOf(sock);
                if (peer.empty()) { fail("no peer certificate"s); return; }
                res.peerPubkey = peer;
                call([&] { onReady(); });
            }));
        conns.push_back(QObject::connect(s, &QSslSocket::sslErrors, s,
            [s](const QList<QSslError>&) { s->ignoreSslErrors(); }));
        conns.push_back(QObject::connect(s, &QIODevice::readyRead, s,
            [this] { onReadyRead(); }));
        conns.push_back(QObject::connect(s, &QIODevice::bytesWritten, s,
            [this](qint64) { pumpWrite(); }));
        conns.push_back(QObject::connect(s, &QAbstractSocket::disconnected, s,
            [this] {
                auto self = shared_from_this();
                onReadyRead();               // дочитать то, что успело прийти
                fail("connection closed"s);
            }));
        conns.push_back(QObject::connect(s, &QAbstractSocket::errorOccurred, s,
            [this](QAbstractSocket::SocketError) {
                auto self = shared_from_this();
                fail(sock ? sock->errorString().toStdString() : "socket error"s);
            }));
    }

    // Отцепить сокет. hard (отмена/деструктор) — рвать немедленно, иначе дать
    // вытеснить хвост (последнее ["done"]) и закрыться штатно.
    void detach(bool hard) {
        for (auto& c : conns) QObject::disconnect(c);
        conns.clear();
        QSslSocket* s = sock;
        sock = nullptr;
        if (!s) return;
        if (hard) { s->abort(); s->deleteLater(); return; }
        if (s->state() == QAbstractSocket::UnconnectedState) {
            s->deleteLater();               // уже закрыт — ждать нечего
            return;
        }
        // Дальше сокет живёт сам: удалится по первому из событий, а если
        // партнёр не закрывает соединение — по сторожевому таймеру.
        // Подписаться надо ДО disconnectFromHost(): при пустом буфере он
        // закрывает сразу и испускает disconnected синхронно.
        QObject::connect(s, &QAbstractSocket::disconnected, s, &QObject::deleteLater);
        QObject::connect(s, &QAbstractSocket::errorOccurred, s,
                         [s](QAbstractSocket::SocketError) { s->deleteLater(); });
        QTimer::singleShot(kLingerMs, s, [s] { s->deleteLater(); });
        s->disconnectFromHost();
    }

    // -------------------------------------------------- завершение
    void finish() {
        if (completed) return;
        completed = true;
        auto self = shared_from_this();      // переживём удаление владельца
        if (!res.ok && res.error.empty() && cancelled) res.error = "cancelled";
        // Не отмена — дать хвосту (последнему ["done"] / ["error"]) уйти.
        detach(cancelled);
        outq.clear();
        if (outFile.is_open()) outFile.close();
        lineCb = nullptr; blockCb = nullptr; blockSink = nullptr; writeCb = nullptr;
        auto cb = std::move(done);
        done = nullptr;
        if (cb) cb(res);
    }

    void fail(const std::string& e) {
        if (completed) return;
        if (res.error.empty()) res.error = cancelled ? "cancelled"s : e;
        finish();
    }

    void cancel() {
        if (completed) return;
        auto self = shared_from_this();
        cancelled = true;
        fail("cancelled"s);
    }

    // Выполнить продолжение, поймав исключение разбора/ввода-вывода.
    // false — сессия завершилась (ошибкой или успехом), продолжать нельзя.
    template<class F> bool call(F&& f) {
        try { f(); }
        catch (const std::exception& e) { fail(e.what()); return false; }
        catch (...) { fail("unknown error"s); return false; }
        return !completed;
    }

    // -------------------------------------------------- чтение
    void readLine(Cont cb) {
        lineCb = std::move(cb);
        pending = PendLine;
        pump();
    }

    // Ровно count байт JSON-значений, затем разделитель '\n'.
    void readBlock(std::size_t count,
                   std::function<void(const json::value&)> sink, Cont cb) {
        sp.reset();
        blockLeft = count;
        blockSink = std::move(sink);
        blockCb = std::move(cb);
        pending = PendBlock;
        pump();
    }

    void onReadyRead() {
        if (completed || !sock) return;
        QByteArray ba = sock->readAll();
        if(!ba.isEmpty()) {
	    if(rbuf_pos == rbuf.size()) { rbuf = ba; rbuf_pos = 0; }
	    else if(!rbuf_pos) rbuf.append(ba);
	    else { rbuf = rbuf.sliced(rbuf_pos) + ba; rbuf_pos = 0; }
	}
        pump();
    }

    // Скормить парсеру то, что уже пришло. true — блок дочитан целиком.
    bool feedLine() {
        if(rbuf_pos < rbuf.size()) {
	    auto n = rbuf.size() - rbuf_pos;
            const char* p = rbuf.constData() + rbuf_pos;
	    auto consumed = sp.write_some(p, n);
	    rbuf_pos += consumed;
	    if (!sp.done()) return false;      // весь остаток скормлен
	    av = sp.release();
	    sp.reset();
	    return true;
	}
	return false;
    }

    // Скормить парсеру то, что уже пришло. true — блок дочитан целиком.
    bool feedBlock() {
        while (blockLeft && (rbuf_pos < rbuf.size())) {
	    auto n = rbuf.size() - rbuf_pos;
	    if(n > blockLeft) n = blockLeft;
            const char* p = rbuf.constData() + rbuf_pos;
            std::size_t left = n;
            while (left) {
                auto consumed = sp.write_some(p, left);
                p += consumed; left -= consumed;
                if (!sp.done()) break;      // весь остаток скормлен
                auto v = sp.release();
                sp.reset();
                blockSink(v);
            }
            if (left) throw std::runtime_error("bad protocol"s);
	    rbuf_pos += n;
            blockLeft -= n;
        }
        if(blockLeft) return false;
        if(rbuf_pos == rbuf.size()) return false;
        if(rbuf[rbuf_pos] != '\n') throw std::runtime_error("bad protocol"s);
	++rbuf_pos;
        return true;
    }

    void pump() {
        if (inPump || completed) return;
        auto self = shared_from_this();      // цикл ниже переживёт завершение сеанса
        inPump = true;
        for (;;) {
            if (completed) break;
            if (pending == PendLine) {
                bool ready = false;
                if (!call([&] { ready = feedLine(); })) break;
                if (!ready) break;           // ждём данных из сокета
                pending = PendNone;
		auto cb = std::move(lineCb);
		lineCb = nullptr;
                if (!call([&] { cb(); })) break;
                continue;                    // продолжение могло заказать ещё чтение
            }
            if (pending == PendBlock) {
                bool ready = false;
                if (!call([&] { ready = feedBlock(); })) break;
                if (!ready) break;           // ждём данных из сокета
                pending = PendNone;
                auto cb = std::move(blockCb);
                blockCb = nullptr;
                blockSink = nullptr;
                if (!call([&] { cb(); })) break;
                continue;
            }
            break;
        }
        inPump = false;
    }

    // -------------------------------------------------- запись
    void send(std::string data) {
        if (data.empty()) return;
        outq.push_back(Out{std::move(data), {}, 0, 0});
    }
    void sendFile(const fs::path& p, uint64_t from, uint64_t to) {
        if (to > from) outq.push_back(Out{{}, p, from, to});
    }
    void flush(Cont cb) {
        writeCb = std::move(cb);
        pumpWrite();
    }

    void pumpWrite() {
        if (completed || !sock || inWrite) return;
        auto self = shared_from_this();
        inWrite = true;
        while (!outq.empty()) {
            if (sock->bytesToWrite() >= kHighWater) { inWrite = false; return; }
            Out& o = outq.front();
            if (o.path.empty()) {
                sock->write(o.data.data(), (qint64)o.data.size());
                outq.pop_front();
                continue;
            }
            if (!outFile.is_open()) {
                outFile.open(o.path, std::ios::binary);
                if (!outFile) { inWrite = false; fail("file error"s); return; }
                if (o.from) outFile.seekg((std::streamoff)o.from);
                outPos = o.from;
            }
            std::size_t want = (std::size_t)std::min<uint64_t>(o.to - outPos,
                                                               block_size());
            outFile.read(wbuf.get(), (std::streamsize)want);
            std::streamsize got = outFile.gcount();
            if (got <= 0) { inWrite = false; fail("bad data"s); return; }
            sock->write(wbuf.get(), (qint64)got);
            outPos += (uint64_t)got;
            if (outPos >= o.to) { outFile.close(); outq.pop_front(); }
        }
        inWrite = false;
        if (writeCb) {
            auto cb = std::move(writeCb);
            writeCb = nullptr;
            call([&] { cb(); });
        }
    }

    // -------------------------------------------------- команды протокола
    void cmdNext(Cont cb) {
        readLine([this, cb]() {
            ao = &av.as_array();
            cmd = std::string(ao->at(0).as_string());
            cb();
        });
    }

    // ---- постановка в очередь целых файлов/диапазонов ----
    void queueFullFile(std::string_view name) {
        auto path = store.dbDir() / (std::string(name) + ".jsonl"s);
        uint64_t size = fs::file_size(path);
        json::array h;
        h.emplace_back(name);
        h.emplace_back(size);
        send(json::serialize(h) + "\n"s);
        sendFile(path, 0, size);
        send("\n"s);
    }

    static std::string eventHead(int yyyymm, uint64_t size) {
        json::array h;
        h.emplace_back("event"sv);
        h.emplace_back(yyyymm);
        h.emplace_back(size);
        return json::serialize(h) + "\n"s;
    }

    // TODO +++ review mark, double file read = scanRange & sendFile
    // TODO +++ разобраться с idx(idxPeer) idxCur idxNew - Клод очень криво перенёс их из прошлого алгоритма
    MonthSyncData queueFullEventFile(int yyyymm, const fs::path& path) {
        uint64_t size = fs::file_size(path);
        auto sc = scanRange(path, 0, size);
        send(eventHead(yyyymm, size));
        sendFile(path, 0, size);
        send("\n"s);
        return MonthSyncData{(int64_t)size, sc.lastHeader};
    }

    // Отдать только начало файла — то, что партнёр ещё не видел, но что уже
    // пришло от него самого, отдавать обратно смысла нет.
    void queueTopEventFile(int yyyymm, const fs::path& path,
                           const MonthSyncData& cur) {
        if (fs::file_size(path) < (uint64_t)cur.offset)
            throw std::runtime_error("bad data"s);
        send(eventHead(yyyymm, (uint64_t)cur.offset));
        sendFile(path, 0, (uint64_t)cur.offset);
        send("\n"s);
    }

    // Хвост файла начиная с того, что у партнёра уже есть. Если хвост не
    // начинается с header'а — предпосылаем действующую у партнёра схему.
    MonthSyncData queueTailEventFile(int yyyymm, const fs::path& path,
                                 const MonthSyncData& peerData) {
        MonthSyncData cur;
        cur.offset = (int64_t)fs::file_size(path);
        if (cur.offset <= peerData.offset) return peerData;
        auto sc = scanRange(path, (uint64_t)peerData.offset, (uint64_t)cur.offset);
        uint64_t rest = (uint64_t)(cur.offset - peerData.offset);
        if (sc.firstIsHeader) send(eventHead(yyyymm, rest));
        else {
            std::string ch = peerData.header.serialize() + "\n"s;
            send(eventHead(yyyymm, rest + ch.size()));
            send(ch);
        }
        sendFile(path, (uint64_t)peerData.offset, (uint64_t)cur.offset);
        send("\n"s);
        cur.header = sc.lastHeader ? sc.lastHeader : peerData.header;
        return cur;
    }

    // Середина файла: от того, что у партнёра есть, до того, что от него же
    // сейчас и пришло.
    void queueMiddleEventFile(int yyyymm, const fs::path& path,
                              const MonthSyncData& peerData,
                              const MonthSyncData& cur) {
        if (fs::file_size(path) < (uint64_t)cur.offset)
            throw std::runtime_error("bad data"s);
        auto sc = scanRange(path, (uint64_t)peerData.offset, (uint64_t)cur.offset);
        uint64_t rest = (uint64_t)(cur.offset - peerData.offset);
        if (sc.firstIsHeader) send(eventHead(yyyymm, rest));
        else {
            std::string ch = peerData.header.serialize() + "\n"s;
            send(eventHead(yyyymm, rest + ch.size()));
            send(ch);
        }
        sendFile(path, (uint64_t)peerData.offset, (uint64_t)cur.offset);
        send("\n"s);
    }

    // ============================================================
    //     Отдать всё партнёру, у которого ещё ничего нет
    // ============================================================
    void sendAllToEmptyPeer(bool recv_follow, Cont next) {
        peerDeviceNo = store.addDevice(peer);
        queueFullFile("device"sv);
        ++res.sent;
        if (!store.people_.empty() || !store.people_delete.empty()) {
            queueFullFile("people"sv);
            ++res.sent;
        }
        if (!store.catalog_.empty()) {
            queueFullFile("catalog"sv);
            ++res.sent;
        }
        store.listManifest(idx);
        for (auto& [yyyymm, path] : store.enumerateMonths()) {
            idx.events[yyyymm] = queueFullEventFile(yyyymm, path);
            ++res.sent;
        }
        send(R"(["end"])" "\n"s);
        if (recv_follow) flush(next);
        else flush([this, next] {
            cmdNext([this, next] {
                if (cmd != "done"sv) { res.error = "bad protocol"sv; next(); return; }
                store.saveSyncIndex(peerDeviceNo, idx);
                res.ok = true;
                next();
            });
        });
    }

    // ============================================================
    //     Принять всё, когда у нас пусто (первое сопряжение)
    // ============================================================
    // Стадии: 0 device (обязательна), 1 people, 2 catalog, 3 event*, затем end.
    int rwStage = 0;
    Cont rwNext;
    std::vector<Device> newDevices;
    int newDeviceNo = 0;
    Store::People newPeople, newPeopleDelete, *pPeople = nullptr;
    std::unique_ptr<CatalogLoader> catLoader;
    std::unique_ptr<MonthEvents> monthEv;
    std::unique_ptr<std::ofstream> monthOut;
    int monthYm = 0;

    void recvAllWhenEmpty(Cont next) {
        rwNext = std::move(next);
        rwStage = 0;
        idx = SyncIndex();
        rwStep();
    }

    void rwStep() {
        auto again = [this] { cmdNext([this] { rwStep(); }); };
        if (rwStage == 0) {
            rwStage = 1;
            if (cmd != "device"sv) { res.error = "bad protocol"sv; rwNext(); return; }
            newDevices.clear();
            newDeviceNo = 0;
            peerDeviceNo = 0;
            readBlock((std::size_t)ao->at(1).as_int64(),
                [this](const json::value& v) {
                    newDevices.push_back(Device(v, false));
                    if (newDevices.back().pubkey == store.myPubkey_) {
                        if (newDeviceNo) throw std::runtime_error("bad protocol"s);
                        newDeviceNo = newDevices.back().no;
                        newDevices.back().name = "this"s;
                    }
                    else if (newDevices.back().pubkey == peer) {
                        if (peerDeviceNo) throw std::runtime_error("bad protocol"s);
                        peerDeviceNo = newDevices.back().no;
                    }
                    ++res.received;
                },
                [this, again] {
                    if (!newDeviceNo || !peerDeviceNo) {
                        res.error = "bad protocol"sv;
                        rwNext();
                        return;
                    }
                    store.devices_.swap(newDevices);
                    store.deviceNo_ = newDeviceNo;
                    store.saveDevices(&idx.device);
                    store.saveConfig();
                    again();
                });
            return;
        }
        if (rwStage == 1) {
            rwStage = 2;
            if (cmd == "people"sv) {
                newPeople.clear();
                newPeopleDelete.clear();
                pPeople = &newPeople;
                readBlock((std::size_t)ao->at(1).as_int64(),
                    [this](const json::value& v) {
                        if (v.is_object()) {
                            for (auto& [value, time] : v.as_object())
                                if (time.is_string())
                                    pPeople->emplace_hint(pPeople->end(),
                                        std::string(value),
                                        std::string(time.as_string()));
                        }
                        else if (v.is_array()) {
                            auto a = v.as_array();
                            if (a.size() == 1 && a[0].is_string() &&
                                a[0].as_string() == "delete"s)
                                pPeople = &newPeopleDelete;
                        }
                        ++res.received;
                    },
                    [this, again] {
                        store.people_.swap(newPeople);
                        store.people_delete.swap(newPeopleDelete);
                        store.savePeople(&idx.people);
                        again();
                    });
                return;
            }
            idx.people = store.stateOf(store.pPeople());
        }
        if (rwStage == 2) {
            rwStage = 3;
            if (cmd == "catalog"sv) {
                catLoader = std::make_unique<CatalogLoader>();
                readBlock((std::size_t)ao->at(1).as_int64(),
                    [this](const json::value& v) {
                        catLoader->add(v);
                        ++res.received;
                    },
                    [this, again] {
                        store.catalog_.swap(catLoader->catalog_);
                        store.catalog_delete.swap(catLoader->catalog_delete);
                        catLoader.reset();
                        store.saveCatalog(&idx.catalog);
                        again();
                    });
                return;
            }
            idx.catalog = store.stateOf(store.pCatalog());
        }
        if (rwStage == 3 && cmd == "event"sv) {
            monthYm = (int)ao->at(1).as_int64();
            auto p = store.monthPath(monthYm);
            if (p.has_parent_path()) fs::create_directories(p.parent_path());
            monthEv = std::make_unique<MonthEvents>(store);
            monthOut = std::make_unique<std::ofstream>(p, std::ios::binary);
            if (!*monthOut) { res.error = "file error"sv; rwNext(); return; }
            readBlock((std::size_t)ao->at(2).as_int64(),
                [this](const json::value& v) {
                    *monthOut << json::serialize(v) << std::endl;
                    monthEv->add(v);
                    ++res.received;
                },
                [this, again] {
                    monthOut.reset();
                    monthEv->commit(monthYm);
                    idx.events[monthYm] = {
                        (int64_t)fs::file_size(store.monthPath(monthYm)),
                        monthEv->header };
                    monthEv.reset();
                    again();
                });
            return;
        }
        if (cmd != "end"sv) { res.error = "bad protocol"sv; rwNext(); return; }
        send(R"(["done"])" "\n"s);
        flush([this] {
            store.saveSyncIndex(peerDeviceNo, idx);
            res.ok = true;
            rwNext();
        });
    }

    // ============================================================
    //     Отдать приращение
    // ============================================================
    void sendAllIncrement(SyncIndex* idxPeer, SyncIndex* pCur, SyncIndex* pNew,
                          Cont next) {
        // В pNew записать всё, что не попало в pCur.
        // Туда не попадут те файлы, для которых у собеседника нет изменений.
        FileState tmp, *cur;
        if (pCur) cur = &pCur->device; else {
            tmp = store.stateOf(store.pDevice());
            cur = &tmp;
        }
        if (idxPeer->device != *cur) {
            queueFullFile("device"sv);
            ++res.sent;
            if (!pCur) idxPeer->device = *cur;
        }
        if (pCur) cur = &pCur->people; else {
            tmp = store.stateOf(store.pPeople());
            cur = &tmp;
        }
        if (idxPeer->people != *cur) {
            queueFullFile("people"sv);
            ++res.sent;
            if (!pCur) idxPeer->people = *cur;
        }
        if (pCur) cur = &pCur->catalog; else {
            tmp = store.stateOf(store.pCatalog());
            cur = &tmp;
        }
        if (idxPeer->catalog != *cur) {
            queueFullFile("catalog"sv);
            ++res.sent;
            if (!pCur) idxPeer->catalog = *cur;
        }
        for (auto& [yyyymm, path] : store.enumerateMonths()) {
            auto pd = idxPeer->events.lower_bound(yyyymm);
            if (pd == idxPeer->events.end() || pd->first != yyyymm) {
                // раньше ничего не передавали
                if (pCur) {
                    auto c = pCur->events.find(yyyymm);
                    if (c == pCur->events.end())
                        // сейчас ничего не пришло
                        pNew->events[yyyymm] = queueFullEventFile(yyyymm, path);
                    else if (c->second.offset)
                        // сейчас пришло то, что не имеет смысла отдавать обратно
                        queueTopEventFile(yyyymm, path, c->second);
                }
                else idxPeer->events.emplace_hint(pd, yyyymm,
                        queueFullEventFile(yyyymm, path));
            }
            else if (pCur) {
                auto c = pCur->events.find(yyyymm);
                if (c == pCur->events.end())
                    pNew->events[yyyymm] =
                        queueTailEventFile(yyyymm, path, pd->second);
                else if (c->second.offset > pd->second.offset)
                    queueMiddleEventFile(yyyymm, path, pd->second, c->second);
            }
            else pd->second = queueTailEventFile(yyyymm, path, pd->second);
            ++res.sent;
        }
        send(R"(["end"])" "\n"s);
        flush(next);
    }

    // ============================================================
    //     Принять приращение
    // ============================================================
    int riStage = 0;
    SyncIndex* riCur = nullptr;
    SyncIndex* riNew = nullptr;
    std::function<void(bool)> riNext;
    std::unique_ptr<std::ofstream> devOut;
    std::list<Device> reno;
    bool peopleDelete = false;
    std::unique_ptr<CatalogIncrementLoader> catInc;
    MonthDeletions mdels;
    Schema monthHeader;
    const std::map<int, int>* dnMap = nullptr;

    void recvAllIncrement(SyncIndex* cur, SyncIndex* nw,
                          std::function<void(bool)> next) {
        riCur = cur; riNew = nw; riNext = std::move(next);
        riStage = 0;
        riStep();
    }

    void riStep() {
        auto again = [this] { cmdNext([this] { riStep(); }); };
        if (riStage == 0) {
            riStage = 1;
            if (cmd == "device"sv) {
                devOut.reset();
                reno.clear();
                readBlock((std::size_t)ao->at(1).as_int64(),
                    [this](const json::value& v) {
                        Device n(v, false);
                        bool busyno = false;
                        for (auto& d : store.devices_)
                            if (d.pubkey == n.pubkey) {
                                riNew->dnMap[n.no] = d.no;
                                return;
                            }
                            else if (d.no == n.no) busyno = true;
                        if (busyno) reno.push_back(std::move(n));
                        else {
                            store.addDevice(devOut, n.no, n.pubkey);
                            riNew->dnMap[n.no] = n.no;
                        }
                    },
                    [this, again] {
                        if (!reno.empty()) {
                            auto m = store.maxDeviceNo();
                            for (auto& n : reno) {
                                if (m == std::numeric_limits<int>::max())
                                    throw std::runtime_error("too big device no"s);
                                store.addDevice(devOut, ++m, n.pubkey);
                                riNew->dnMap[n.no] = m;
                            }
                            reno.clear();
                        }
                        devOut.reset();          // == store.saveDevices();
                        if (!peerDeviceNo) peerDeviceNo = store.knowsDevice(peer);
                        if (!peerDeviceNo) {
                            res.error = "bad protocol"sv;
                            riNext(false);
                            return;
                        }
                        riNew->device = store.stateOf(store.pDevice());
                        again();
                    });
                return;
            }
            if (!peerDeviceNo) { res.error = "bad protocol"sv; riNext(false); return; }
            if (riCur) {
                riNew->device = riCur->device;
                riNew->dnMap = riCur->dnMap;
            }
            else riNew->device = store.stateOf(store.pDevice());
        }
        if (riStage == 1) {
            riStage = 2;
            if (cmd == "people"sv) {
                peopleDelete = false;
                readBlock((std::size_t)ao->at(1).as_int64(),
                    [this](const json::value& v) {
                        if (v.is_object()) {
                            for (auto& [value, time] : v.as_object())
                                if (time.is_string()) {
                                    ++res.received;
                                    std::string nm(value), t(time.as_string());
                                    if (peopleDelete) {
                                        auto a = store.people_.find(nm);
                                        if (a == store.people_.end()) ;
                                        else if (a->second >= t) return;
                                        else store.people_.erase(a);
                                        auto& d = store.people_delete[nm];
                                        if (d < t) d = t;
                                    }
                                    else {
                                        auto d = store.people_delete.find(nm);
                                        if (d == store.people_delete.end()) ;
                                        else if (d->second > t) return;
                                        else store.people_delete.erase(d);
                                        auto& a = store.people_[nm];
                                        if (a < t) a = t;
                                    }
                                }
                        }
                        else if (v.is_array()) {
                            auto a = v.as_array();
                            if (a.size() == 1 && a[0].is_string() &&
                                a[0].as_string() == "delete"s)
                                peopleDelete = true;
                        }
                    },
                    [this, again] {
                        store.savePeople();
                        riNew->people = store.stateOf(store.pPeople());
                        again();
                    });
                return;
            }
            riNew->people = riCur ? riCur->people : store.stateOf(store.pPeople());
        }
        if (riStage == 2) {
            riStage = 3;
            if (cmd == "catalog"sv) {
                catInc = std::make_unique<CatalogIncrementLoader>(store);
                readBlock((std::size_t)ao->at(1).as_int64(),
                    [this](const json::value& v) {
                        catInc->add(v);
                        ++res.received;
                    },
                    [this, again] {
                        catInc.reset();
                        store.saveCatalog();
                        riNew->catalog = store.stateOf(store.pCatalog());
                        again();
                    });
                return;
            }
            riNew->catalog = riCur ? riCur->catalog : store.stateOf(store.pCatalog());
        }
        if (riStage == 3 && cmd == "event"sv) {
            dnMap = riNew->dnMap.empty() ? nullptr : &riNew->dnMap;
            monthYm = (int)ao->at(1).as_int64();
            auto path = store.monthPath(monthYm);
            if (path.has_parent_path()) fs::create_directories(path.parent_path());
            monthOut = std::make_unique<std::ofstream>(
                    path, std::ios::binary | std::ios::app);
            if (!*monthOut) { res.error = "file error"sv; riNext(false); return; }
            mdels.ops.clear();
            monthHeader = Schema();
            if (auto pos = monthOut->tellp()) {
                if (riCur)
                    // Заголовок в riCur не используется
                    riCur->events[monthYm].offset = pos;
                mdels.read(path);
            }
            readBlock((std::size_t)ao->at(2).as_int64(),
                [this](const json::value& v) { riEvent(v); },
                [this, again] {
                    if (monthHeader) store.checkCanonical(monthYm, monthHeader);
                    riNew->events[monthYm].offset = monthOut->tellp();
                    monthOut.reset();
                    again();
                });
            return;
        }
        // riCur -> riNew для отсутствующих на приёме не нужно:
        // для клиента riNew заполнит sendAllIncrement,
        // для сервера riCur==null, а riNew уже заполнен одним из send*
        if (cmd != "end"sv) { res.error = "bad protocol"sv; riNext(false); return; }
        riNext(true);
    }

    // Одна принятая строка месячного файла.
    void riEvent(const json::value& v) {
        ++res.received;
        std::ofstream& out = *monthOut;
        if (v.is_object()) {
            auto& o = v.as_object();
            if (o.if_contains("header")) {
                out << json::serialize(v) << std::endl;
                monthHeader = Schema(o);
            }
            else if (!monthHeader) return;
            else if (auto* del = o.if_contains("delete")) {
                auto* ths = o.if_contains("this");
                if (!ths) return;
                RecRefDel d = Store::parseRefDel(del->as_array(),
                                                 monthHeader.reference, dnMap);
                if (d.dev_no == -1) return;
                RecRef t = Store::parseRef(ths->as_array(),
                                           monthHeader.reference, dnMap);
                if (t.dev_no == -1 || t.dev_no == store.deviceNo()) return;
                RecRefDel u;
                if (auto* upd = o.if_contains("update")) {
                    u = Store::parseRefDel(upd->as_array(),
                                           monthHeader.reference, dnMap);
                    if (u.dev_no == -1) return;
                }
                MonthDeletions::Op op{std::move(d), std::move(t), std::move(u)};
                if (!mdels.ops.contains(op)) {
                    out << json::serialize(withOurDevNoDel(v, monthHeader, dnMap))
                        << std::endl;
                    // Искать по op.del: d уже перемещён в op и обнулён.
                    auto p = store.events_.find(&op.del);
                    if (p != store.events_.end()) store.events_.erase(p);
                }
            }
            return;
        }
        if (!monthHeader) return;
        if (!v.is_array()) return;
        Event* ep;
        std::shared_ptr<Event> eh(ep = Store::parseEventArray(
                v.as_array(), monthHeader, dnMap));
        if (ep->dev_no == -1 || ep->dev_no == store.deviceNo() ||
            mdels.ops.contains(*ep)) return;
        if (store.events_.empty()) store.events_.insert(eh);
        else {
            auto p = store.events_.lower_bound(eh);
            if (p != store.events_.end() && p->get()->eq_edit(*ep)) return;
            /* TODO +++ до решения вопросов с размещением delete адекватней будет исключить весь блок
            for(auto a = p, b = store.events_.begin(); a != b;) {
                --a;
                if(a->get()->event_datetime != ep->event_datetime) break;
                if(a->get()->eq_data(*ep)) return; // TODO +++ 1. удалить у собеседника, иначе он удалит наше событие. А если событие добавлено уже после синхронизации, то его удалять не стоит - весь вопрос в том, как это определить. 2. (Под вопросом) Всё таки записать в файл оба - событие и удаление?
            }
            for(auto a = p, e = store.events_.end();
                a != e && a->get()->event_datetime == ep->event_datetime;
                ++a) if(a->get()->eq_data(*ep)) return;
            */
            store.events_.insert(p, eh);
        }
        out << json::serialize(withOurDevNo(v, monthHeader, dnMap)) << std::endl;
    }

    // Присланная строка записывается «как получили»: состав и порядок полей —
    // в том числе неизвестных нам колонок — остаются прежними, меняется ТОЛЬКО
    // значение dev_no, потому что оно приходит в пространстве DN собеседника.
    static void mapDevNo(json::array& a, const std::vector<std::string>& fields,
                         const std::map<int, int>* dn) {
        for (std::size_t i = 0; i < fields.size() && i < a.size(); ++i)
            if (fields[i] == "dev_no"sv) {
                // Строки с DN вне карты сюда не доходят (отсеяны по dev_no == -1).
                a[i] = dn->at(jsonAsDevNo(a[i]));
                return;
            }
    }
    static json::value withOurDevNo(const json::value& v, const Schema& header,
                                    const std::map<int, int>* dn) {
        if (!dn) return v;
        json::value out = v;
        mapDevNo(out.as_array(), header.columns, dn);
        return out;
    }
    static json::value withOurDevNoDel(const json::value& v, const Schema& header,
                                       const std::map<int, int>* dn) {
        if (!dn) return v;
        json::value out = v;
        auto& o = out.as_object();
        for (auto name : {"delete"sv, "this"sv, "update"sv})
            if (auto* r = o.if_contains(name))
                if (r->is_array())
                    mapDevNo(r->as_array(), header.reference, dn);
        return out;
    }

    // ============================================================
    //     Протокол: сервер
    // ============================================================
    std::string code;               // ожидаемый код сопряжения
    Cont onElected;                 // верный код: закрыть слушателя и остальных

    void serverGo() {
        readLine([this]() {
            auto& h = av.as_array();
            std::string clientCode(h.at(0).as_string());
            std::string clientDb(h.at(1).as_string());
            bool clientEmpty = h.size() > 2 && h[2].as_string() == "empty"sv;
            res.peerDb = clientDb;

            if (clientCode != code) {
                // Постороннее подключение: вежливо отказываем и закрываемся,
                // но сервер продолжает ждать настоящего партнёра.
                res.error = "bad_code";
                send(R"(["error","bad_code"])" "\n"s);
                flush([this] { finish(); });
                return;
            }
            // Код верный — этот сеанс и есть партнёр.
            if (onElected) { auto f = std::move(onElected); onElected = nullptr; f(); }
            if (clientDb != store.database()) {
                res.error = "db_mismatch";
                json::array e;
                e.emplace_back("error"sv);
                e.emplace_back("db_mismatch"sv);
                e.emplace_back(store.database());
                send(json::serialize(e) + "\n"s);
                flush([this] { finish(); });
                return;
            }

            if (!store.hasData()) {
                if (clientEmpty) {
                    peerDeviceNo = store.addDevice(peer);
                    queueFullFile("device"sv);
                    send(R"(["end"])" "\n"s);
                    flush([this] {
                        cmdNext([this] {
                            if (cmd != "done"sv) { fail("bad protocol"s); return; }
                            idx = SyncIndex();
                            idx.device = Store::stateOf(store.pDevice());
                            store.saveSyncIndex(peerDeviceNo, idx);
                            res.ok = true; res.received = 0; res.sent = 0;
                            finish();
                        });
                    });
                }
                else {
                    send(R"(["empty"])" "\n"s);
                    flush([this] {
                        cmdNext([this] {
                            recvAllWhenEmpty([this] { finish(); });
                        });
                    });
                }
                return;
            }
            if (clientEmpty) {
                sendAllToEmptyPeer(false, [this] { finish(); });
                return;
            }
            peerDeviceNo = store.knowsDevice(peer);
            if (peerDeviceNo) store.loadSyncIndex(peerDeviceNo, idx);
            Cont after = [this] {
                cmdNext([this] {
                    recvAllIncrement(nullptr, &idx, [this](bool ok) {
                        if (!ok) { finish(); return; }
			store.saveSyncIndex(peerDeviceNo, idx);
                        send(R"(["done"])" "\n"s);
                        flush([this] {
                            res.ok = true;
                            finish();
                        });
                    });
                });
            };
            if (idx.empty) sendAllToEmptyPeer(true, after);
            else sendAllIncrement(&idx, nullptr, nullptr, after);
        });
    }

    // ============================================================
    //     Протокол: клиент
    // ============================================================
    PairInfo info;

    void clientGo() {
        storeEmpty = !store.hasData();
        json::array hello;
        hello.emplace_back(info.code);
        hello.emplace_back(store.database());
        if (storeEmpty) hello.emplace_back("empty"sv);
        send(json::serialize(hello) + "\n"s);
        flush([this] {
            cmdNext([this] {
                if (cmd == "error"sv) {
                    res.error = std::string(ao->at(1).as_string());
                    if (ao->size() > 2)
                        res.peerDb = std::string(ao->at(2).as_string());
                    finish();
                    return;
                }
                if (storeEmpty) {
                    recvAllWhenEmpty([this] { finish(); });
                    return;
                }
                if (cmd == "empty"sv) {
                    sendAllToEmptyPeer(false, [this] { finish(); });
                    return;
                }
                peerDeviceNo = store.knowsDevice(peer);
                if (peerDeviceNo) {
                    store.loadSyncIndex(peerDeviceNo, idx);
                    idxNew.dnMap = idx.dnMap;
                }
                store.listManifest(idxCur);   // events заполнит приём
                recvAllIncrement(&idxCur, &idxNew, [this](bool ok) {
                    if (!ok) { finish(); return; }
                    sendAllIncrement(&idx, &idxCur, &idxNew, [this] {
                        cmdNext([this] {
                            if (cmd != "done"sv) {
                                res.error = "bad protocol"sv;
                                finish();
                                return;
                            }
                            store.saveSyncIndex(peerDeviceNo, idxNew);
                            res.ok = true;
                            finish();
                        });
                    });
                });
            });
        });
    }
};

// Слушающий сокет: QSslSocket поднимаем сами из дескриптора, чтобы полностью
// управлять рукопожатием (и не зависеть от версии QSslServer).
struct Listener : QTcpServer {
    std::function<void(qintptr)> onConn;
    void incomingConnection(qintptr d) override {
        if (onConn) onConn(d);
        else ::close((int)d);
    }
};

} // namespace

// ============================================================
//                          SyncServer
// ============================================================
struct SyncServer::Impl {
    explicit Impl(Store& s) : store(s) {}
    Store& store;
    Listener listener;
    QSslConfiguration sslConf;
    // До верного кода подключений может быть сколько угодно: любой сканер портов
    // или чужой клиент занял бы единственный слот. Все они — кандидаты, и лишь
    // приславший верный код становится сеансом.
    std::map<Session *, std::shared_ptr<Session>> pending;
    std::shared_ptr<Session> winner;
    std::string code;
    ConfirmFn confirm;
    DoneFn done;
    bool cancelled = false;
    bool reported = false;

    // Результат отдаём ровно один раз — в том числе если отменили ещё до того,
    // как подключился настоящий партнёр (сеанса тогда просто нет).
    void report(const SyncResult& r) {
        if (reported) return;
        reported = true;
        auto cb = std::move(done);
        done = nullptr;
        if (cb) cb(r);
    }

    // Кандидат отвалился (неверный код, разрыв, сбой TLS) — просто забыть его.
    void drop(Session* s) { pending.erase(s); }

    // Пришёл верный код: слушателя закрыть, остальных кандидатов оборвать молча.
    void elect(Session* s) {
        listener.close();
	decltype(pending) others; others.swap(pending);
        for (auto& [k,p] : others) {
            if (p.get() == s) winner = p;
	    else {
		p->done = nullptr; // о проигравших наверх не сообщаем
		p->cancel();
	    }
        }
    }

    // Оборвать всех кандидатов (отмена/разрушение сервера).
    void dropAll() {
	decltype(pending) others; others.swap(pending);
        for (auto& [k,p] : others) { p->done = nullptr; p->cancel(); }
    }
};

SyncServer::SyncServer(Store& store) : d_(std::make_unique<Impl>(store)) {}
SyncServer::~SyncServer() { cancel(); }

PairInfo SyncServer::start(ConfirmFn confirm, DoneFn done) {
    Impl* d = d_.get();
    d->sslConf = makeConfig(d->store);       // одна на все подключения
    // Обработчик ставим ДО listen(): иначе между «порт занят» и «есть кому
    // разбирать подключение» остаётся окно (его и приходилось затыкать
    // pauseAccepting).
    d->listener.onConn = [d](qintptr fd) {
        auto* s = new QSslSocket();
        s->setSslConfiguration(d->sslConf);
        if (!s->setSocketDescriptor(fd)) { delete s; ::close((int)fd); return; }
        auto sess = std::make_shared<Session>(d->store);
        sess->confirm = d->confirm;
        sess->code = d->code;
        Session* p = sess.get();
        // Пока код не пришёл — это лишь кандидат: его провал не завершает
        // работу сервера, а только вычёркивает его из списка.
        sess->done = [d, p](const SyncResult& r) {
            if (d->winner.get() == p) d->report(r);
            else d->drop(p);
        };
        sess->onElected = [d, p] { d->elect(p); };
        d->pending[p] = sess;
        sess->attach(s, [p] { p->serverGo(); });
        s->startServerEncryption();
    };
    if (!d->listener.listen(QHostAddress::AnyIPv4, 0))
        throw std::runtime_error("listen: " +
                                 d->listener.errorString().toStdString());
    // Взводим done только после удачного listen: у неудачного start()
    // продолжения нет — вызывающий получает исключение.
    d->code = randomCode(8);
    d->confirm = std::move(confirm);
    d->done = std::move(done);
    PairInfo info;
    info.ip = localIPv4();
    info.port = d->listener.serverPort();
    info.code = d->code;
    info.db = d->store.database();
    return info;
}

void SyncServer::cancel() {
    d_->cancelled = true;
    d_->listener.close();
    d_->dropAll();
    if (d_->winner) d_->winner->cancel();     // сеанс сам вызовет report()
    SyncResult r;
    r.error = "cancelled";
    d_->report(r);                            // no-op, если сеанс уже отчитался
}

// ============================================================
//                          SyncClient
// ============================================================
struct SyncClient::Impl {
    explicit Impl(Store& s) : store(s) {}
    Store& store;
    std::shared_ptr<Session> session;
    bool cancelled = false;
};

SyncClient::SyncClient(Store& store) : d_(std::make_unique<Impl>(store)) {}
SyncClient::~SyncClient() { cancel(); }

void SyncClient::start(const PairInfo& info, ConfirmFn confirm, DoneFn done) {
    auto sess = std::make_shared<Session>(d_->store);
    d_->session = sess;
    sess->confirm = std::move(confirm);
    sess->done = std::move(done);
    sess->info = info;
    auto* s = new QSslSocket();
    try {
        s->setSslConfiguration(makeConfig(d_->store));
    } catch (...) {
        delete s;
        d_->session.reset();
        throw;
    }
    Session* p = sess.get();
    sess->attach(s, [p] { p->clientGo(); });
    s->connectToHostEncrypted(QString::fromStdString(info.ip),
                              (quint16)info.port);
}

void SyncClient::cancel() {
    d_->cancelled = true;
    if (d_->session) d_->session->cancel();
}

} // namespace ha
