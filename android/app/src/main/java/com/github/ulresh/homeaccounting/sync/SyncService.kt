package com.github.ulresh.homeaccounting.sync

import com.github.ulresh.homeaccounting.model.ListManifest
import com.github.ulresh.homeaccounting.model.FileState
import com.github.ulresh.homeaccounting.model.Store
import com.github.ulresh.homeaccounting.model.SyncSendItem
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

// ---- покадровый ввод/вывод ----
private fun writeLine(out: OutputStream, line: String) {
    out.write((line + "\n").toByteArray(Charsets.UTF_8)); out.flush()
}
private fun readLine(ins: InputStream): String {
    val baos = ByteArrayOutputStream()
    while (true) {
        val b = ins.read()
        if (b < 0) { if (baos.size() == 0) throw java.io.IOException("eof"); break }  // EOF — не зацикливаемся
        if (b == '\n'.code) break
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

// Отдать блоки потоково: читаем файл блоками с диска и сразу пишем в сеть.
private fun sendItems(out: OutputStream, items: List<SyncSendItem>) {
    for (it in items) {
        val header = if (it.kind == "event-tail")
            buildJsonArray { add("event-tail"); add(it.month); add(it.offset); add(it.frameSize()) }
        else
            buildJsonArray { add(it.kind); add(it.frameSize()) }
        out.write((header.toString() + "\n").toByteArray(Charsets.UTF_8))
        if (it.prepend.isNotEmpty()) out.write(it.prepend.toByteArray(Charsets.UTF_8))
        if (it.fileLen > 0 && it.path.exists()) {                 // пустой/отсутствующий файл не открываем
            it.path.inputStream().use { ins ->
                var toSkip = it.fileFrom.toLong()
                while (toSkip > 0) { val s = ins.skip(toSkip); if (s <= 0) break; toSkip -= s }
                val block = ByteArray(16384)
                var rem = it.fileLen
                while (rem > 0) {
                    val r = ins.read(block, 0, minOf(rem, block.size))
                    if (r < 0) break
                    out.write(block, 0, r); rem -= r
                }
            }
        }
        out.write('\n'.code)
    }
    out.write("[\"end\"]\n".toByteArray(Charsets.UTF_8))
    out.flush()
}

// Принять блоки до ["end"], каждый сразу (по блокам сети) скармливать в store.
private fun recvItems(ins: InputStream, store: Store, replaceLists: Boolean) {
    while (true) {
        val line = readLine(ins)
        if (line.isEmpty()) continue
        val a = J.parseToJsonElement(line).jsonArray
        val kind = a[0].jsonPrimitive.content
        if (kind == "end") break
        var month = 0
        val size: Int
        if (kind == "event-tail") { month = a[1].jsonPrimitive.int; size = a[3].jsonPrimitive.int }
        else size = a[1].jsonPrimitive.int
        store.syncRecvBegin(kind, month, replaceLists)
        var rem = size
        val block = ByteArray(16384)
        while (rem > 0) {
            val r = ins.read(block, 0, minOf(rem, block.size))
            if (r < 0) throw java.io.IOException("short read")
            store.syncRecvFeed(block, 0, r); rem -= r
        }
        store.syncRecvFinish()
        readExact(ins, 1)   // завершающий '\n'
    }
}

// ---- манифест справочников (состояние собеседника) ----
private fun manifestJson(m: ListManifest): String = buildJsonObject {
    put("manifest", buildJsonObject {
        put("people", buildJsonArray { add(m.people.size); add(m.people.sha1) })
        put("catalog", buildJsonArray { add(m.catalog.size); add(m.catalog.sha1) })
        put("device", buildJsonArray { add(m.device.size); add(m.device.sha1) })
    })
}.toString()
private fun manifestParse(line: String): ListManifest = runCatching {
    val mo = J.parseToJsonElement(line).jsonObject["manifest"]!!.jsonObject
    fun rd(k: String): FileState {
        val a = mo[k]?.jsonArray ?: return FileState()
        val sha = if (a.size > 1 && a[1] is JsonPrimitive && (a[1] as JsonPrimitive).isString) a[1].jsonPrimitive.content else ""
        return FileState(a[0].jsonPrimitive.intOrNull ?: 0, sha)
    }
    ListManifest(rd("people"), rd("catalog"), rd("device"))
}.getOrDefault(ListManifest())

private fun summaryLine(received: Int): String =
    buildJsonObject { put("summary", buildJsonObject { put("received", received) }) }.toString()
private fun summaryReceived(line: String): Int =
    J.parseToJsonElement(line).jsonObject["summary"]!!.jsonObject["received"]!!.jsonPrimitive.int

// ---------------- Сервер ----------------
class SyncServer(private val store: Store) {
    private var server: SSLServerSocket? = null
    @Volatile private var sock: SSLSocket? = null
    private var code: String = ""

    fun listen(): PairInfo {
        val ctx = store.sslContext()
        val ss = ctx.serverSocketFactory.createServerSocket(0) as SSLServerSocket
        ss.needClientAuth = true
        server = ss
        code = randomCode(8)
        return PairInfo(localIPv4(), ss.localPort, code, store.database)
    }

    // Прервать в любом месте: закрытие серверного И активного сокета прерывает
    // accept / handshake / любой блокирующий read/write.
    fun cancel() {
        runCatching { server?.close() }
        runCatching { sock?.close() }
    }

    fun waitAndSync(confirm: ConfirmFn): SyncResult {
        val ss = server ?: return SyncResult(error = "not listening")
        return try {
            val s = ss.accept() as SSLSocket
            sock = s
            s.startHandshake()
            val peer = peerPubkey(s)
            val ins = BufferedInputStream(s.inputStream)
            val out = BufferedOutputStream(s.outputStream)

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

            // обмен манифестами справочников
            writeLine(out, manifestJson(store.listManifest()))
            val peerMan = manifestParse(readLine(ins))

            val peerLocal = store.reserveDeviceNo(peer, clientDevNo, "peer")
            store.syncBegin(peerLocal)
            recvItems(ins, store, replaceLists = false)        // принять и слить
            store.syncDedup()
            sendItems(out, store.syncPlanOutgoing(peerMan))    // отдать (потоково)
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
            runCatching { server?.close() }
        }
    }
}

// ---------------- Клиент ----------------
class SyncClient(private val store: Store) {
    @Volatile private var sock: SSLSocket? = null

    fun cancel() { runCatching { sock?.close() } }   // прервать в любом месте

    fun connect(info: PairInfo, confirm: ConfirmFn): SyncResult {
        return try {
            val ctx = store.sslContext()
            val s = ctx.socketFactory.createSocket(info.ip, info.port) as SSLSocket
            sock = s
            s.startHandshake()
            val peer = peerPubkey(s)
            val ins = BufferedInputStream(s.inputStream)
            val out = BufferedOutputStream(s.outputStream)

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

            val serverMan = manifestParse(readLine(ins))       // сервер прислал манифест
            writeLine(out, manifestJson(store.listManifest()))

            val peerLocal = store.reserveDeviceNo(peer, serverDevNo, "peer")
            store.syncBegin(peerLocal)
            sendItems(out, store.syncPlanOutgoing(serverMan))   // отдать (потоково)
            recvItems(ins, store, replaceLists = true)          // принять итог как есть
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
