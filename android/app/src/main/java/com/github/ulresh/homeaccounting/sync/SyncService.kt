package com.github.ulresh.homeaccounting.sync

import com.github.ulresh.homeaccounting.model.Store
import com.github.ulresh.homeaccounting.model.SyncBlob
import kotlinx.serialization.json.*
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.io.OutputStream
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

private fun peerPubkey(s: SSLSocket): String = runCatching {
    Crypto.pubkeyOf(s.session.peerCertificates[0])
}.getOrDefault("")

// ---- покадровый ввод/вывод (строки + сырые блоки) ----
private fun writeLine(out: OutputStream, line: String) {
    out.write((line + "\n").toByteArray(Charsets.UTF_8)); out.flush()
}
private fun readLine(ins: InputStream): String {
    val baos = ByteArrayOutputStream()
    while (true) {
        val b = ins.read()
        if (b < 0 || b == '\n'.code) break
        baos.write(b)
    }
    var s = baos.toString("UTF-8")
    if (s.endsWith("\r")) s = s.dropLast(1)
    return s
}
private fun readExact(ins: InputStream, n: Int): ByteArray {
    val buf = ByteArray(n)
    var off = 0
    while (off < n) {
        val r = ins.read(buf, off, n - off)
        if (r < 0) throw java.io.IOException("short read")
        off += r
    }
    return buf
}

// Отдать набор блоков: заголовок-строка + сырые данные + '\n'. Завершить ["end"].
private fun sendBlobs(out: OutputStream, blobs: List<SyncBlob>) {
    for (b in blobs) {
        val bytes = b.data.toByteArray(Charsets.UTF_8)
        val header = if (b.kind == "event-tail")
            buildJsonArray { add("event-tail"); add(b.month); add(b.offset); add(bytes.size) }
        else
            buildJsonArray { add(b.kind); add(bytes.size) }
        out.write((header.toString() + "\n").toByteArray(Charsets.UTF_8))
        out.write(bytes)
        out.write('\n'.code)
    }
    out.write("[\"end\"]\n".toByteArray(Charsets.UTF_8))
    out.flush()
}

// Принять блоки до ["end"], каждый сразу применить к store.
private fun recvBlobs(ins: InputStream, store: Store, replaceLists: Boolean) {
    while (true) {
        val line = readLine(ins)
        if (line.isEmpty()) continue
        val a = J.parseToJsonElement(line).jsonArray
        val kind = a[0].jsonPrimitive.content
        if (kind == "end") break
        val blob = if (kind == "event-tail") {
            val month = a[1].jsonPrimitive.int
            val offset = a[2].jsonPrimitive.int
            val size = a[3].jsonPrimitive.int
            val data = String(readExact(ins, size), Charsets.UTF_8)
            readExact(ins, 1)
            SyncBlob("event-tail", month, offset, data)
        } else {
            val size = a[1].jsonPrimitive.int
            val data = String(readExact(ins, size), Charsets.UTF_8)
            readExact(ins, 1)
            SyncBlob(kind, 0, 0, data)
        }
        store.syncApply(blob, replaceLists, null)
    }
}

private fun summaryLine(received: Int): String =
    buildJsonObject { put("summary", buildJsonObject { put("received", received) }) }.toString()
private fun summaryReceived(line: String): Int =
    J.parseToJsonElement(line).jsonObject["summary"]!!.jsonObject["received"]!!.jsonPrimitive.int

// ---------------- Сервер ----------------
class SyncServer(private val store: Store) {
    private var server: SSLServerSocket? = null
    private var code: String = ""

    fun listen(): PairInfo {
        val ctx = store.sslContext()
        val ss = ctx.serverSocketFactory.createServerSocket(0) as SSLServerSocket
        ss.needClientAuth = true
        server = ss
        code = randomCode(8)
        return PairInfo(localIPv4(), ss.localPort, code, store.database)
    }

    fun cancel() = runCatching { server?.close() }.let {}

