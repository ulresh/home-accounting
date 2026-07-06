#include "SyncService.h"
#include "Crypto.h"
#include "../model/Store.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json.hpp>
#include <openssl/x509.h>
#include <openssl/pem.h>

#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <random>
#include <stdexcept>
#include <atomic>
#include <mutex>
#include <memory>
#include <fstream>
#include <algorithm>
#include <functional>
#include <vector>

using namespace std::literals::string_view_literals;
using namespace std::string_literals;
namespace asio = boost::asio;
namespace ssl  = boost::asio::ssl;
namespace json = boost::json;
namespace fs   = std::filesystem;
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

std::string peerPubkey(SslStream& s) {
    return crypto::peerPubkey(s.native_handle());
}

void configureContext(ssl::context& ctx, Store& store) {
    ctx.set_verify_mode(ssl::verify_peer);
    ctx.set_verify_callback([](bool, ssl::verify_context&) { return true; });
    ctx.use_certificate_file(store.certPath().string(), ssl::context::pem);
    ctx.use_private_key_file(store.keyPath().string(), ssl::context::pem);
}

// ---- асинхронные помощники (всё через co_await — прерываются закрытием сокета) ----
asio::awaitable<std::string> aReadLine(SslStream& s, std::string& rbuf) {
    std::size_t nl;
    while ((nl = rbuf.find('\n')) == std::string::npos) {
        char tmp[4096];
        std::size_t n = co_await s.async_read_some(asio::buffer(tmp), asio::use_awaitable);
        rbuf.append(tmp, n);
    }
    std::string line = rbuf.substr(0, nl);
    rbuf.erase(0, nl + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    co_return line;
}

/*
// Прочитать ровно count байт, отдавая их блоками в sink сразу (без накопления).
asio::awaitable<void> aReadToSink(SslStream& s, std::string& rbuf, std::size_t count,
                                  std::function<void(const char*, std::size_t)> sink) {
    if (!rbuf.empty()) {
        std::size_t take = std::min(count, rbuf.size());
        if (take) { sink(rbuf.data(), take); rbuf.erase(0, take); count -= take; }
    }
    char block[16384];
    while (count > 0) {
        std::size_t want = std::min(count, sizeof(block));
        std::size_t got = co_await s.async_read_some(asio::buffer(block, want), asio::use_awaitable);
        sink(block, got);
        count -= got;
    }
}
*/

asio::awaitable<void> aReadSizedJson(SslStream &s, std::string &rbuf,
	std::size_t count, std::function<void(const json::value &v)> sink) {
    json::stream_parser sp;
    if(!rbuf.empty()) {
	auto p = rbuf.data();
	if(count < rbuf.size()) {
	    auto consumed = sp.write_some(p, count);
	    while(sp.done()) {
		sink(sp.release());
		sp.reset();
		p += consumed; count -= consumed;
		consumed = sp.write_some(p, count);
	    }
	    p += count;
	    if(*p != '\n') throw std::runtime_error("bad protocol"s);
	    rbuf.erase(0, count + 1);
	    co_return;
	}
	else {
	    auto size = rbuf.size();
	    count -= size;
	    auto consumed = sp.write_some(rbuf);
	    while(sp.done()) {
		sink(sp.release());
		sp.reset();
		p += consumed; size -= consumed;
		consumed = sp.write_some(p, size);
	    }
	    rbuf.clear();
	}
    }
    ++count; // \n после данных
    char block[16384];
    for(;;) {
        std::size_t want = std::min(count, sizeof(block));
        std::size_t got = co_await s.async_read_some(asio::buffer(block, want), asio::use_awaitable);
        count -= got;
	char *p = block;
	auto consumed = sp.write_some(p, got);
	while(sp.done()) {
	    sink(sp.release());
	    sp.reset();
	    p += consumed; got -= consumed;
	    consumed = sp.write_some(p, got);
	}
	if(!count) {
	    if(block[got - 1] != '\n')
		throw std::runtime_error("bad protocol"s);
	    break;
	}
    }
}

asio::awaitable<void> aWrite(SslStream& s, std::string data) {
    co_await asio::async_write(s, asio::buffer(data), asio::use_awaitable);
}
asio::awaitable<void> aWriteLine(SslStream& s, std::string line) {
    line.push_back('\n');
    co_await asio::async_write(s, asio::buffer(line), asio::use_awaitable);
}

asio::awaitable<void> aStreamFullFile(SslStream &s,
				      const Store &store,
				      std::string_view name) {
    auto path = store.dbDir() / (std::string(name) + ".jsonl"s);
    {   json::array h;
	h.emplace_back(name);
	h.emplace_back((uint64_t)fs::file_size(path));
	co_await aWriteLine(s, json::serialize(h));
    }
    std::ifstream in(path, std::ios::binary);
    char block[16384];
    while (in) {
        in.read(block, sizeof(block));
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        co_await asio::async_write(s, asio::buffer(block, (std::size_t)got),
				   asio::use_awaitable);
    }
    co_await aWrite(s, "\n");
}

asio::awaitable<MonthSyncData> aStreamFullEventFile(SslStream &s,
	const Store &store, int yyyymm, const fs::path &path) {
    auto size = fs::file_size(path);
    {   json::array h;
	h.emplace_back("event"sv);
	h.emplace_back(yyyymm);
	h.emplace_back((uint64_t)size);
	co_await aWriteLine(s, json::serialize(h));
    }
    std::ifstream in(path, std::ios::binary);
    char block[16384];
    json::stream_parser sp;
    Schema header;
    while (in) {
        in.read(block, sizeof(block));
        std::streamsize got = in.gcount();
        if (got <= 0) break;
	char *p = block;
	auto si = got;
	for(;;) {
	    auto consumed = sp.write_some(p, si);
	    if(!sp.done()) break;
	    auto v = sp.release();
            if(v.is_object()) {
                auto &o = v.as_object();
                if(o.if_contains("header")) header = Schema(o);
	    }
	    sp.reset();
	    p += consumed; si -= consumed;
	}
        co_await asio::async_write(s, asio::buffer(block, (std::size_t)got),
				   asio::use_awaitable);
    }
    co_await aWrite(s, "\n"s);
    co_return MonthSyncData{size, std::move(header)};
}

/*
// Прочитать файл [from, from+len) блоками и сразу слать в сеть (без накопления).
asio::awaitable<void> aStreamFile(SslStream& s, fs::path path, long long from, long long len) {
    std::ifstream in(path, std::ios::binary);
    if (in) in.seekg(from);
    char block[16384];
    long long remaining = len;
    while (remaining > 0 && in) {
        std::streamsize want = (std::streamsize)std::min<long long>(remaining, (long long)sizeof(block));
        in.read(block, want);
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        co_await asio::async_write(s, asio::buffer(block, (std::size_t)got), asio::use_awaitable);
        remaining -= got;
    }
}

asio::awaitable<void> aSendItems(SslStream& s, std::vector<SyncSendItem> items) {
    for (auto& it : items) {
        json::array h;
        if (it.kind == "event-tail") {
            h.emplace_back("event-tail"); h.emplace_back(it.month);
            h.emplace_back((int64_t)it.offset); h.emplace_back((int64_t)it.frameSize());
        } else {
            h.emplace_back(it.kind); h.emplace_back((int64_t)it.frameSize());
        }
        co_await aWriteLine(s, json::serialize(h));
        if (!it.prepend.empty()) co_await aWrite(s, it.prepend);
        co_await aStreamFile(s, it.path, it.fileFrom, it.fileLen);
        co_await aWrite(s, "\n");
    }
    co_await aWriteLine(s, "[\"end\"]");
}

asio::awaitable<void> aRecvItems(SslStream& s, std::string& rbuf, Store& store, bool replaceLists) {
    for (;;) {
        std::string line = co_await aReadLine(s, rbuf);
        if (line.empty()) continue;
        json::value v = json::parse(line);
        auto& a = v.as_array();
        std::string kind = std::string(a[0].as_string());
        if (kind == "end") break;
        int month = 0; long long size = 0;
        if (kind == "event-tail") { month = (int)a[1].as_int64(); size = a[3].as_int64(); }
        else size = a[1].as_int64();
        store.syncRecvBegin(kind, month, replaceLists);
        co_await aReadToSink(s, rbuf, (std::size_t)size,
            [&store](const char* d, std::size_t n){ store.syncRecvFeed(d, n); });
        store.syncRecvFinish();
        co_await aReadToSink(s, rbuf, 1, [](const char*, std::size_t){});   // завершающий '\n'
    }
}

// ---- манифест справочников ----
std::string manifestJson(const ListManifest& m) {
    auto arr = [](const FileState& f){ json::array a; a.emplace_back((int64_t)f.size); a.emplace_back(f.sha1); return a; };
    json::object inner;
    inner["people"]  = arr(m.people);
    inner["catalog"] = arr(m.catalog);
    inner["device"]  = arr(m.device);
    json::object o; o["manifest"] = std::move(inner);
    return json::serialize(o);
}
ListManifest manifestParse(const std::string& line) {
    ListManifest m;
    try {
        auto v = json::parse(line);
        auto& mo = v.as_object().at("manifest").as_object();
        auto rd = [&](const char* k, FileState& f){
            if (auto* p = mo.if_contains(k)) {
                auto& a = p->as_array();
                if (a.size() >= 2) { f.size = a[0].as_int64(); f.sha1 = std::string(a[1].as_string()); }
            }
        };
        rd("people", m.people); rd("catalog", m.catalog); rd("device", m.device);
    } catch (...) {}
    return m;
}

std::string summaryLine(int received) {
    return json::serialize(json::object{{"summary", json::object{{"received", received}}}});
}
int summaryReceived(const std::string& line) {
    return (int)json::parse(line).as_object().at("summary").as_object().at("received").as_int64();
}
*/

} // namespace

