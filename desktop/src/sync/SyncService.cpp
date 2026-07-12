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
#include <list>

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
#define DvCMD(s) \
    auto av = json::parse(co_await aReadLine(s, rbuf)); \
    ao = &av.as_array(); \
    cmd = ao->at(0).as_string()
#define CMD(s) \
    av = json::parse(co_await aReadLine(s, rbuf)); \
    ao = &av.as_array(); \
    cmd = ao->at(0).as_string()

asio::awaitable<void> aSendAllToEmptyPeer(SslStream &s, Store &store,
	const std::string &peer, SyncResult &res,
	SyncIndex *idxp = nullptr) {
    auto peerDeviceNo = store.addDevice(peer);
    co_await aStreamFullFile(s, store, "device"sv);
    ++res.sent;
    if(!store.people_.empty() || !store.people_delete.empty()) {
	co_await aStreamFullFile(s, store, "people"sv);
	++res.sent;
    }
    if(!store.catalog_.empty()) {
	co_await aStreamFullFile(s, store, "catalog"sv);
	++res.sent;
    }
    if(!idxp) {
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
	for(auto &[yyyymm, path] : store.enumerateMonths()) {
	    idxp->events[yyyymm] =
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
	    newDevices.push_back(Device(v, false));
	    if(newDevices.back().pubkey == store.myPubkey_) {
		if(newDeviceNo)
		    throw std::runtime_error("bad protocol"s);
		newDeviceNo = newDevices.back().no;
		newDevices.back().name = "this"s;
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
	decltype(store.people_) newPeople, newPeopleDelete;
	auto *p = &newPeople;
	co_await aReadSizedJson(s, rbuf, ao->at(1).as_uint64(),
		[&p,&newPeopleDelete,&res](const json::value &v) -> void {
        if(v.is_object())
	    for(auto &[value,time] : v.as_object())
		if(time.is_string())
		    p->emplace_hint(p->end(), std::string(value),
				    std::string(time.as_string()));
	else if(v.is_array()) {
	    auto a = v.as_array();
	    if(a.size() == 1 && a[0].is_string() &&
	       a[0].as_string() == "delete"s)
		p = &newPeopleDelete;
	}
		    ++res.received;
		});
	store.people_.swap(newPeople);
	store.people_delete.swap(newPeopleDelete);
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

asio::awaitable<void> aSendAllIncrement(SslStream &s, Store &store,
	const std::string &peer, SyncResult &res) {
    // TODO +++ send all increment
    co_await aWrite(s, R"(["end"])" "\n"s);
}

asio::awaitable<bool> aRecvAllIncrement(SslStream &s, Store &store,
	const std::string &peer, SyncResult &res,
	std::string &rbuf, json::array *ao, std::string cmd,
	int &peerDeviceNo, SyncIndex *idxCur, SyncIndex *idxNew) {
    if(cmd == "device"sv) {
	std::unique_ptr<std::ofstream> outp;
	std::list<Device> reno;
	co_await aReadSizedJson(s, rbuf, ao->at(1).as_uint64(),
		[&](const json::value &v) -> void {
		    Device n(v, false);
		    bool busyno = false;
		    for(auto &d : store.devices_)
			if(d.pubkey == n.pubkey) {
			    if(d.no != n.no) idxNew->dnMap[n.no] = d.no;
			    return;
			}
			else if(d.no == n.no) busyno = true;
		    if(busyno) reno.push_back(std::move(n));
		    else store.addDevice(outp, n.no, n.pubkey);
		});
	if(!reno.empty()) {
	    auto m = store.maxDeviceNo();
	    for(auto &n : reno) {
		if(m == std::numeric_limits<int>::max())
		    throw std::runtime_error("too big device no"s);
		store.addDevice(outp, ++m, n.pubkey);
		idxNew->dnMap[n.no] = m;
	    }
	}
	outp.reset();
	if(!peerDeviceNo) peerDeviceNo = store.knowsDevice(peer);
	if(!peerDeviceNo) {
	    res.error = "bad protocol"sv;
	    co_return false;
	}
	idxNew->device = store.stateOf(store.pDevice());
	DvCMD(s);
    }
    else {
	if(!peerDeviceNo) {
	    res.error = "bad protocol"sv;
	    co_return false;
	}
	idxNew->device = idxCur ? idxCur->device
	    : store.stateOf(store.pDevice());
    }
    if(cmd == "people"sv) {
	co_await aReadSizedJson(s, rbuf, ao->at(1).as_uint64(),
		[&](const json::value &v) -> void {
		    // TODO +++
		});
	// TODO +++
	idxNew->people = store.stateOf(store.pPeople());
	DvCMD(s);
    }
    else idxNew->people = idxCur ? idxCur->people
	    : store.stateOf(store.pPeople());
    if(cmd == "catalog"sv) {
	co_await aReadSizedJson(s, rbuf, ao->at(1).as_uint64(),
		[&](const json::value &v) -> void {
		    // TODO +++
		});
	// TODO +++
	idxNew->catalog = store.stateOf(store.pCatalog());
	DvCMD(s);
    }
    else idxNew->catalog = idxCur ? idxCur->catalog
	    : store.stateOf(store.pCatalog());
    // TODO +++ recv all increment
    // TODO +++ не забыть почистить дубликаты в event
    // TODO +++ recv "[\"end\"]\n"
    // TODO +++
    co_return true;
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
	    else co_await aSendAllIncrement(*stream, d.store, peer, res);
	    DCMD(*stream);
	    if(!co_await aRecvAllIncrement(*stream, d.store, peer, res,
			rbuf, ao, cmd, peerDeviceNo, nullptr, &idx))
		co_return;
	    co_await aWrite(*stream, R"(["done"])" "\n"s);
	    d.store.saveSyncIndex(peerDeviceNo, idx);
	    res.ok = true;
	}
        boost::system::error_code ec; stream->shutdown(ec);
    } catch (const std::exception& e) {
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
	    SyncIndex idxOld, idxCur, idxNew;
	    auto peerDeviceNo = d.store.knowsDevice(peer);
	    if(peerDeviceNo) {
		d.store.loadSyncIndex(peerDeviceNo, idxOld);
		idxNew.dnMap = idxOld.dnMap;
	    }
	    d.store.listManifest(idxCur);
	    if(!co_await aRecvAllIncrement(*stream, d.store, peer, res,
			rbuf, ao, cmd, peerDeviceNo, &idxCur, &idxNew))
		co_return;
	    co_await aSendAllIncrement(*stream, d.store, peer, res);
	    CMD(*stream);
	    if(cmd != "done"sv) {
		res.error = "bad protocol"sv;
		co_return;
	    }
	    d.store.saveSyncIndex(peerDeviceNo, idxNew);
	    res.ok = true;
	}
        boost::system::error_code ec; stream->shutdown(ec);
    } catch (const std::exception& e) {
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
