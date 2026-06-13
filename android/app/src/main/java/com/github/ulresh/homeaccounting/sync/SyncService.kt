package com.github.ulresh.homeaccounting.sync

import com.github.ulresh.homeaccounting.model.CatalogEntry
import com.github.ulresh.homeaccounting.model.Device
import com.github.ulresh.homeaccounting.model.Store
import com.github.ulresh.homeaccounting.model.SyncDump
import kotlinx.serialization.json.*
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.Inet4Address
import java.net.NetworkInterface
import javax.net.ssl.SSLServerSocket
import javax.net.ssl.SSLSocket

data class PairInfo(
    val ip: String,
    val port: Int,
    val code: String,
    val db: String,
) {
    fun toJson(): String = buildJsonObject {
        put("ip", ip); put("port", port); put("code", code); put("db", db)
    }.toString()

    companion object {
        fun fromJson(s: String): PairInfo = runCatching {
            val o = Json.parseToJsonElement(s).jsonObject
            PairInfo(
                ip = o["ip"]!!.jsonPrimitive.content,
                port = o["port"]!!.jsonPrimitive.int,
                code = o["code"]!!.jsonPrimitive.content,
                db = o["db"]?.jsonPrimitive?.content ?: "",
            )
        }.getOrElse { PairInfo("", 0, "", "") }
    }
}

data class SyncResult(
    val ok: Boolean = false,
    val error: String = "",
    val peerDb: String = "",
    val peerPubkey: String = "",
    val sent: Int = 0,
    val received: Int = 0,
)

typealias ConfirmFn = (String) -> Boolean

private val J = Json { ignoreUnknownKeys = true }

internal fun localIPv4(): String =
    runCatching {
        NetworkInterface.getNetworkInterfaces().toList()
            .flatMap { it.inetAddresses.toList() }
            .firstOrNull { !it.isLoopbackAddress && it is Inet4Address }
            ?.hostAddress
    }.getOrNull() ?: "127.0.0.1"

internal fun randomCode(n: Int): String {
    val a = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
    val r = java.security.SecureRandom()
    return (1..n).map { a[r.nextInt(a.length)] }.joinToString("")
}

internal fun dumpToJson(d: SyncDump): JsonObject = buildJsonObject {
    put("db", d.db)
    put("people", buildJsonArray { d.people.forEach { add(it) } })
    put("catalog", buildJsonArray {
        d.catalog.forEach { c -> add(buildJsonArray { add(c.category); c.items.forEach { add(it) } }) }
    })
    put("devices", buildJsonArray {
        d.devices.forEach { dev ->
            add(buildJsonArray { add(dev.no); add(dev.pubkey); if (dev.name.isNotEmpty()) add(dev.name) })
        }
    })
    put("events", buildJsonArray {
        d.events.forEach { (mf, line) -> add(buildJsonArray { add(mf); add(line) }) }
    })
}

internal fun dumpFromJson(o: JsonObject): SyncDump {
    val people = (o["people"] as? JsonArray)?.map { it.jsonPrimitive.content } ?: emptyList()
    val catalog = (o["catalog"] as? JsonArray)?.map { c ->
        val a = c.jsonArray
        CatalogEntry(a[0].jsonPrimitive.content, a.drop(1).map { it.jsonPrimitive.content })
    } ?: emptyList()
    val devices = (o["devices"] as? JsonArray)?.map { d ->
        val a = d.jsonArray
        Device(a[0].jsonPrimitive.int, a[1].jsonPrimitive.content,
            if (a.size > 2) a[2].jsonPrimitive.content else "")
    } ?: emptyList()
    val events = (o["events"] as? JsonArray)?.map { e ->
        val a = e.jsonArray
        a[0].jsonPrimitive.content to a[1].jsonPrimitive.content
    } ?: emptyList()
    return SyncDump(o["db"]?.jsonPrimitive?.content ?: "", people, catalog, devices, events)
}

private fun peerPubkey(s: SSLSocket): String = runCatching {
    Crypto.pubkeyOf(s.session.peerCertificates[0])
}.getOrDefault("")

private fun writeLine(w: BufferedWriter, line: String) {
    w.write(line); w.write("\n"); w.flush()
}

// ---------------- Сервер ----------------
class SyncServer(private val store: Store) {
    private var server: SSLServerSocket? = null
    private var code: String = ""

    fun listen(): PairInfo {
        val ctx = Crypto.sslContext()
        val ss = ctx.serverSocketFactory.createServerSocket(0) as SSLServerSocket
        ss.needClientAuth = true
        server = ss
        code = randomCode(8)
        return PairInfo(localIPv4(), ss.localPort, code, store.database)
    }

    fun cancel() = runCatching { server?.close() }.let {}