// ============================================================
//                          SyncServer
// ============================================================
struct SyncServer::Impl {
    Store& store;
    asio::io_context io;
    ssl::context ctx;
    tcp::acceptor acceptor;
    std::shared_ptr<SslStream> stream;
    std::mutex mtx;
    std::string code;
    int port = 0;
    std::atomic<bool> cancelled{false};
    Impl(Store& s) : store(s), ctx(ssl::context::tls_server), acceptor(io) {}
};

SyncServer::SyncServer(Store& store) : d_(std::make_unique<Impl>(store)) {}
SyncServer::~SyncServer() = default;

PairInfo SyncServer::listen() {
    configureContext(d_->ctx, d_->store);
    tcp::endpoint ep(tcp::v4(), 0);
    d_->acceptor.open(ep.protocol());
    d_->acceptor.set_option(tcp::acceptor::reuse_address(true));
    d_->acceptor.bind(ep);
    d_->acceptor.listen();
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
    // Закрыть acceptor и активный сокет в io-потоке — это прерывает ЛЮБУЮ
    // ожидающую async-операцию (accept/handshake/read/write).
    asio::post(d_->io, [this] {
        boost::system::error_code ec;
        d_->acceptor.close(ec);
        std::lock_guard<std::mutex> lk(d_->mtx);
        if (d_->stream) d_->stream->lowest_layer().close(ec);
    });
}

