package com.github.ulresh.homeaccounting.model

import com.fasterxml.jackson.databind.JsonNode
import com.fasterxml.jackson.databind.node.DoubleNode
import com.fasterxml.jackson.databind.node.IntNode
import com.fasterxml.jackson.databind.node.LongNode
import com.github.ulresh.homeaccounting.sync.Crypto
import java.io.File
import java.io.InputStream
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
// Разбор JSON — потоковый, через Jackson (см. Jk): файл/блок целиком не грузим.
class Store(val root: File) {

    var database: String = "Основная"
        private set
    var deviceNo: Int = 0
        private set
    var myPubkey: String = ""
        private set
    private var keyStore: KeyStore? = null

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
    // Файл читаем потоково (Jackson читает поток блоками сам), без загрузки целиком.
    private fun readValues(f: File, onValue: (JsonNode) -> Unit) {
        if (!f.exists()) return
        f.inputStream().use { Jk.forEachValue(it, onValue) }
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
    // Размер + SHA1 файла потоком (блоками), без загрузки целиком в память.
    private fun stateOf(f: File): FileState {
        if (!f.exists()) return FileState(0, "")
        val md = MessageDigest.getInstance("SHA-1")
        var total = 0; var any = false
        f.inputStream().use { ins ->
            val block = ByteArray(64 * 1024)
            while (true) { val r = ins.read(block); if (r < 0) break; md.update(block, 0, r); total += r; any = true }
        }
        return FileState(total, if (any) md.digest().joinToString("") { "%02x".format(it) } else "")
    }

    // ---------- схемы ----------
    private fun canonicalSchema() = Schema(
        listOf("event_datetime", "subject", "cost", "edit_datetime", "rec_no", "dev_no", "people", "volume", "comment"),
        listOf("edit_datetime", "rec_no", "dev_no"),
    )
    private fun headerLineFor(sc: Schema): String {
        val o = Jk.obj()
        val h = o.putArray("header"); sc.columns.forEach { h.add(it) }
        val r = o.putArray("reference"); sc.reference.forEach { r.add(it) }
        return o.toString()
    }
    private fun canonicalHeaderLine(): String = headerLineFor(canonicalSchema())
    private fun schemaFromHeader(o: JsonNode): Schema {
        val cols = ArrayList<String>()
        o.get("header")?.let { if (it.isArray) it.forEach { e -> if (e.isTextual) cols.add(e.asText()) } }
        val ref = ArrayList<String>()
        o.get("reference")?.let { if (it.isArray) it.forEach { e -> if (e.isTextual) ref.add(e.asText()) } }
        return Schema(
            if (cols.isEmpty()) canonicalSchema().columns else cols,
            if (ref.isEmpty()) canonicalSchema().reference else ref,
        )
    }

    private data class RecRef(val edit: String, val rn: Int, val dn: Int)
    private fun parseRef(a: JsonNode, ref: List<String>): RecRef {
        var edit = ""; var rn = 0; var dn = 0
        for (i in ref.indices) {
            if (i >= a.size()) break
            when (ref[i]) {
                "edit_datetime" -> edit = a.get(i).asText()
                "rec_no" -> rn = a.get(i).asInt()
                "dev_no" -> dn = a.get(i).asInt()
            }
        }
        return RecRef(edit, rn, dn)
    }
    private fun refIndexOfDn(ref: List<String>) = ref.indexOf("dev_no")

    private fun parseEventArray(a: JsonNode, s: Schema): Event {
        var ed = ""; var subj = ""; var cost = 0.0; var edit = ""; var rn = 0; var dn = 0
        var people: String? = null; var volume: String? = null; var comment: String? = null
        for (i in s.columns.indices) {
            if (i >= a.size()) break
            val v = a.get(i)
            when (s.columns[i]) {
                "event_datetime" -> ed = v.asText()
                "subject" -> subj = v.asText()
                "cost" -> cost = v.asDouble()
                "edit_datetime" -> edit = v.asText()
                "rec_no" -> rn = v.asInt()
                "dev_no" -> dn = v.asInt()
                "people" -> if (v.isTextual) people = v.asText()
                "volume" -> if (v.isTextual) volume = v.asText()
                "comment" -> if (v.isTextual) comment = v.asText()
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

    // Копия массива с заменой одного элемента (для DN map «как получили»).
    private fun JsonNode.withReplaced(idx: Int, v: JsonNode): JsonNode {
        val out = Jk.arr()
        for (i in 0 until this.size()) out.add(if (i == idx) v else this.get(i))
        return out
    }

    // ---------- сериализация события ----------
    private fun costElement(c: Double): JsonNode =
        if (!c.isNaN() && !c.isInfinite() && c == floor(c)) LongNode.valueOf(c.toLong()) else DoubleNode.valueOf(c)
    private fun costStr(c: Double): String = costElement(c).toString()

    private fun eventToLine(e: Event): String {
        val hasC = !e.comment.isNullOrEmpty()
        val hasV = !e.volume.isNullOrEmpty()
        val hasP = !e.people.isNullOrEmpty()
        val a = Jk.arr()
        a.add(e.eventDatetime); a.add(e.subject); a.add(costElement(e.cost))
        a.add(e.editDatetime); a.add(e.recNo); a.add(e.devNo)
        if (hasP || hasV || hasC) {
            if (hasP) a.add(e.people!!) else a.addNull()
            if (hasV || hasC) {
                if (hasV) a.add(e.volume!!) else a.addNull()
                if (hasC) a.add(e.comment!!)
            }
        }
        return a.toString()
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
        readValues(File(root, "database.jsonl")) { el -> if (el.isTextual) dbs.add(el.asText()) }
        readValues(File(root, "config.json")) { el ->
            if (el.isObject) {
                el.get("current_database")?.let { if (it.isTextual) database = it.asText() }
                el.get("current_device_no")?.let { if (it.isNumber) deviceNo = it.asInt() }
            }
        }
        if (dbs.isNotEmpty() && !dbs.contains(database)) database = dbs.first()
        if (dbs.isEmpty()) writeAtomic(File(root, "database.jsonl"), Jk.quote(database) + "\n")
    }

    private fun saveConfig() {
        val o = Jk.obj()
        o.put("current_database", database)
        o.put("current_device_no", deviceNo)
        writeAtomic(File(root, "config.json"), o.toString() + "\n")
    }

    fun databases(): List<String> {
        val dbs = ArrayList<String>()
        readValues(File(root, "database.jsonl")) { el -> if (el.isTextual) dbs.add(el.asText()) }
        if (dbs.isEmpty()) dbs.add(database)
        return dbs
    }

    fun switchDatabase(name: String, create: Boolean) {
        val dbs = databases().toMutableList()
        if (!dbs.contains(name)) {
            if (!create) return
            dbs.add(name)
            writeAtomic(File(root, "database.jsonl"), dbs.joinToString("\n") { Jk.quote(it) } + "\n")
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
            if (el.isArray && el.size() >= 2) {
                val no = el.get(0).asInt()
                val pk = el.get(1).asText()
                val nm = if (el.size() > 2) el.get(2).asText() else ""
                deviceList.add(Device(no, pk, nm))
            }
        }
    }
    private fun saveDevices() {
        val content = deviceList.joinToString("\n") { d ->
            val a = Jk.arr(); a.add(d.no); a.add(d.pubkey); if (d.name.isNotEmpty()) a.add(d.name); a.toString()
        } + "\n"
        writeAtomic(File(dbDir(), "device.jsonl"), content)
    }

    private fun loadPeople() {
        peopleList.clear()
        readValues(File(dbDir(), "people.jsonl")) { el -> if (el.isTextual) peopleList.add(el.asText()) }
    }
    private fun savePeople() {
        writeAtomic(File(dbDir(), "people.jsonl"), peopleList.joinToString("\n") { Jk.quote(it) } + "\n")
    }

    private fun loadCatalog() {
        catalogList.clear()
        readValues(File(dbDir(), "catalog.jsonl")) { el ->
            if (el.isArray && el.size() > 0) {
                val cat = el.get(0).asText()
                val items = (1 until el.size()).map { el.get(it).asText() }
                catalogList.add(CatalogEntry(cat, items))
            }
        }
    }
    private fun saveCatalog() {
        val content = catalogList.joinToString("\n") { e ->
            val a = Jk.arr(); a.add(e.category); e.items.forEach { a.add(it) }; a.toString()
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
                if (el.isObject) {
                    if (el.has("header")) { cur = schemaFromHeader(el); monthSchema[yyyymm] = cur; sawHeader = true }
                    else {
                        val del = el.get("delete")
                        if (del != null && del.isArray) { val t = parseRef(del, cur.reference); applyDeleteToState(keyStr(t.edit, t.rn, t.dn)); anyEventLines = true }
                    }
                } else if (el.isArray) { val e = parseEventArray(el, cur); anyEventLines = true; applyEventToState(e) }
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
        val ym = yyyymmOf(tgtEdit)
        ensureCanonicalHeader(ym)
        val o = Jk.obj()
        val del = o.putArray("delete"); del.add(tgtEdit); del.add(tgtRn); del.add(tgtDn)
        val ths = o.putArray("this"); ths.add(stamp); ths.add(rn); ths.add(deviceNo)
        if (update) o.put("update", true)
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
        val ym = yyyymmOf(eventDatetime)
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

    private class RecvState(val kind: String, val month: Int, val replaceLists: Boolean) {
        var cur: Schema = Schema(emptyList(), emptyList())
        var headerReceived = false
        val people = ArrayList<String>()
        val catalog = ArrayList<CatalogEntry>()
    }
    private class SyncSession(val peerDn: Int) {
        var offsets: Map<Int, Long> = emptyMap()   // СОСТОЯНИЕ СОБЕСЕДНИКА: yyyymm -> сколько наших байт у него
        val dnMap = HashMap<Int, Int>()
        val deleteKeys = HashSet<String>()
        var deleteKeysLoaded = false
        var received = 0
    }
    private var sync: SyncSession? = null

    fun listManifest(): ListManifest = ListManifest(
        stateOf(File(dbDir(), "people.jsonl")),
        stateOf(File(dbDir(), "catalog.jsonl")),
        stateOf(File(dbDir(), "device.jsonl")),
    )

    // Индекс = состояние собеседника: [yyyymm, offset]. Старые записи со строковым ключом игнорируем.
    fun loadSyncIndex(peerDn: Int): HashMap<Int, Long> {
        val off = HashMap<Int, Long>()
        readValues(syncIndexPath(peerDn)) { el ->
            if (el.isArray && el.size() >= 2 && el.get(0).isNumber) off[el.get(0).asInt()] = el.get(1).asLong()
        }
        return off
    }
    fun saveSyncIndex(peerDn: Int, off: Map<Int, Long>) {
        val sb = StringBuilder()
        for ((month, o) in off) { val a = Jk.arr(); a.add(month); a.add(o); sb.append(a.toString()).append("\n") }
        writeAtomic(syncIndexPath(peerDn), sb.toString())
    }

    fun syncBegin(peerDn: Int) {
        val s = SyncSession(peerDn)
        s.offsets = loadSyncIndex(peerDn)
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
                if (el.isObject) {
                    if (el.has("header")) cur = schemaFromHeader(el)
                    else {
                        val del = el.get("delete")
                        if (del != null && del.isArray) {
                            val t = parseRef(del, cur.reference)
                            val u = el.get("update")
                            val upd = u != null && u.isBoolean && u.asBoolean()
                            s.deleteKeys.add(keyStr(t.edit, t.rn, t.dn) + "|" + (if (upd) "1" else "0"))
                        }
                    }
                }
            }
        }
        s.deleteKeysLoaded = true
    }

    fun inEffectHeader(yyyymm: Int): String {
        val sc = monthSchema[yyyymm] ?: return canonicalHeaderLine()
        return headerLineFor(sc)
    }

    // Что отправить партнёру (планы — данные в память не накапливаем). Справочники
    // шлём, только если у партнёра другая версия (его манифест); события — хвостом от offset.
    fun syncPlanOutgoing(peer: ListManifest): List<SyncSendItem> {
        val out = ArrayList<SyncSendItem>()
        val dev = File(dbDir(), "device.jsonl")
        out.add(SyncSendItem("device-data", path = dev, fileFrom = 0, fileLen = fileBytesLen(dev)))
        run {
            val f = File(dbDir(), "people.jsonl"); val me = stateOf(f)
            if (me.size != peer.people.size || me.sha1 != peer.people.sha1)
                out.add(SyncSendItem("people-data", path = f, fileLen = me.size))
        }
        run {
            val f = File(dbDir(), "catalog.jsonl"); val me = stateOf(f)
            if (me.size != peer.catalog.size || me.sha1 != peer.catalog.sha1)
                out.add(SyncSendItem("catalog-data", path = f, fileLen = me.size))
        }
        for ((yyyymm, f) in enumerateMonths()) {
            val off = (sync?.offsets?.get(yyyymm) ?: 0L).toInt()
            val size = fileBytesLen(f)
            if (size <= off) continue
            val prepend = if (off > 0) inEffectHeader(yyyymm) + "\n" else ""
            out.add(SyncSendItem("event-tail", month = yyyymm, offset = off, prepend = prepend,
                                 path = f, fileFrom = off, fileLen = size - off))
        }
        return out
    }

    // Потоковый приём блока: Jackson читает поток инкрементно, значения применяются
    // сразу. ins ограничен размером блока (закрытие сокета не делаем).
    fun syncReceiveBlob(kind: String, month: Int, replaceLists: Boolean, ins: InputStream) {
        if (sync == null) return
        val r = RecvState(kind, month, replaceLists)
        r.cur = canonicalSchema()
        Jk.forEachValue(ins) { v -> handleRecvValue(r, v) }
        when (r.kind) {
            "people-data" -> if (r.replaceLists) { peopleList.clear(); peopleList.addAll(r.people); savePeople() }
                             else { var ch = false; for (p in r.people) if (!peopleList.contains(p)) { peopleList.add(p); ch = true }; if (ch) savePeople() }
            "catalog-data" -> if (r.replaceLists) { catalogList.clear(); catalogList.addAll(r.catalog); saveCatalog() }
                              else for (e in r.catalog) upsertCatalog(e)
        }
    }

    private fun handleRecvValue(r: RecvState, el: JsonNode) {
        val s = sync ?: return
        when (r.kind) {
            "device-data" -> if (el.isArray && el.size() >= 2 && el.get(0).isNumber && el.get(1).isTextual) {
                val peerNo = el.get(0).asInt()
                val pub = el.get(1).asText()
                val name = if (el.size() > 2 && el.get(2).isTextual) el.get(2).asText() else "peer"
                val localNo = reserveDeviceNo(pub, peerNo, name)
                s.dnMap[peerNo] = localNo
            }
            "people-data" -> if (el.isTextual) r.people.add(el.asText())
            "catalog-data" -> if (el.isArray && el.size() > 0) r.catalog.add(CatalogEntry(el.get(0).asText(), (1 until el.size()).map { el.get(it).asText() }))
            "event-tail" -> handleEventTailValue(r, el)
        }
    }

    private fun handleEventTailValue(r: RecvState, el: JsonNode) {
        val s = sync ?: return
        val month = r.month
        if (el.isObject) {
            if (el.has("header")) {
                val sch = schemaFromHeader(el); r.cur = sch; r.headerReceived = true
                if (monthSchema[month] != sch) appendToMonth(month, headerLineFor(sch), true, sch)
                return
            }
            val del = el.get("delete")
            if (del != null && del.isArray) {
                if (!r.headerReceived) { r.cur = canonicalSchema(); if (monthSchema[month] != r.cur) appendToMonth(month, canonicalHeaderLine(), true, r.cur) }
                val tgt = parseRef(del, r.cur.reference)
                val tdn = s.dnMap[tgt.dn] ?: return
                val u = el.get("update"); val upd = u != null && u.isBoolean && u.asBoolean()
                val dkey = keyStr(tgt.edit, tgt.rn, tdn) + "|" + (if (upd) "1" else "0")
                ensureDeleteKeysLoaded()
                if (s.deleteKeys.contains(dkey)) return
                s.deleteKeys.add(dkey)
                val rIdx = refIndexOfDn(r.cur.reference)
                val outObj = Jk.obj()
                outObj.set<JsonNode>("delete", if (rIdx >= 0) del.withReplaced(rIdx, IntNode.valueOf(tdn)) else del)
                val th = el.get("this")
                if (th != null && th.isArray) {
                    val a = parseRef(th, r.cur.reference)
                    val adn = s.dnMap[a.dn] ?: a.dn
                    outObj.set<JsonNode>("this", if (rIdx >= 0) th.withReplaced(rIdx, IntNode.valueOf(adn)) else th)
                }
                if (upd) outObj.put("update", true)
                appendToMonth(month, outObj.toString(), false, null)
                applyDeleteToState(keyStr(tgt.edit, tgt.rn, tdn))
                s.received++
            }
        } else if (el.isArray) {
            if (!r.headerReceived) { r.cur = canonicalSchema(); if (monthSchema[month] != r.cur) appendToMonth(month, canonicalHeaderLine(), true, r.cur) }
            val dnIdx = r.cur.columns.indexOf("dev_no")
            if (dnIdx < 0) return
            val e0 = parseEventArray(el, r.cur)
            val md = s.dnMap[e0.devNo] ?: return
            if (md == deviceNo) return
            val e = e0.copy(devNo = md)
            if (knownEvent(e.key())) return
            val outA = if (dnIdx < el.size()) el.withReplaced(dnIdx, IntNode.valueOf(md)) else el
            appendToMonth(month, outA.toString(), false, null)
            applyEventToState(e)
            s.received++
        }
    }

    fun syncDedup(): Int {
        val groups = LinkedHashMap<String, MutableList<Event>>()
        for (e in live.values) {
            val sig = listOf(e.eventDatetime, e.subject, costStr(e.cost),
                e.people ?: "", e.volume ?: "", e.comment ?: "").joinToString("")
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

    fun syncCommit(peerDn: Int) {
        // Состояние собеседника: после успешного обмена у него есть все наши месячные
        // файлы целиком — фиксируем их текущие размеры как его offset.
        val off = HashMap<Int, Long>()
        for ((yyyymm, f) in enumerateMonths()) off[yyyymm] = f.length()
        saveSyncIndex(peerDn, off)
    }
}