    fun waitAndSync(confirm: ConfirmFn): SyncResult {
        val ss = server ?: return SyncResult(error = "not listening")
        return try {
            val sock = ss.accept() as SSLSocket
            sock.startHandshake()
            val peer = peerPubkey(sock)
            val r = BufferedReader(InputStreamReader(sock.inputStream, Charsets.UTF_8))
            val w = BufferedWriter(OutputStreamWriter(sock.outputStream, Charsets.UTF_8))

            val hello = J.parseToJsonElement(r.readLine() ?: "{}").jsonObject["hello"]!!.jsonObject
            val clientDb = hello["db"]!!.jsonPrimitive.content
            val clientCode = hello["code"]!!.jsonPrimitive.content
            val clientDevNo = hello["device_no"]?.jsonPrimitive?.intOrNull ?: 0
            val clientHasData = hello["has_data"]?.jsonPrimitive?.booleanOrNull ?: false

            if (clientCode != code) {
                writeLine(w, """{"error":"bad_code"}""")
                return SyncResult(error = "bad_code", peerDb = clientDb, peerPubkey = peer)
            }
            if (clientDb != store.database) {
                writeLine(w, buildJsonObject { put("error", "db_mismatch"); put("db", store.database) }.toString())
                return SyncResult(error = "db_mismatch", peerDb = clientDb, peerPubkey = peer)
            }
            // Разрешение конфликта собственных номеров (меняет ТОЛЬКО одно устройство).
            if (clientDevNo == store.deviceNo && !store.hasData() && clientHasData) {
                store.renumberSelf(maxOf(store.maxDeviceNo(), clientDevNo) + 1)
            }
            val ackMaxDn = store.maxDeviceNo()   // до резервирования партнёра
            if (!store.knowsDevice(peer)) {
                if (!confirm(peer)) {
                    writeLine(w, """{"error":"rejected"}""")
                    return SyncResult(error = "rejected", peerDb = clientDb, peerPubkey = peer)
                }
                store.reserveDeviceNo(peer, clientDevNo, "peer")
            }

            writeLine(w, buildJsonObject {
                put("ok", true); put("db", store.database)
                put("device_no", store.deviceNo); put("pubkey", store.myPubkey)
                put("max_dn", ackMaxDn)
            }.toString())

            writeLine(w, dumpToJson(store.dump()).toString())
            val remote = dumpFromJson(J.parseToJsonElement(r.readLine() ?: "{}").jsonObject)
            val ks = store.mergeDump(remote)

            writeLine(w, buildJsonObject { put("summary", buildJsonObject { put("received", ks) }) }.toString())
            val cs = J.parseToJsonElement(r.readLine() ?: "{}").jsonObject
            val kc = cs["summary"]!!.jsonObject["received"]!!.jsonPrimitive.int

            runCatching { sock.close() }
            SyncResult(ok = true, peerDb = clientDb, peerPubkey = peer, sent = kc, received = ks)
        } catch (e: Exception) {
            SyncResult(error = e.message ?: "ошибка")
        } finally {
            cancel()
        }
    }
}

// ---------------- Клиент ----------------
class SyncClient(private val store: Store) {
    fun connect(info: PairInfo, confirm: ConfirmFn): SyncResult {
        return try {
            val ctx = Crypto.sslContext()
            val sock = ctx.socketFactory.createSocket(info.ip, info.port) as SSLSocket
            sock.startHandshake()
            val peer = peerPubkey(sock)
            val r = BufferedReader(InputStreamReader(sock.inputStream, Charsets.UTF_8))
            val w = BufferedWriter(OutputStreamWriter(sock.outputStream, Charsets.UTF_8))

            writeLine(w, buildJsonObject {
                put("hello", buildJsonObject {
                    put("db", store.database); put("device_no", store.deviceNo)
                    put("pubkey", store.myPubkey); put("code", info.code)
                    put("has_data", store.hasData())
                })
            }.toString())

            val ack = J.parseToJsonElement(r.readLine() ?: "{}").jsonObject
            ack["error"]?.let { err ->
                val peerDb = ack["db"]?.jsonPrimitive?.content ?: ""
                runCatching { sock.close() }
                return SyncResult(error = err.jsonPrimitive.content, peerDb = peerDb, peerPubkey = peer)
            }
            val peerDb = ack["db"]?.jsonPrimitive?.content ?: ""

            // Разрешение конфликта собственных номеров на стороне клиента
            // (если сервер не сменил свой; меняет только одно устройство).
            run {
                val serverDevNo = ack["device_no"]?.jsonPrimitive?.intOrNull ?: 0
                val serverMaxDn = ack["max_dn"]?.jsonPrimitive?.intOrNull ?: serverDevNo
                if (serverDevNo == store.deviceNo && !store.hasData()) {
                    store.renumberSelf(maxOf(store.maxDeviceNo(), serverMaxDn) + 1)
                }
            }

            if (!store.knowsDevice(peer)) {
                if (!confirm(peer)) { runCatching { sock.close() }; return SyncResult(error = "rejected", peerPubkey = peer) }
                val devNo = ack["device_no"]?.jsonPrimitive?.intOrNull ?: 0
                store.reserveDeviceNo(peer, devNo, "peer")
            }

            val remote = dumpFromJson(J.parseToJsonElement(r.readLine() ?: "{}").jsonObject)
            val kc = store.mergeDump(remote)
            writeLine(w, dumpToJson(store.dump()).toString())

            val sv = J.parseToJsonElement(r.readLine() ?: "{}").jsonObject
            val ks = sv["summary"]!!.jsonObject["received"]!!.jsonPrimitive.int
            writeLine(w, buildJsonObject { put("summary", buildJsonObject { put("received", kc) }) }.toString())

            runCatching { sock.close() }
            SyncResult(ok = true, peerDb = peerDb, peerPubkey = peer, sent = ks, received = kc)
        } catch (e: Exception) {
            SyncResult(error = e.message ?: "ошибка")
        }
    }
}