namespace {

#define DCMD(s) \
    auto av = json::parse(co_await aReadLine(s, rbuf)); \
    json::array *ao = &av.as_array(); \
    std::string cmd(ao->at(0).as_string())

asio::awaitable<void> aSendAllToEmptyPeer(SslStream &s, Store &store,
	const std::string &peer, SyncResult &res, bool storeSync = true) {
    auto peerDeviceNo = store.addDevice(peer);
    co_await aStreamFullFile(s, store, "device"sv);
    ++res.sent;
    if(!store.people_.empty()) {
	co_await aStreamFullFile(s, store, "people"sv);
	++res.sent;
    }
    if(!store.catalog_.empty()) {
	co_await aStreamFullFile(s, store, "catalog"sv);
	++res.sent;
    }
    if(storeSync) {
	SyncIndex idx;
	store.listManifest(idx);
	for(auto &[yyyymm, path] : store.enumerateMonths()) {
	    idx.events[yyyymm] =
		co_await aStreamFullEventFile(s, store, yyyymm, path);
	    ++res.sent;
	}
	co_await aWrite(s, R"(["end"])" "\n"s);
	std::string rbuf; DCMD(s);
	if(cmd != "done"sv) {
	    res.error = "bad protocol"sv;
	    co_return;
	}
	store.saveSyncIndex(peerDeviceNo, idx);
	res.ok = true;
    }
    else {
	// TODO +++ idx.events[yyyymm] = +++ заголовки надо сохранить, мы можем по этим файлам ничего не записать в разделе Recv
	for(auto &[yyyymm, path] : store.enumerateMonths()) {
	    co_await aStreamFullEventFile(s, store, yyyymm, path);
	    ++res.sent;
	}
	co_await aWrite(s, R"(["end"])" "\n"s);
    }
}

asio::awaitable<void> aRecvAllWhenEmpty(SslStream &s, Store &store,
	const std::string &peer, SyncResult &res,
	std::string &rbuf, json::array *ao, std::string cmd) {
    if(cmd != "device"sv) {
	res.error = "bad protocol"sv;
	co_return;
    }
    decltype(store.devices_) newDevices;
    int newDeviceNo = 0;
    int peerDeviceNo = 0;
    co_await aReadSizedJson(s, rbuf, ao->at(1).as_uint64(),
	[&newDevices, &newDeviceNo, &peer, &peerDeviceNo, &store, &res
	 ](const json::value &v) -> void {
	    newDevices.push_back(Device(v));
	    if(newDevices.back().pubkey == store.myPubkey_) {
		if(newDeviceNo)
		    throw std::runtime_error("bad protocol"s);
		newDeviceNo = newDevices.back().no;
	    }
	    else if(newDevices.back().pubkey == peer) {
		if(peerDeviceNo)
		    throw std::runtime_error("bad protocol"s);
		peerDeviceNo = newDevices.back().no;
	    }
	    ++res.received;
	});
    if(!newDeviceNo || !peerDeviceNo) {
	res.error = "bad protocol"sv;
	co_return;
    }
    store.devices_.swap(newDevices);
    store.deviceNo_ = newDeviceNo;
    store.saveDevices();
    store.saveConfig();
    auto av = json::parse(co_await aReadLine(s, rbuf));
    ao = &av.as_array();
    cmd = ao->at(0).as_string();
    if(cmd == "people"sv) {
	decltype(store.people_) newPeople;
	co_await aReadSizedJson(s, rbuf, ao->at(1).as_uint64(),
		[&newPeople,&res](const json::value &v) -> void {
		    newPeople.insert(newPeople.end(),
				     std::string(v.as_string()));
		    ++res.received;
		});
	store.people_.swap(newPeople);
	store.savePeople();
	av = json::parse(co_await aReadLine(s, rbuf));
	ao = &av.as_array();
	cmd = ao->at(0).as_string();
    }
    if(cmd == "catalog"sv) {
	decltype(store.catalog_) newCatalog;
	co_await aReadSizedJson(s, rbuf, ao->at(1).as_uint64(),
		[&newCatalog,&res](const json::value &v) -> void {
		    Store::appendCatalog(newCatalog, v);
		    ++res.received;
		});
	store.catalog_.swap(newCatalog);
	store.saveCatalog();
	av = json::parse(co_await aReadLine(s, rbuf));
	ao = &av.as_array();
	cmd = ao->at(0).as_string();
    }
    SyncIndex idx;
    store.listManifest(idx); // TODO +++ получать сразу из d.store.save*()
    while(cmd == "event"sv) {
	int yyyymm = ao->at(1).as_uint64();
	auto p = store.monthPath(yyyymm);
	if(p.has_parent_path()) fs::create_directories(p.parent_path());
	MonthEvents m(store);
	{   std::ofstream out(p, std::ios::binary);
	    co_await aReadSizedJson(s, rbuf,
				    ao->at(2).as_uint64(),
		[&m,&out,&res](const json::value &v) -> void {
		    out << json::serialize(v) << '\n';
		    m.add(v);
		    ++res.received;
		});
	}
	m.commit(yyyymm);
	idx.events[yyyymm] = { fs::file_size(p), m.header };
	av = json::parse(co_await aReadLine(s, rbuf));
	ao = &av.as_array();
	cmd = ao->at(0).as_string();
    }
    if(cmd == "end"sv) {
	co_await aWrite(s, R"(["done"])" "\n"s);
	store.saveSyncIndex(peerDeviceNo, idx);
	res.ok = true;
    }
    else res.error = "bad protocol"sv;
}

asio::awaitable<void> aRecvAllIncrement(SslStream &s, Store &store,
	const std::string &peer, SyncResult &res,
	std::string &rbuf, json::array *ao, std::string cmd,
	bool storeSync, int peerDeviceNo = 0) {
    // TODO +++ storeSync -> dnMapPtr
    if(cmd == "device"sv) {
	co_await aReadSizedJson(s, rbuf, ao->at(1).as_uint64(),
		[&](const json::value &v) -> void {
		    Device n(v);
	// TODO +++
		});
	// TODO +++
    }
    else if(storeSync && !peerDeviceNo) {
	res.error = "bad protocol"sv;
	co_return;
    }
    // TODO +++ recv all increment
    // TODO +++ recv "[\"end\"]\n"
    // TODO +++
}

asio::awaitable<void> serverProtocol(SyncServer::Impl& d, ConfirmFn confirm, SyncResult& res) {
    std::string rbuf;
    try {
        tcp::socket sock = co_await d.acceptor.async_accept(asio::use_awaitable);
        auto stream = std::make_shared<SslStream>(std::move(sock), d.ctx);
        { std::lock_guard<std::mutex> lk(d.mtx); d.stream = stream; }
        co_await stream->async_handshake(ssl::stream_base::server, asio::use_awaitable);

        std::string peer = peerPubkey(*stream);
        res.peerPubkey = peer;

	std::string clientCode, clientDb;
	bool clientEmpty;
        {   auto hv = json::parse(co_await aReadLine(*stream, rbuf));
	    auto& ha = hv.as_array();
	    clientCode = std::string(ha[0].as_string());
	    clientDb = std::string(ha[1].as_string());
	    clientEmpty = ha.size() > 2 && ha[2].as_string() == "empty"sv;
	}
        res.peerDb = clientDb;

        if (clientCode != d.code) {
	    co_await aWrite(*stream, R"(["error","bad_code"])" "\n"s);
	    res.error = "bad_code";
	    co_return;
	}
        if (clientDb != d.store.database()) {
            json::array e;
	    e.emplace_back("error"sv);
	    e.emplace_back("db_mismatch"sv);
	    e.emplace_back(d.store.database());
            co_await aWriteLine(*stream, json::serialize(e));
	    res.error = "db_mismatch";
	    co_return;
        }

	if(!d.store.hasData()) {
	    if(clientEmpty) {
		auto peerDeviceNo = d.store.addDevice(peer);
		co_await aStreamFullFile(*stream, d.store, "device"sv);
		co_await aWrite(*stream, R"(["end"])" "\n"s);
		DCMD(*stream);
		if(cmd != "done"sv) {
		    res.error = "bad protocol"sv;
		    co_return;
		}
		SyncIndex idx;
		idx.device = Store::stateOf(d.store.pDevice());
		d.store.saveSyncIndex(peerDeviceNo, idx);
		res.ok = true; res.received = 0; res.sent = 0;
	    }
	    else {
		co_await aWrite(*stream, R"(["empty"])" "\n"s);
		DCMD(*stream);
		co_await aRecvAllWhenEmpty(*stream, d.store, peer, res,
					   rbuf, ao, cmd);
	    }
	}
	else if(clientEmpty)
	    co_await aSendAllToEmptyPeer(*stream, d.store, peer, res);
	else {
	    SyncIndex idx;
	    auto peerDeviceNo = d.store.knowsDevice(peer);
	    if(peerDeviceNo)
		d.store.loadSyncIndex(peerDeviceNo, idx);
	    if(idx.empty)
		co_await aSendAllToEmptyPeer(*stream, d.store, peer, res,
					     &idx);
	    else {
		// TODO +++ send all increment
		co_await aWrite(*stream, R"(["end"])" "\n"s);
	    }
	    DCMD(*stream);
	    co_await aRecvAllIncrement(*stream, d.store, peer, res,
				       rbuf, ao, cmd, true, peerDeviceNo);
	}
#if 0
        // обмен манифестами справочников (состояние собеседника)
        co_await aWriteLine(*stream, manifestJson(d.store.listManifest()));
        ListManifest peerMan = manifestParse(co_await aReadLine(*stream, rbuf));

        int peerLocal = d.store.reserveDeviceNo(peer, clientDevNo, "peer");
        d.store.syncBegin(peerLocal);
        co_await aRecvItems(*stream, rbuf, d.store, /*replaceLists=*/false);   // принять и слить
        d.store.syncDedup();
        co_await aSendItems(*stream, d.store.syncPlanOutgoing(peerMan));       // отдать (потоково)
        int ks = d.store.syncReceived();

        co_await aWriteLine(*stream, summaryLine(ks));
        int kc = summaryReceived(co_await aReadLine(*stream, rbuf));

        d.store.syncCommit(peerLocal);
        d.store.syncEnd();
        res.ok = true; res.received = ks; res.sent = kc;
#endif
        boost::system::error_code ec; stream->shutdown(ec);
    } catch (const std::exception& e) {
        d.store.syncEnd();
        if (res.error.empty()) res.error = d.cancelled ? "cancelled" : e.what();
    }
    co_return;
}
} // namespace