    fun waitAndSync(confirm: ConfirmFn): SyncResult {
        val ss = server ?: return SyncResult(error = "not listening")
        var sock: SSLSocket? = null
        return try {
            sock = ss.accept() as SSLSocket
            sock.startHandshake()
            val peer = peerPubkey(sock)
            val ins = BufferedInputStream(sock.inputStream)
            val out = BufferedOutputStream(sock.outputStream)

            val hello = J.parseToJsonElement(readLine(ins)).jsonObject["hello"]!!.jsonObject
            val clientDb = hello["db"]!!.jsonPrimitive.content
            val clientCode = hello["code"]!!.jsonPrimitive.content
            val clientDevNo = hello["device_no"]?.jsonPrimitive?.intOrNull ?: 0
            val clientHasData = hello["has_data"]?.jsonPrimitive?.booleanOrNull ?: false

            if (clientCode != code) {
                writeLine(out, """{"error":"bad_code"}""")
                return SyncResult(error = "bad_code", peerDb = clientDb, peerPubkey = peer)
            }
            if (clientDb != store.database) {
                writeLine(out, buildJsonObject { put("error", "db_mismatch"); put("db", store.database) }.toString())
                return SyncResult(error = "db_mismatch", peerDb = clientDb, peerPubkey = peer)
            }
            // Разрешение конфликта собственных номеров (меняет ТОЛЬКО одно устройство).
            if (clientDevNo == store.deviceNo && !store.hasData() && clientHasData) {
                store.renumberSelf(maxOf(store.maxDeviceNo(), clientDevNo) + 1)
            }
            val ackMaxDn = store.maxDeviceNo()
            if (!store.knowsDevice(peer)) {
                if (!confirm(peer)) {
                    writeLine(out, """{"error":"rejected"}""")
                    return SyncResult(error = "rejected", peerDb = clientDb, peerPubkey = peer)
                }
                store.reserveDeviceNo(peer, clientDevNo, "peer")
            }

            writeLine(out, buildJsonObject {
                put("ok", true); put("db", store.database)
                put("device_no", store.deviceNo); put("pubkey", store.myPubkey)
                put("max_dn", ackMaxDn)
            }.toString())

            // ----- инкрементный обмен файлами -----
            val peerLocal = store.reserveDeviceNo(peer, clientDevNo, "peer")
            store.syncBegin(peerLocal)
            recvBlobs(ins, store, replaceLists = false)        // принять и слить
            store.syncDedup()                                  // убрать дубликаты
            sendBlobs(out, store.syncBuildOutgoing(forceLists = true)) // отдать итог
            val ks = store.syncReceived()

            writeLine(out, summaryLine(ks))
            val kc = summaryReceived(readLine(ins))

            store.syncCommit(peerLocal)
            store.syncEnd()
            SyncResult(ok = true, peerDb = clientDb, peerPubkey = peer, sent = kc, received = ks)
        } catch (e: Exception) {
            store.syncEnd()
            SyncResult(error = e.message ?: "ошибка")
        } finally {
            runCatching { sock?.close() }
            cancel()
        }
    }
}

// ---------------- Клиент ----------------
class SyncClient(private val store: Store) {
    fun connect(info: PairInfo, confirm: ConfirmFn): SyncResult {
        var sock: SSLSocket? = null
        return try {
            val ctx = store.sslContext()
            sock = ctx.socketFactory.createSocket(info.ip, info.port) as SSLSocket
            sock.startHandshake()
            val peer = peerPubkey(sock)
            val ins = BufferedInputStream(sock.inputStream)
            val out = BufferedOutputStream(sock.outputStream)

            writeLine(out, buildJsonObject {
                put("hello", buildJsonObject {
                    put("db", store.database); put("device_no", store.deviceNo)
                    put("pubkey", store.myPubkey); put("code", info.code)
                    put("has_data", store.hasData())
                })
            }.toString())

            val ack = J.parseToJsonElement(readLine(ins)).jsonObject
            ack["error"]?.let { err ->
                val peerDb = ack["db"]?.jsonPrimitive?.content ?: ""
                return SyncResult(error = err.jsonPrimitive.content, peerDb = peerDb, peerPubkey = peer)
            }
            val peerDb = ack["db"]?.jsonPrimitive?.content ?: ""
            val serverDevNo = ack["device_no"]?.jsonPrimitive?.intOrNull ?: 0
            val serverMaxDn = ack["max_dn"]?.jsonPrimitive?.intOrNull ?: serverDevNo
            if (serverDevNo == store.deviceNo && !store.hasData()) {
                store.renumberSelf(maxOf(store.maxDeviceNo(), serverMaxDn) + 1)
            }

            if (!store.knowsDevice(peer)) {
                if (!confirm(peer)) return SyncResult(error = "rejected", peerPubkey = peer)
                store.reserveDeviceNo(peer, serverDevNo, "peer")
            }

            // ----- инкрементный обмен файлами -----
            val peerLocal = store.reserveDeviceNo(peer, serverDevNo, "peer")
            store.syncBegin(peerLocal)
            sendBlobs(out, store.syncBuildOutgoing(forceLists = false)) // отдать свой хвост
            recvBlobs(ins, store, replaceLists = true)                  // принять итог как есть
            store.syncDedup()
            val kc = store.syncReceived()

            val ks = summaryReceived(readLine(ins))
            writeLine(out, summaryLine(kc))

            store.syncCommit(peerLocal)
            store.syncEnd()
            SyncResult(ok = true, peerDb = peerDb, peerPubkey = peer, sent = ks, received = kc)
        } catch (e: Exception) {
            store.syncEnd()
            SyncResult(error = e.message ?: "ошибка")
        } finally {
            runCatching { sock?.close() }
        }
    }
}
