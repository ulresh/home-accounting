#include "SyncService.h"
#include "Crypto.h"
#include "../model/Store.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/json.hpp>
#include <openssl/x509.h>
#include <openssl/pem.h>

#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <random>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>
#include <vector>

namespace asio = boost::asio;
namespace ssl  = boost::asio::ssl;
namespace json = boost::json;
using tcp = asio::ip::tcp;
using SslStream = ssl::stream<tcp::socket>;

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

// ---------- общие помощники ----------
namespace {

std::string randomCode(int n) {
    static const char* A = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 30);
    std::string s;
    for (int i = 0; i < n; ++i) s += A[dist(rd)];
    return s;
}

std::string localIPv4() {
    struct ifaddrs* ifs = nullptr;
    std::string result = "127.0.0.1";
    if (getifaddrs(&ifs) != 0) return result;
    for (auto* p = ifs; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (p->ifa_flags & IFF_LOOPBACK) continue;
        char buf[INET_ADDRSTRLEN];
        auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        result = buf;
        break;
    }
    freeifaddrs(ifs);
    return result;
}

void writeLine(SslStream& s, const std::string& line) {
    std::string out = line;
    out.push_back('\n');
    asio::write(s, asio::buffer(out));
}

std::string readLine(SslStream& s, asio::streambuf& buf) {
    asio::read_until(s, buf, '\n');
    std::istream is(&buf);
    std::string line;
    std::getline(is, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

// Прочитать ровно n байт (хвост блока передаётся «без разбора»).
std::string readExact(SslStream& s, asio::streambuf& buf, std::size_t n) {
    while (buf.size() < n) {
        boost::system::error_code ec;
        asio::read(s, buf, asio::transfer_at_least(n - buf.size()), ec);
        if (ec) { if (buf.size() < n) throw std::runtime_error("short read"); break; }
    }
    auto begin = asio::buffers_begin(buf.data());
    std::string out(begin, begin + n);
    buf.consume(n);
    return out;
}

std::string peerPubkey(SslStream& s) {
    X509* cert = SSL_get1_peer_certificate(s.native_handle());
    if (!cert) return "";
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    char* p = nullptr;
    long n = BIO_get_mem_data(bio, &p);
    std::string pem(p, n);
    BIO_free(bio);
    X509_free(cert);
    try { return crypto::publicKeyFromCertPem(pem); } catch (...) { return ""; }
}

// Отдать набор блоков: заголовок-строка + сырые данные + '\n'. Завершить ["end"].
void sendBlobs(SslStream& s, const std::vector<SyncBlob>& blobs) {
    for (auto& b : blobs) {
        json::array h;
        if (b.kind == "event-tail") {
            h.emplace_back("event-tail");
            h.emplace_back(b.month);
            h.emplace_back((int64_t)b.offset);
            h.emplace_back((int64_t)b.data.size());
        } else {
            h.emplace_back(b.kind);
            h.emplace_back((int64_t)b.data.size());
        }
        writeLine(s, json::serialize(h));
        std::string payload = b.data;
        payload.push_back('\n');
        asio::write(s, asio::buffer(payload));
    }
    writeLine(s, "[\"end\"]");
}

// Принять блоки до ["end"], каждый сразу применить к store.
void recvBlobs(SslStream& s, asio::streambuf& buf, Store& store,
               bool replaceLists, std::set<std::string>* added) {
    for (;;) {
        std::string line = readLine(s, buf);
        if (line.empty()) continue;
        json::value v = json::parse(line);
        auto& a = v.as_array();
        std::string kind = std::string(a[0].as_string());
        if (kind == "end") break;
        SyncBlob b; b.kind = kind;
        long long size = 0;
        if (kind == "event-tail") {
            b.month  = (int)a[1].as_int64();
            b.offset = a[2].as_int64();
            size     = a[3].as_int64();
        } else {
            size = a[1].as_int64();
        }
        b.data = readExact(s, buf, (std::size_t)size);
        readExact(s, buf, 1);            // завершающий '\n'
        store.syncApply(b, replaceLists, added);
    }
}

void configureContext(ssl::context& ctx, Store& store) {
    ctx.set_verify_mode(ssl::verify_peer);
    ctx.set_verify_callback([](bool, ssl::verify_context&) { return true; });
    ctx.use_certificate_file(store.certPath().string(), ssl::context::pem);
    ctx.use_private_key_file(store.keyPath().string(), ssl::context::pem);
}

} // namespace

// ---------- SyncServer ----------
struct SyncServer::Impl {
    Store& store;
    asio::io_context io;
    tcp::acceptor acceptor;
    std::string code;
    int port = 0;
    std::atomic<bool> cancelled{false};
    Impl(Store& s) : store(s), acceptor(io) {}
};

SyncServer::SyncServer(Store& store) : d_(std::make_unique<Impl>(store)) {}
SyncServer::~SyncServer() = default;

PairInfo SyncServer::listen() {
    tcp::endpoint ep(tcp::v4(), 0);
    d_->acceptor.open(ep.protocol());
    d_->acceptor.set_option(tcp::acceptor::reuse_address(true));
    d_->acceptor.bind(ep);
    d_->acceptor.listen();
    d_->acceptor.non_blocking(true);
    d_->port = d_->acceptor.local_endpoint().port();
    d_->code = randomCode(8);
    PairInfo info;
    info.ip = localIPv4();
    info.port = d_->port;
    info.code = d_->code;
    info.db = d_->store.database();
    return info;
}

void SyncServer::cancel() {
    d_->cancelled = true;
    boost::system::error_code ec;
    d_->acceptor.close(ec);
}

SyncResult SyncServer::wait(ConfirmFn confirm) {
    SyncResult res;
    try {
        tcp::socket sock(d_->io);
        for (;;) {
            if (d_->cancelled) { res.error = "cancelled"; return res; }
            boost::system::error_code ec;
            d_->acceptor.accept(sock, ec);
            if (!ec) break;
            if (ec == asio::error::would_block || ec == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            res.error = d_->cancelled ? "cancelled" : ec.message();
            return res;
        }
        sock.non_blocking(false);

        ssl::context ctx(ssl::context::tls_server);
        configureContext(ctx, d_->store);
        SslStream stream(std::move(sock), ctx);
        stream.handshake(ssl::stream_base::server);

        std::string peer = peerPubkey(stream);
        res.peerPubkey = peer;
        asio::streambuf buf;

        auto hv = json::parse(readLine(stream, buf));
        auto& ho = hv.as_object().at("hello").as_object();
        std::string clientDb = std::string(ho.at("db").as_string());
        std::string clientCode = std::string(ho.at("code").as_string());
        int clientDevNo = ho.if_contains("device_no") ? (int)ho.at("device_no").as_int64() : 0;
        bool clientHasData = ho.if_contains("has_data") && ho.at("has_data").as_bool();
        res.peerDb = clientDb;

        if (clientCode != d_->code) {
            writeLine(stream, "{\"error\":\"bad_code\"}");
            res.error = "bad_code"; return res;
        }
        if (clientDb != d_->store.database()) {
            json::object e; e["error"] = "db_mismatch"; e["db"] = d_->store.database();
            writeLine(stream, json::serialize(e));
            res.error = "db_mismatch"; return res;
        }
        // Разрешение конфликта собственных номеров (меняет ТОЛЬКО одно устройство).
        if (clientDevNo == d_->store.deviceNo() && !d_->store.hasData() && clientHasData) {
            d_->store.renumberSelf(std::max(d_->store.maxDeviceNo(), clientDevNo) + 1);
        }
        int ackMaxDn = d_->store.maxDeviceNo();
        if (!d_->store.knowsDevice(peer)) {
            if (!confirm || !confirm(peer)) {
                writeLine(stream, "{\"error\":\"rejected\"}");
                res.error = "rejected"; return res;
            }
            d_->store.reserveDeviceNo(peer, clientDevNo, "peer");
        }

        json::object ok;
        ok["ok"] = true; ok["db"] = d_->store.database();
        ok["device_no"] = d_->store.deviceNo(); ok["pubkey"] = d_->store.myPubkey();
        ok["max_dn"] = ackMaxDn;
        writeLine(stream, json::serialize(ok));

        // ----- инкрементный обмен файлами -----
        int peerLocal = d_->store.reserveDeviceNo(peer, clientDevNo, "peer");
        d_->store.syncBegin(peerLocal);
        recvBlobs(stream, buf, d_->store, /*replaceLists=*/false, nullptr);  // принять и слить
        d_->store.syncDedup();                                              // убрать дубликаты
        sendBlobs(stream, d_->store.syncBuildOutgoing(/*forceLists=*/true)); // отдать итог
        int ks = d_->store.syncReceived();

        writeLine(stream, json::serialize(json::object{{"summary", json::object{{"received", ks}}}}));
        auto cs = json::parse(readLine(stream, buf));
        int kc = (int)cs.as_object().at("summary").as_object().at("received").as_int64();

        d_->store.syncCommit(peerLocal);
        d_->store.syncEnd();

        res.ok = true; res.received = ks; res.sent = kc;
        boost::system::error_code ec;
        stream.shutdown(ec);
    } catch (const std::exception& e) {
        d_->store.syncEnd();
        res.ok = false;
        if (res.error.empty()) res.error = e.what();
    }
    return res;
}

// ---------- SyncClient ----------
SyncClient::SyncClient(Store& store) : store_(store) {}

SyncResult SyncClient::connect(const PairInfo& info, ConfirmFn confirm) {
    SyncResult res;
    try {
        asio::io_context io;
        tcp::socket sock(io);
        tcp::endpoint ep(asio::ip::make_address(info.ip), (unsigned short)info.port);
        sock.connect(ep);

        ssl::context ctx(ssl::context::tls_client);
        configureContext(ctx, store_);
        SslStream stream(std::move(sock), ctx);
        stream.handshake(ssl::stream_base::client);

        std::string peer = peerPubkey(stream);
        res.peerPubkey = peer;
        asio::streambuf buf;

        json::object hello;
        hello["db"] = store_.database();
        hello["device_no"] = store_.deviceNo();
        hello["pubkey"] = store_.myPubkey();
        hello["code"] = info.code;
        hello["has_data"] = store_.hasData();
        writeLine(stream, json::serialize(json::object{{"hello", hello}}));

        auto av = json::parse(readLine(stream, buf));
        auto& ao = av.as_object();
        if (auto* err = ao.if_contains("error")) {
            res.error = std::string(err->as_string());
            if (auto* db = ao.if_contains("db")) res.peerDb = std::string(db->as_string());
            return res;
        }
        res.peerDb = std::string(ao.at("db").as_string());

        int serverDevNo = ao.if_contains("device_no") ? (int)ao.at("device_no").as_int64() : 0;
        int serverMaxDn = ao.if_contains("max_dn") ? (int)ao.at("max_dn").as_int64() : serverDevNo;
        if (serverDevNo == store_.deviceNo() && !store_.hasData()) {
            store_.renumberSelf(std::max(store_.maxDeviceNo(), serverMaxDn) + 1);
        }

        if (!store_.knowsDevice(peer)) {
            if (!confirm || !confirm(peer)) { res.error = "rejected"; return res; }
            store_.reserveDeviceNo(peer, serverDevNo, "peer");
        }

        // ----- инкрементный обмен файлами -----
        int peerLocal = store_.reserveDeviceNo(peer, serverDevNo, "peer");
        store_.syncBegin(peerLocal);
        sendBlobs(stream, store_.syncBuildOutgoing(/*forceLists=*/false));   // отдать свой хвост
        recvBlobs(stream, buf, store_, /*replaceLists=*/true, nullptr);      // принять итог как есть
        store_.syncDedup();
        int kc = store_.syncReceived();

        auto sv = json::parse(readLine(stream, buf));
        int ks = (int)sv.as_object().at("summary").as_object().at("received").as_int64();
        writeLine(stream, json::serialize(json::object{{"summary", json::object{{"received", kc}}}}));

        store_.syncCommit(peerLocal);
        store_.syncEnd();

        res.ok = true; res.received = kc; res.sent = ks;
        boost::system::error_code ec;
        stream.shutdown(ec);
    } catch (const std::exception& e) {
        store_.syncEnd();
        res.ok = false;
        if (res.error.empty()) res.error = e.what();
    }
    return res;
}

} // namespace ha
