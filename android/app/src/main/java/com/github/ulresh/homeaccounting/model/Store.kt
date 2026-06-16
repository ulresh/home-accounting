package com.github.ulresh.homeaccounting.model

import com.github.ulresh.homeaccounting.sync.Crypto
import kotlinx.serialization.json.*
import java.io.File
import java.security.KeyStore
import java.security.MessageDigest
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import javax.net.ssl.SSLContext
import kotlin.math.floor
import kotlin.math.max

// Центральное хранилище. root — каталог данных приложения (Context.filesDir).
// События — только дозапись; справочники — атомарная перезапись. raw в памяти не
// держим: при загрузке удаления применяются на лету (по месяцам, по порядку).
class Store(val root: File) {

    var database: String = "Основная"
        private set
    var deviceNo: Int = 0
        private set
    var myPubkey: String = ""
        private set
    private var keyStore: KeyStore? = null

    private val json = Json { ignoreUnknownKeys = true }

    private val peopleList = ArrayList<String>()
    private val catalogList = ArrayList<CatalogEntry>()
    private val deviceList = ArrayList<Device>()

    private val live = LinkedHashMap<String, Event>()       // видимые: key -> Event
    private val deletedTargets = HashSet<String>()          // ключи удалённых
    private val monthSchema = HashMap<Int, Schema>()        // действующая схема месяца
    private val seqAtStamp = HashMap<String, Int>()         // счётчик RN на штамп (наши)
    private var anyEventLines = false                       // были ли строки событий

    fun people(): List<String> = peopleList
    fun catalog(): List<CatalogEntry> = catalogList
    fun devices(): List<Device> = deviceList

    private fun dbDir() = File(root, database)
    private fun identityDir() = File(root, "identity")

    // ---------- путь/формат ----------
    private fun decadeFolder(year: Int) = ((year / 10) * 10).toString()
    private fun monthFileName(year: Int, month: Int) = "%02d%02d".format(year % 100, month)

    private fun yyyymmOf(dt: String): Int {
        if (dt.length >= 7) {
            val y = dt.substring(0, 4).toIntOrNull() ?: 0
            val m = dt.substring(5, 7).toIntOrNull() ?: 1
            return y * 100 + m
        }
        return 1
    }

    private fun monthPath(yyyymm: Int): File {
        val year = yyyymm / 100; val month = yyyymm % 100
        return File(File(dbDir(), decadeFolder(year)), monthFileName(year, month) + ".jsonl")
    }
    private fun syncIndexPath(peerDn: Int) = File(File(dbDir(), "sync"), "$peerDn.jsonl")