SyncResult SyncServer::wait(ConfirmFn confirm) {
    SyncResult res;
    asio::co_spawn(d_->io, serverProtocol(*d_, confirm, res), asio::detached);
    d_->io.run();
    if (!res.ok && res.error.empty() && d_->cancelled) res.error = "cancelled";
    return res;
}

// ============================================================
//                          SyncClient
// ============================================================
struct SyncClient::Impl {
    Store& store;
    asio::io_context io;
    ssl::context ctx;
    std::shared_ptr<SslStream> stream;
    std::mutex mtx;
    std::atomic<bool> cancelled{false};
    Impl(Store& s) : store(s), ctx(ssl::context::tls_client) {}
};

SyncClient::SyncClient(Store& store) : d_(std::make_unique<Impl>(store)) {}
SyncClient::~SyncClient() = default;

void SyncClient::cancel() {
    d_->cancelled = true;
    asio::post(d_->io, [this] {
        boost::system::error_code ec;
        std::lock_guard<std::mutex> lk(d_->mtx);
        if (d_->stream) d_->stream->lowest_layer().close(ec);
    });
}

namespace {
asio::awaitable<void> clientProtocol(SyncClient::Impl& d, const PairInfo& info, ConfirmFn confirm, SyncResult& res) {
    std::string rbuf;
    try {
        tcp::socket sock(d.io);
        tcp::endpoint ep(asio::ip::make_address(info.ip), (unsigned short)info.port);
        co_await sock.async_connect(ep, asio::use_awaitable);
        auto stream = std::make_shared<SslStream>(std::move(sock), d.ctx);
        { std::lock_guard<std::mutex> lk(d.mtx); d.stream = stream; }
        co_await stream->async_handshake(ssl::stream_base::client, asio::use_awaitable);

        std::string peer = peerPubkey(*stream);
        res.peerPubkey = peer;

	bool storeEmpty = !d.store.hasData();
        {   json::array hello;
	    hello.emplace_back(info.code);
	    hello.emplace_back(d.store.database());
	    if(storeEmpty) hello.emplace_back("empty"sv);
	    co_await aWriteLine(*stream, json::serialize(hello));
	}

        auto av = json::parse(co_await aReadLine(*stream, rbuf));
	json::array *ao = &av.as_array();
	std::string cmd(ao->at(0).as_string());
	if(cmd == "error"sv) {
            res.error = std::string(ao->at(1).as_string());
	    if(ao->size() > 2) res.peerDb = std::string(ao->at(2).as_string());
            co_return;
	}

	if(storeEmpty)
	    co_await aRecvAllWhenEmpty(*stream, d.store, peer, res,
				       rbuf, ao, cmd);
	else if(cmd == "empty"sv)
	    co_await aSendAllToEmptyPeer(*stream, d.store, peer, res);
	else {
	    co_await aRecvAllIncrement(*stream, d.store, peer, res,
				       rbuf, ao, cmd, false);
	    auto peerDeviceNo = d.store.knowsDevice(peer);
	    if(!peerDeviceNo) {
		res.error = "bad protocol"sv;
		co_return;
	    }
	    // TODO +++
	}
#if 0
        if (!d.store.knowsDevice(peer)) {
            if (!confirm || !confirm(peer)) { res.error = "rejected"; co_return; }
            d.store.reserveDeviceNo(peer, serverDevNo, "peer");
        }

        ListManifest serverMan = manifestParse(co_await aReadLine(*stream, rbuf));  // сервер прислал манифест
        co_await aWriteLine(*stream, manifestJson(d.store.listManifest()));

        int peerLocal = d.store.reserveDeviceNo(peer, serverDevNo, "peer");
        d.store.syncBegin(peerLocal);
        co_await aSendItems(*stream, d.store.syncPlanOutgoing(serverMan));     // отдать (потоково)
        co_await aRecvItems(*stream, rbuf, d.store, /*replaceLists=*/true);    // принять итог как есть
        d.store.syncDedup();
        int kc = d.store.syncReceived();

        int ks = summaryReceived(co_await aReadLine(*stream, rbuf));
        co_await aWriteLine(*stream, summaryLine(kc));

        d.store.syncCommit(peerLocal);
        d.store.syncEnd();
        res.ok = true; res.received = kc; res.sent = ks;
#endif
        boost::system::error_code ec; stream->shutdown(ec);
    } catch (const std::exception& e) {
        d.store.syncEnd();
        if (res.error.empty()) res.error = d.cancelled ? "cancelled" : e.what();
    }
    co_return;
}
} // namespace

SyncResult SyncClient::connect(const PairInfo& info, ConfirmFn confirm) {
    SyncResult res;
    configureContext(d_->ctx, d_->store);
    asio::co_spawn(d_->io, clientProtocol(*d_, info, confirm, res), asio::detached);
    d_->io.run();
    if (!res.ok && res.error.empty() && d_->cancelled) res.error = "cancelled";
    return res;
}

} // namespace ha
