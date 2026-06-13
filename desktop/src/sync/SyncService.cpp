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

// дамп <-> json
json::value dumpToJson(const SyncDump& d) {
    json::object o;
    o["db"] = d.db;
    json::array people;
    for (auto& p : d.people) people.emplace_back(p);
    o["people"] = std::move(people);
    json::array cat;
    for (auto& e : d.catalog) {
        json::array a; a.emplace_back(e.category);
        for (auto& it : e.items) a.emplace_back(it);
        cat.push_back(std::move(a));
    }
    o["catalog"] = std::move(cat);
    json::array devs;
    for (auto& dev : d.devices) {
        json::array a; a.emplace_back(dev.no); a.emplace_back(dev.pubkey);
        if (!dev.name.empty()) a.emplace_back(dev.name);
        devs.push_back(std::move(a));
    }
    o["devices"] = std::move(devs);
    json::array evs;
    for (auto& [mf, line] : d.events) {
        json::array a; a.emplace_back(mf); a.emplace_back(line);
        evs.push_back(std::move(a));
    }
    o["events"] = std::move(evs);
    return o;
}

SyncDump dumpFromJson(const json::value& v) {
    SyncDump d;
    auto& o = v.as_object();
    if (auto* x = o.if_contains("db")) d.db = std::string(x->as_string());
    if (auto* x = o.if_contains("people"))
        for (auto& p : x->as_array()) d.people.push_back(std::string(p.as_string()));
    if (auto* x = o.if_contains("catalog"))
        for (auto& c : x->as_array()) {
            auto& a = c.as_array();
            CatalogEntry e; e.category = std::string(a[0].as_string());
            for (size_t i = 1; i < a.size(); ++i) e.items.push_back(std::string(a[i].as_string()));
            d.catalog.push_back(std::move(e));
        }
    if (auto* x = o.if_contains("devices"))
        for (auto& c : x->as_array()) {
            auto& a = c.as_array();
            Device dev; dev.no = (int)a[0].as_int64(); dev.pubkey = std::string(a[1].as_string());
            if (a.size() > 2 && a[2].is_string()) dev.name = std::string(a[2].as_string());
            d.devices.push_back(std::move(dev));
        }
    if (auto* x = o.if_contains("events"))
        for (auto& c : x->as_array()) {
            auto& a = c.as_array();
            d.events.emplace_back(std::string(a[0].as_string()), std::string(a[1].as_string()));
        }
    return d;
}

void configureContext(ssl::context& ctx, Store& store) {
    ctx.set_verify_mode(ssl::verify_peer);
    ctx.set_verify_callback([](bool, ssl::verify_context&) { return true; }); // self-signed ок
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
    tcp::endpoint ep(tcp::v4(), 0);           // случайный свободный порт
    d_->acceptor.open(ep.protocol());
    d_->acceptor.set_option(tcp::acceptor::reuse_address(true));
    d_->acceptor.bind(ep);
    d_->acceptor.listen();
    d_->acceptor.non_blocking(true);          // чтобы ожидание можно было прервать
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
        // Ожидание подключения с возможностью отмены (cancel()/закрытие окна).
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
        sock.non_blocking(false);             // дальше — блокирующий обмен

        ssl::context ctx(ssl::context::tls_server);
        configureContext(ctx, d_->store);
        SslStream stream(std::move(sock), ctx);
        stream.handshake(ssl::stream_base::server);

        std::string peer = peerPubkey(stream);
        res.peerPubkey = peer;
        asio::streambuf buf;

        // hello
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
        // Разрешение конфликта собственных номеров (меняет ТОЛЬКО одно устройство):
        // свой номер меняем, если у нас нет своих данных, а у партнёра — есть.
        if (clientDevNo == d_->store.deviceNo() && !d_->store.hasData() && clientHasData) {
            d_->store.renumberSelf(std::max(d_->store.maxDeviceNo(), clientDevNo) + 1);
        }
        int ackMaxDn = d_->store.maxDeviceNo();   // до резервирования партнёра
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

        // обмен дампами: сервер шлёт первым
        writeLine(stream, json::serialize(dumpToJson(d_->store.dump())));
        SyncDump remote = dumpFromJson(json::parse(readLine(stream, buf)));
        int ks = d_->store.mergeDump(remote);

        json::object sum; sum["received"] = ks;
        writeLine(stream, json::serialize(json::object{{"summary", sum}}));
        auto cs = json::parse(readLine(stream, buf));
        int kc = (int)cs.as_object().at("summary").as_object().at("received").as_int64();

        res.ok = true; res.received = ks; res.sent = kc;

        boost::system::error_code ec;
        stream.shutdown(ec);
    } catch (const std::exception& e) {
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

        // Разрешение конфликта собственных номеров на стороне клиента:
        // если сервер не сменил свой номер и он совпал с нашим, а у нас нет
        // своих данных — меняем свой (так меняет только одно устройство).
        {
            int serverDevNo = ao.if_contains("device_no") ? (int)ao.at("device_no").as_int64() : 0;
            int serverMaxDn = ao.if_contains("max_dn") ? (int)ao.at("max_dn").as_int64() : serverDevNo;
            if (serverDevNo == store_.deviceNo() && !store_.hasData()) {
                store_.renumberSelf(std::max(store_.maxDeviceNo(), serverMaxDn) + 1);
            }
        }

        // подтверждение нового устройства и на стороне клиента
        if (!store_.knowsDevice(peer)) {
            if (!confirm || !confirm(peer)) { res.error = "rejected"; return res; }
            int devNo = ao.if_contains("device_no") ? (int)ao.at("device_no").as_int64() : 0;
            store_.reserveDeviceNo(peer, devNo, "peer");
        }

        // сервер шлёт дамп первым
        SyncDump remote = dumpFromJson(json::parse(readLine(stream, buf)));
        int kc = store_.mergeDump(remote);
        writeLine(stream, json::serialize(dumpToJson(store_.dump())));

        auto sv = json::parse(readLine(stream, buf));
        int ks = (int)sv.as_object().at("summary").as_object().at("received").as_int64();
        writeLine(stream, json::serialize(json::object{{"summary", json::object{{"received", kc}}}}));

        res.ok = true; res.received = kc; res.sent = ks;
        boost::system::error_code ec;
        stream.shutdown(ec);
    } catch (const std::exception& e) {
        res.ok = false;
        if (res.error.empty()) res.error = e.what();
    }
    return res;
}

} // namespace ha