    private fun nowStamp(): String =
        SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())

    // ---------- ввод/вывод ----------
    // Границы каждого значения определяются разбором JSON-структуры (учёт {}[] и строк),
    // затем каждое значение разбирается JSON-парсером. Устойчиво к значению на нескольких
    // строках и к нескольким значениям в одной строке.
    private fun parseValues(text: String, onValue: (JsonElement) -> Unit) {
        val n = text.length
        var i = 0
        while (i < n) {
            while (i < n && text[i].isWhitespace()) i++
            if (i >= n) break
            val start = i
            when (text[i]) {
                '{', '[' -> {
                    var depth = 0; var inStr = false; var esc = false
                    while (i < n) {
                        val ch = text[i]
                        if (inStr) {
                            when {
                                esc -> esc = false
                                ch == '\\' -> esc = true
                                ch == '"' -> inStr = false
                            }
                            i++
                        } else when (ch) {
                            '"' -> { inStr = true; i++ }
                            '{', '[' -> { depth++; i++ }
                            '}', ']' -> { depth--; i++; if (depth == 0) break }
                            else -> i++
                        }
                    }
                }
                '"' -> {
                    i++
                    var esc = false
                    while (i < n) {
                        val ch = text[i]; i++
                        if (esc) esc = false
                        else if (ch == '\\') esc = true
                        else if (ch == '"') break
                    }
                }
                else -> while (i < n && !text[i].isWhitespace()) i++
            }
            val chunk = text.substring(start, i).trim()
            if (chunk.isNotEmpty()) runCatching { onValue(json.parseToJsonElement(chunk)) }
        }
    }

    private fun readValues(f: File, onValue: (JsonElement) -> Unit) {
        if (!f.exists()) return
        parseValues(f.readText(), onValue)
    }

    private fun appendLine(f: File, line: String) {
        f.parentFile?.mkdirs()
        f.appendText(line + "\n")
    }

    private fun writeAtomic(f: File, content: String) {
        f.parentFile?.mkdirs()
        val tmp = File(f.parentFile, f.name + ".tmp." + System.nanoTime())
        tmp.writeText(content)
        if (!tmp.renameTo(f)) { f.delete(); tmp.renameTo(f) }
    }

    private fun fileBytesLen(f: File): Int = if (f.exists()) f.length().toInt() else 0
    private fun readWholeStr(f: File): String = if (f.exists()) f.readText() else ""
    private fun readSliceStr(f: File, off: Int, len: Int): String {
        if (!f.exists() || len <= 0) return ""
        val all = f.readBytes()
        val end = minOf(off + len, all.size)
        if (off >= end) return ""
        return String(all.copyOfRange(off, end), Charsets.UTF_8)
    }
    private fun sha1hex(b: ByteArray): String =
        MessageDigest.getInstance("SHA-1").digest(b).joinToString("") { "%02x".format(it) }
    private fun stateOf(f: File): FileState {
        if (!f.exists()) return FileState(0, "")
        val b = f.readBytes()
        return FileState(b.size, if (b.isEmpty()) "" else sha1hex(b))
    }

    // ---------- схемы ----------
    private fun canonicalSchema() = Schema(
        listOf("event_datetime", "subject", "cost", "edit_datetime", "rec_no", "dev_no", "people", "volume", "comment"),
        listOf("edit_datetime", "rec_no", "dev_no"),
    )
    private fun canonicalHeaderLine(): String {
        val s = canonicalSchema()
        return buildJsonObject {
            put("header", buildJsonArray { s.columns.forEach { add(it) } })
            put("reference", buildJsonArray { s.reference.forEach { add(it) } })
        }.toString()
    }
    private fun schemaFromHeader(o: JsonObject): Schema {
        val cols = (o["header"] as? JsonArray)?.mapNotNull { (it as? JsonPrimitive)?.takeIf { p -> p.isString }?.content } ?: emptyList()
        val ref = (o["reference"] as? JsonArray)?.mapNotNull { (it as? JsonPrimitive)?.takeIf { p -> p.isString }?.content } ?: emptyList()
        return Schema(
            if (cols.isEmpty()) canonicalSchema().columns else cols,
            if (ref.isEmpty()) canonicalSchema().reference else ref,
        )
    }

    private data class RecRef(val edit: String, val rn: Int, val dn: Int)
    private fun parseRef(a: JsonArray, ref: List<String>): RecRef {
        var edit = ""; var rn = 0; var dn = 0
        for (i in ref.indices) {
            if (i >= a.size) break
            when (ref[i]) {
                "edit_datetime" -> edit = a[i].jsonPrimitive.content
                "rec_no" -> rn = a[i].jsonPrimitive.intOrNull ?: 0
                "dev_no" -> dn = a[i].jsonPrimitive.intOrNull ?: 0
            }
        }
        return RecRef(edit, rn, dn)
    }
    private fun refIndexOfDn(ref: List<String>) = ref.indexOf("dev_no")

    private fun parseEventArray(a: JsonArray, s: Schema): Event {
        var ed = ""; var subj = ""; var cost = 0.0; var edit = ""; var rn = 0; var dn = 0
        var people: String? = null; var volume: String? = null; var comment: String? = null
        for (i in s.columns.indices) {
            if (i >= a.size) break
            val v = a[i]
            when (s.columns[i]) {
                "event_datetime" -> ed = v.jsonPrimitive.content
                "subject" -> subj = v.jsonPrimitive.content
                "cost" -> cost = v.jsonPrimitive.doubleOrNull ?: 0.0
                "edit_datetime" -> edit = v.jsonPrimitive.content
                "rec_no" -> rn = v.jsonPrimitive.intOrNull ?: 0
                "dev_no" -> dn = v.jsonPrimitive.intOrNull ?: 0
                "people" -> if (v is JsonPrimitive && v.isString) people = v.content
                "volume" -> if (v is JsonPrimitive && v.isString) volume = v.content
                "comment" -> if (v is JsonPrimitive && v.isString) comment = v.content
            }
        }
        return Event(ed, subj, cost, edit, rn, dn, people, volume, comment)
    }

    private fun keyStr(edit: String, rn: Int, dn: Int) = "$edit|$rn|$dn"
    private fun splitKey(k: String): RecRef {
        val p2 = k.lastIndexOf('|')
        val p1 = if (p2 <= 0) -1 else k.lastIndexOf('|', p2 - 1)
        if (p1 < 0 || p2 < 0) return RecRef("", 0, 0)
        return RecRef(k.substring(0, p1), k.substring(p1 + 1, p2).toIntOrNull() ?: 0, k.substring(p2 + 1).toIntOrNull() ?: 0)
    }

    private fun JsonArray.withReplaced(idx: Int, v: JsonElement): JsonArray =
        buildJsonArray { this@withReplaced.forEachIndexed { i, e -> add(if (i == idx) v else e) } }

    // ---------- сериализация события ----------
    private fun costElement(c: Double): JsonElement =
        if (!c.isNaN() && !c.isInfinite() && c == floor(c)) JsonPrimitive(c.toLong()) else JsonPrimitive(c)
    private fun costStr(c: Double): String = costElement(c).toString()

    private fun eventToLine(e: Event): String {
        val hasC = !e.comment.isNullOrEmpty()
        val hasV = !e.volume.isNullOrEmpty()
        val hasP = !e.people.isNullOrEmpty()
        return buildJsonArray {
            add(e.eventDatetime); add(e.subject); add(costElement(e.cost))
            add(e.editDatetime); add(e.recNo); add(e.devNo)
            if (hasP || hasV || hasC) {
                if (hasP) add(e.people!!) else add(JsonNull)
                if (hasV || hasC) {
                    if (hasV) add(e.volume!!) else add(JsonNull)
                    if (hasC) add(e.comment!!)
                }
            }
        }.toString()
    }

    // ---------- загрузка ----------
    fun load() {
        live.clear(); deletedTargets.clear(); monthSchema.clear(); seqAtStamp.clear(); anyEventLines = false
        loadConfig()
        loadDevices()
        loadPeople()
        loadCatalog()
        loadEvents()
    }

    private fun loadConfig() {
        val dbs = ArrayList<String>()
        readValues(File(root, "database.jsonl")) { el ->
            (el as? JsonPrimitive)?.let { if (it.isString) dbs.add(it.content) }
        }
        readValues(File(root, "config.json")) { el ->
            (el as? JsonObject)?.let { o ->
                o["current_database"]?.jsonPrimitive?.content?.let { database = it }
                o["current_device_no"]?.jsonPrimitive?.intOrNull?.let { deviceNo = it }
            }
        }
        if (dbs.isNotEmpty() && !dbs.contains(database)) database = dbs.first()
        if (dbs.isEmpty()) writeAtomic(File(root, "database.jsonl"), JsonPrimitive(database).toString() + "\n")
    }

    private fun saveConfig() {
        val o = buildJsonObject {
            put("current_database", database)
            put("current_device_no", deviceNo)
        }
        writeAtomic(File(root, "config.json"), o.toString() + "\n")
    }

    fun databases(): List<String> {
        val dbs = ArrayList<String>()
        readValues(File(root, "database.jsonl")) { el ->
            (el as? JsonPrimitive)?.let { if (it.isString) dbs.add(it.content) }
        }
        if (dbs.isEmpty()) dbs.add(database)
        return dbs
    }

    fun switchDatabase(name: String, create: Boolean) {
        val dbs = databases().toMutableList()
        if (!dbs.contains(name)) {
            if (!create) return
            dbs.add(name)
            val content = dbs.joinToString("\n") { JsonPrimitive(it).toString() } + "\n"
            writeAtomic(File(root, "database.jsonl"), content)
        }
        database = name
        deviceNo = 0
        peopleList.clear(); catalogList.clear(); deviceList.clear()
        live.clear(); deletedTargets.clear(); monthSchema.clear(); seqAtStamp.clear(); anyEventLines = false
        saveConfig()
        loadDevices(); loadPeople(); loadCatalog(); loadEvents()
        ensureIdentity()
    }

    private fun loadDevices() {
        deviceList.clear()
        readValues(File(dbDir(), "device.jsonl")) { el ->
            runCatching {
                val a = el.jsonArray
                val no = a[0].jsonPrimitive.int
                val pk = a[1].jsonPrimitive.content
                val nm = if (a.size > 2) a[2].jsonPrimitive.content else ""
                deviceList.add(Device(no, pk, nm))
            }
        }
    }
    private fun saveDevices() {
        val content = deviceList.joinToString("\n") { d ->
            buildJsonArray { add(d.no); add(d.pubkey); if (d.name.isNotEmpty()) add(d.name) }.toString()
        } + "\n"
        writeAtomic(File(dbDir(), "device.jsonl"), content)
    }

    private fun loadPeople() {
        peopleList.clear()
        readValues(File(dbDir(), "people.jsonl")) { el ->
            (el as? JsonPrimitive)?.let { if (it.isString) peopleList.add(it.content) }
        }
    }
    private fun savePeople() {
        val content = peopleList.joinToString("\n") { JsonPrimitive(it).toString() } + "\n"
        writeAtomic(File(dbDir(), "people.jsonl"), content)
    }

    private fun loadCatalog() {
        catalogList.clear()
        readValues(File(dbDir(), "catalog.jsonl")) { el ->
            runCatching {
                val a = el.jsonArray
                if (a.isNotEmpty()) {
                    val cat = a[0].jsonPrimitive.content
                    val items = a.drop(1).map { it.jsonPrimitive.content }
                    catalogList.add(CatalogEntry(cat, items))
                }
            }
        }
    }
    private fun saveCatalog() {
        val content = catalogList.joinToString("\n") { e ->
            buildJsonArray { add(e.category); e.items.forEach { add(it) } }.toString()
        } + "\n"
        writeAtomic(File(dbDir(), "catalog.jsonl"), content)
    }

    private fun enumerateMonths(): List<Pair<Int, File>> {
        val out = ArrayList<Pair<Int, File>>()
        val base = dbDir()
        if (!base.exists()) return out
        base.listFiles()?.forEach { dec ->
            if (dec.isDirectory && dec.name.isNotEmpty() && dec.name.all { it.isDigit() }) {
                dec.listFiles()?.forEach { f ->
                    if (f.isFile && f.name.endsWith(".jsonl")) {
                        val stem = f.name.removeSuffix(".jsonl")
                        if (stem.length == 4 && stem.all { it.isDigit() }) {
                            val yymm = stem.toInt()
                            out.add(((2000 + yymm / 100) * 100 + (yymm % 100)) to f)
                        }
                    }
                }
            }
        }
        out.sortBy { it.first }
        return out
    }

    private fun loadEvents() {
        for ((yyyymm, f) in enumerateMonths()) {
            var cur = canonicalSchema()
            var sawHeader = false
            readValues(f) { el ->
                when (el) {
                    is JsonObject -> {
                        if (el.containsKey("header")) { cur = schemaFromHeader(el); monthSchema[yyyymm] = cur; sawHeader = true }
                        else {
                            val del = el["delete"] as? JsonArray
                            if (del != null) { val t = parseRef(del, cur.reference); applyDeleteToState(keyStr(t.edit, t.rn, t.dn)); anyEventLines = true }
                        }
                    }
                    is JsonArray -> { val e = parseEventArray(el, cur); anyEventLines = true; applyEventToState(e) }
                    else -> {}
                }
            }
            if (!sawHeader && !monthSchema.containsKey(yyyymm)) monthSchema[yyyymm] = canonicalSchema()
        }
    }

    private fun applyEventToState(e: Event) {
        val key = e.key()
        if (deletedTargets.contains(key)) return
        live[key] = e
    }
    private fun applyDeleteToState(targetKey: String) {
        live.remove(targetKey)
        deletedTargets.add(targetKey)
    }
    private fun knownEvent(key: String) = live.containsKey(key) || deletedTargets.contains(key)

    fun events(): List<Event> = live.values.sortedByDescending { it.eventDatetime }

    fun categoryOf(subject: String): String {
        for (c in catalogList) if (c.items.contains(subject)) return c.category
        return ""
    }

    private fun scanMaxOwnRn(stamp: String): Int {
        var m = -1
        for (e in live.values) if (e.devNo == deviceNo && e.editDatetime == stamp) m = max(m, e.recNo)
        for (k in deletedTargets) { val r = splitKey(k); if (r.dn == deviceNo && r.edit == stamp) m = max(m, r.rn) }
        return m
    }
    private fun allocRecNo(stamp: String): Int {
        val next = seqAtStamp[stamp] ?: (scanMaxOwnRn(stamp) + 1)
        seqAtStamp[stamp] = next + 1
        return next
    }

    private fun appendToMonth(yyyymm: Int, line: String, isHeader: Boolean, sch: Schema?) {
        appendLine(monthPath(yyyymm), line)
        if (isHeader && sch != null) monthSchema[yyyymm] = sch
        if (!isHeader) anyEventLines = true
    }
    private fun ensureCanonicalHeader(yyyymm: Int) {
        val can = canonicalSchema()
        if (monthSchema[yyyymm] != can) appendToMonth(yyyymm, canonicalHeaderLine(), true, can)
    }

    private fun writeDelete(tgtEdit: String, tgtRn: Int, tgtDn: Int, update: Boolean): Boolean {
        val dkey = keyStr(tgtEdit, tgtRn, tgtDn) + "|" + (if (update) "1" else "0")
        val s = sync
        if (s != null) { ensureDeleteKeysLoaded(); if (s.deleteKeys.contains(dkey)) return false }
        val stamp = nowStamp()
        val rn = allocRecNo(stamp)
        val ym = yyyymmOf(stamp)
        ensureCanonicalHeader(ym)
        val o = buildJsonObject {
            put("delete", buildJsonArray { add(tgtEdit); add(tgtRn); add(tgtDn) })
            put("this", buildJsonArray { add(stamp); add(rn); add(deviceNo) })
            if (update) put("update", true)
        }
        appendToMonth(ym, o.toString(), false, null)
        s?.deleteKeys?.add(dkey)
        return true
    }

    // ---------- мутации ----------
    fun addEvent(eventDatetime: String, subject: String, cost: Double,
                 people: String?, volume: String?, comment: String? = null): Event {
        val edit = nowStamp()
        val e = Event(
            eventDatetime = eventDatetime, subject = subject, cost = cost,
            editDatetime = edit, recNo = allocRecNo(edit), devNo = deviceNo,
            people = people?.ifBlank { null }, volume = volume?.ifBlank { null },
            comment = comment?.ifBlank { null },
        )
        val ym = yyyymmOf(edit)
        ensureCanonicalHeader(ym)
        appendToMonth(ym, eventToLine(e), false, null)
        applyEventToState(e)
        return e
    }

    fun deleteEvent(e: Event) {
        writeDelete(e.editDatetime, e.recNo, e.devNo, false)
        applyDeleteToState(e.key())
    }

    fun editEvent(old: Event, eventDatetime: String, subject: String, cost: Double,
                  people: String?, volume: String?, comment: String? = null): Event {
        writeDelete(old.editDatetime, old.recNo, old.devNo, true)
        applyDeleteToState(old.key())
        return addEvent(eventDatetime, subject, cost, people, volume, comment)
    }

    fun addPerson(name: String) {
        if (name.isBlank() || peopleList.contains(name)) return
        peopleList.add(name)
        savePeople()
    }

    fun upsertCatalog(e: CatalogEntry) {
        val idx = catalogList.indexOfFirst { it.category == e.category }
        if (idx >= 0) {
            val merged = catalogList[idx].items.toMutableList()
            for (it in e.items) if (!merged.contains(it)) merged.add(it)
            catalogList[idx] = CatalogEntry(e.category, merged)
        } else catalogList.add(e)
        saveCatalog()
    }

    fun replaceCatalog(list: List<CatalogEntry>) {
        catalogList.clear()
        catalogList.addAll(list)
        saveCatalog()
    }

    fun categoryMembers(category: String): Set<String> {
        val result = LinkedHashSet<String>()
        val visited = HashSet<String>()
        val stack = ArrayDeque<String>()
        stack.addLast(category)
        while (stack.isNotEmpty()) {
            val cat = stack.removeLast()
            if (!visited.add(cat)) continue
            for (e in catalogList) {
                if (e.category != cat) continue
                for (item in e.items) {
                    result.add(item)
                    if (catalogList.any { it.category == item }) stack.addLast(item)
                }
            }
        }
        return result
    }

    // Умный фильтр: наименование / человек / комментарий / категория. Без учёта регистра.
    fun filter(q: String): List<Event> {
        val all = events()
        if (q.isBlank()) return all
        val memberSet = HashSet<String>()
        for (c in catalogList)
            if (c.category.contains(q, ignoreCase = true)) memberSet.addAll(categoryMembers(c.category))
        return all.filter { e ->
            e.subject.contains(q, ignoreCase = true) ||
                (e.people?.contains(q, ignoreCase = true) == true) ||
                (e.comment?.contains(q, ignoreCase = true) == true) ||
                memberSet.contains(e.subject)
        }
    }

    // ---------- идентичность ----------
    // SSLContext с нашей идентичностью (для синхронизации).
    fun sslContext(): SSLContext = Crypto.sslContext(keyStore ?: error("identity not initialized"))

    fun ensureIdentity() {
        identityDir().mkdirs()
        val (pubkey, ks) = Crypto.ensureIdentity(identityDir())
        myPubkey = pubkey
        keyStore = ks

        var maxNo = 0
        for (d in deviceList) {
            if (d.pubkey == myPubkey) {
                if (deviceNo != d.no) { deviceNo = d.no; saveConfig() }
                return
            } else {
                if (d.no == deviceNo) deviceNo = 0
                if (d.no > maxNo) maxNo = d.no
            }
        }
        if (deviceNo <= 0) deviceNo = maxNo + 1
        deviceList.add(Device(deviceNo, myPubkey, "this"))
        saveDevices()
        saveConfig()
    }

    fun knowsDevice(pubkey: String): Boolean = deviceList.any { it.pubkey == pubkey }

    fun hasData(): Boolean {
        if (anyEventLines) return true
        if (peopleList.isNotEmpty()) return true
        if (catalogList.isNotEmpty()) return true
        if (deviceList.any { it.pubkey != myPubkey }) return true
        return false
    }

    fun maxDeviceNo(): Int = deviceList.maxOfOrNull { it.no } ?: 0

    fun renumberSelf(newNo: Int) {
        val idx = deviceList.indexOfFirst { it.pubkey == myPubkey }
        if (idx >= 0) deviceList[idx] = deviceList[idx].copy(no = newNo)
        deviceNo = newNo
        saveDevices()
        saveConfig()
    }

    fun reserveDeviceNo(pubkey: String, preferredNo: Int, name: String): Int {
        deviceList.firstOrNull { it.pubkey == pubkey }?.let { return it.no }
        val maxNo = deviceList.maxOfOrNull { it.no } ?: 0
        val taken = deviceList.any { it.no == preferredNo }
        val no = if (preferredNo > 0 && !taken) preferredNo else maxNo + 1
        deviceList.add(Device(no, pubkey, name))
        saveDevices()
        return no
    }

    // ============================================================
    //                Инкрементная синхронизация
    // ============================================================

    private class SyncSession(val peerDn: Int) {
        var baseline: Map<String, FileState> = emptyMap()
        val dnMap = HashMap<Int, Int>()
        val deleteKeys = HashSet<String>()
        var deleteKeysLoaded = false
        var received = 0
    }
    private var sync: SyncSession? = null

    fun fileStates(): LinkedHashMap<String, FileState> {
        val st = LinkedHashMap<String, FileState>()
        st["device"] = stateOf(File(dbDir(), "device.jsonl"))
        st["people"] = stateOf(File(dbDir(), "people.jsonl"))
        st["catalog"] = stateOf(File(dbDir(), "catalog.jsonl"))
        for ((yyyymm, f) in enumerateMonths()) st[yyyymm.toString()] = stateOf(f)
        return st
    }

    fun loadSyncIndex(peerDn: Int): HashMap<String, FileState> {
        val st = HashMap<String, FileState>()
        readValues(syncIndexPath(peerDn)) { el ->
            (el as? JsonArray)?.let { a ->
                if (a.size >= 2) {
                    val key = a[0].jsonPrimitive.content
                    val size = a[1].jsonPrimitive.intOrNull ?: 0
                    val sha = if (a.size > 2 && a[2] is JsonPrimitive && (a[2] as JsonPrimitive).isString) a[2].jsonPrimitive.content else ""
                    st[key] = FileState(size, sha)
                }
            }
        }
        return st
    }
    fun saveSyncIndex(peerDn: Int, st: Map<String, FileState>) {
        val sb = StringBuilder()
        for ((key, fsr) in st) {
            val arr = buildJsonArray {
                if (key.isNotEmpty() && key.all { it.isDigit() }) add(key.toLong()) else add(key)
                add(fsr.size); add(fsr.sha1)
            }
            sb.append(arr.toString()).append("\n")
        }
        writeAtomic(syncIndexPath(peerDn), sb.toString())
    }

    fun syncBegin(peerDn: Int) {
        val s = SyncSession(peerDn)
        s.baseline = loadSyncIndex(peerDn)
        sync = s
    }
    fun syncEnd() { sync = null }
    fun syncReceived(): Int = sync?.received ?: 0

    private fun ensureDeleteKeysLoaded() {
        val s = sync ?: return
        if (s.deleteKeysLoaded) return
        for ((_, f) in enumerateMonths()) {
            var cur = canonicalSchema()
            readValues(f) { el ->
                if (el is JsonObject) {
                    if (el.containsKey("header")) cur = schemaFromHeader(el)
                    else {
                        val del = el["delete"] as? JsonArray
                        if (del != null) {
                            val t = parseRef(del, cur.reference)
                            val upd = (el["update"] as? JsonPrimitive)?.booleanOrNull == true
                            s.deleteKeys.add(keyStr(t.edit, t.rn, t.dn) + "|" + (if (upd) "1" else "0"))
                        }
                    }
                }
            }
        }
        s.deleteKeysLoaded = true
    }

    private fun inEffectHeaderAt(f: File, offset: Int): String {
        val prefix = readSliceStr(f, 0, offset)
        var last = ""
        for (line in prefix.split("\n")) {
            if (line.contains("\"header\"")) {
                runCatching { val v = json.parseToJsonElement(line); if (v is JsonObject && v.containsKey("header")) last = line }
            }
        }
        return if (last.isEmpty()) canonicalHeaderLine() else last
    }

    private fun stateChanged(base: Map<String, FileState>, cur: Map<String, FileState>, key: String): Boolean {
        val cv = cur[key] ?: FileState(0, "")
        val b = base[key] ?: return cv.size > 0
        return b.size != cv.size || b.sha1 != cv.sha1
    }

    fun syncBuildOutgoing(forceLists: Boolean): List<SyncBlob> {
        val out = ArrayList<SyncBlob>()
        val cur = fileStates()
        val base = sync?.baseline ?: emptyMap()

        out.add(SyncBlob("device-data", 0, 0, readWholeStr(File(dbDir(), "device.jsonl"))))
        if (forceLists || stateChanged(base, cur, "people"))
            out.add(SyncBlob("people-data", 0, 0, readWholeStr(File(dbDir(), "people.jsonl"))))
        if (forceLists || stateChanged(base, cur, "catalog"))
            out.add(SyncBlob("catalog-data", 0, 0, readWholeStr(File(dbDir(), "catalog.jsonl"))))

        for ((yyyymm, f) in enumerateMonths()) {
            val baseSize = base[yyyymm.toString()]?.size ?: 0
            val size = fileBytesLen(f)
            if (size <= baseSize) continue
            var data = ""
            if (baseSize > 0) data = inEffectHeaderAt(f, baseSize) + "\n"
            data += readSliceStr(f, baseSize, size - baseSize)
            out.add(SyncBlob("event-tail", yyyymm, baseSize, data))
        }
        return out
    }

    fun syncApply(b: SyncBlob, replaceLists: Boolean, addedDevices: MutableSet<String>? = null) {
        when (b.kind) {
            "device-data" -> parseValues(b.data) { el ->
                (el as? JsonArray)?.let { a ->
                    if (a.size >= 2 && a[1] is JsonPrimitive && (a[1] as JsonPrimitive).isString) {
                        val peerNo = a[0].jsonPrimitive.intOrNull ?: return@let
                        val pub = a[1].jsonPrimitive.content
                        val name = if (a.size > 2 && a[2] is JsonPrimitive && (a[2] as JsonPrimitive).isString) a[2].jsonPrimitive.content else "peer"
                        val isNew = !knowsDevice(pub)
                        val localNo = reserveDeviceNo(pub, peerNo, name)
                        if (isNew) addedDevices?.add(pub)
                        sync?.dnMap?.put(peerNo, localNo)
                    }
                }
            }
            "people-data" -> {
                val incoming = ArrayList<String>()
                parseValues(b.data) { el -> (el as? JsonPrimitive)?.let { if (it.isString) incoming.add(it.content) } }
                if (replaceLists) { peopleList.clear(); peopleList.addAll(incoming); savePeople() }
                else { var ch = false; for (p in incoming) if (!peopleList.contains(p)) { peopleList.add(p); ch = true }; if (ch) savePeople() }
            }
            "catalog-data" -> {
                val incoming = ArrayList<CatalogEntry>()
                parseValues(b.data) { el ->
                    (el as? JsonArray)?.let { a -> if (a.isNotEmpty()) incoming.add(CatalogEntry(a[0].jsonPrimitive.content, a.drop(1).map { it.jsonPrimitive.content })) }
                }
                if (replaceLists) { catalogList.clear(); catalogList.addAll(incoming); saveCatalog() }
                else for (e in incoming) upsertCatalog(e)
            }
            "event-tail" -> applyEventTail(b.month, b.data)
        }
    }

    private fun applyEventTail(month: Int, data: String) {
        val s = sync ?: return
        var cur = canonicalSchema()
        parseValues(data) { el ->
            when (el) {
                is JsonObject -> {
                    if (el.containsKey("header")) {
                        val sch = schemaFromHeader(el)
                        cur = sch
                        if (monthSchema[month] != sch) appendToMonth(month, el.toString(), true, sch)
                    } else {
                        val del = el["delete"] as? JsonArray
                        if (del != null) {
                            val tgt = parseRef(del, cur.reference)
                            val tdn = s.dnMap[tgt.dn] ?: return@parseValues
                            val upd = (el["update"] as? JsonPrimitive)?.booleanOrNull == true
                            val dkey = keyStr(tgt.edit, tgt.rn, tdn) + "|" + (if (upd) "1" else "0")
                            ensureDeleteKeysLoaded()
                            if (s.deleteKeys.contains(dkey)) return@parseValues
                            s.deleteKeys.add(dkey)
                            val rIdx = refIndexOfDn(cur.reference)
                            val outObj = buildJsonObject {
                                put("delete", if (rIdx >= 0) del.withReplaced(rIdx, JsonPrimitive(tdn)) else del)
                                val th = el["this"] as? JsonArray
                                if (th != null) {
                                    val a = parseRef(th, cur.reference)
                                    val adn = s.dnMap[a.dn] ?: a.dn
                                    put("this", if (rIdx >= 0) th.withReplaced(rIdx, JsonPrimitive(adn)) else th)
                                }
                                if (upd) put("update", true)
                            }
                            appendToMonth(month, outObj.toString(), false, null)
                            applyDeleteToState(keyStr(tgt.edit, tgt.rn, tdn))
                            s.received++
                        }
                    }
                }
                is JsonArray -> {
                    val dnIdx = cur.columns.indexOf("dev_no")
                    if (dnIdx < 0) return@parseValues
                    val e0 = parseEventArray(el, cur)
                    val md = s.dnMap[e0.devNo] ?: return@parseValues
                    if (md == deviceNo) return@parseValues
                    val e = e0.copy(devNo = md)
                    if (knownEvent(e.key())) return@parseValues
                    val outA = if (dnIdx < el.size) el.withReplaced(dnIdx, JsonPrimitive(md)) else el
                    appendToMonth(month, outA.toString(), false, null)
                    applyEventToState(e)
                    s.received++
                }
                else -> {}
            }
        }
    }

    fun syncDedup(): Int {
        val groups = LinkedHashMap<String, MutableList<Event>>()
        for (e in live.values) {
            val sig = listOf(e.eventDatetime, e.subject, costStr(e.cost),
                e.people ?: "", e.volume ?: "", e.comment ?: "").joinToString("\u0001")
            groups.getOrPut(sig) { ArrayList() }.add(e)
        }
        var n = 0
        for ((_, vec) in groups) {
            if (vec.size < 2) continue
            vec.sortWith(compareBy({ it.editDatetime }, { it.devNo }, { it.recNo }))
            for (i in 1 until vec.size) {
                val d = vec[i]
                if (writeDelete(d.editDatetime, d.recNo, d.devNo, false)) { applyDeleteToState(d.key()); n++ }
            }
        }
        return n
    }

    fun syncCommit(peerDn: Int) = saveSyncIndex(peerDn, fileStates())
}
