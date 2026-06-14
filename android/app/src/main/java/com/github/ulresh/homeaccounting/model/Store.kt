package com.github.ulresh.homeaccounting.model

import com.github.ulresh.homeaccounting.sync.Crypto
import kotlinx.serialization.json.*
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.floor
import kotlin.math.max

// Центральное хранилище. root — каталог данных приложения (Context.filesDir).
class Store(val root: File) {

    var database: String = "Основная"
        private set
    var deviceNo: Int = 0
        private set
    var myPubkey: String = ""
        private set

    private val json = Json { ignoreUnknownKeys = true }

    private val peopleList = ArrayList<String>()
    private val catalogList = ArrayList<CatalogEntry>()
    private val deviceList = ArrayList<Device>()

    // Сырые события: (monthfile, line) + индекс присутствия.
    private val rawEvents = ArrayList<Pair<String, String>>()
    private val tokens = HashSet<String>()
    private val live = LinkedHashMap<String, Event>()
    private val deleted = HashSet<String>()

    fun people(): List<String> = peopleList
    fun catalog(): List<CatalogEntry> = catalogList
    fun devices(): List<Device> = deviceList

    private fun dbDir() = File(root, database)
    private fun identityDir() = File(root, "identity")

    // ---------- путь/формат ----------
    private fun decadeFolder(year: Int) = ((year / 10) * 10).toString()
    private fun monthFileName(year: Int, month: Int) =
        "%02d%02d".format(year % 100, month)

    private fun ymOf(dt: String): Pair<Int, Int> {
        if (dt.length >= 7) {
            val y = dt.substring(0, 4).toIntOrNull() ?: 0
            val m = dt.substring(5, 7).toIntOrNull() ?: 1
            return y to m
        }
        return 0 to 1
    }

    private fun nowStamp(): String =
        SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())

    // ---------- ввод/вывод файлов ----------
    // Читаем не построчно: границы каждого значения определяются разбором JSON-структуры
    // (учёт вложенности {}[] и строк), затем каждое значение разбирается JSON-парсером.
    // Устойчиво к значению на нескольких строках и к нескольким значениям в одной строке.
    private fun readValues(f: File, onValue: (JsonElement) -> Unit) {
        if (!f.exists()) return
        val text = f.readText()
        val n = text.length
        var i = 0
        while (i < n) {
            while (i < n && text[i].isWhitespace()) i++
            if (i >= n) break
            val start = i
            when (text[i]) {
                '{', '[' -> {                      // объект или массив — по балансу скобок
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
                '"' -> {                           // строка верхнего уровня
                    i++
                    var esc = false
                    while (i < n) {
                        val ch = text[i]; i++
                        if (esc) esc = false
                        else if (ch == '\\') esc = true
                        else if (ch == '"') break
                    }
                }
                else -> while (i < n && !text[i].isWhitespace()) i++  // число/true/false/null
            }
            val chunk = text.substring(start, i).trim()
            if (chunk.isNotEmpty()) runCatching { onValue(json.parseToJsonElement(chunk)) }
        }
    }

    private fun appendLine(f: File, line: String) {
        f.parentFile?.mkdirs()
        f.appendText(line + "\n")
    }

    private fun writeAtomic(f: File, content: String) {
        f.parentFile?.mkdirs()
        val tmp = File(f.parentFile, f.name + ".tmp." + System.nanoTime())
        tmp.writeText(content)
        if (!tmp.renameTo(f)) {
            f.delete(); tmp.renameTo(f)
        }
    }

    // ---------- загрузка ----------
    fun load() {
        loadConfig()
        loadDevices()
        loadPeople()
        loadCatalog()
        loadEvents()
        rebuildState()
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
        if (dbs.isEmpty()) {
            writeAtomic(File(root, "database.jsonl"), JsonPrimitive(database).toString() + "\n")
        }
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
        rawEvents.clear(); tokens.clear(); live.clear(); deleted.clear()
        saveConfig()
        loadDevices(); loadPeople(); loadCatalog(); loadEvents()
        ensureIdentity()
        rebuildState()
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
            buildJsonArray {
                add(d.no); add(d.pubkey); if (d.name.isNotEmpty()) add(d.name)
            }.toString()
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

    private fun loadEvents() {
        rawEvents.clear(); tokens.clear()
        val base = dbDir()
        if (!base.exists()) return
        base.listFiles()?.forEach { decade ->
            if (decade.isDirectory && decade.name.all { it.isDigit() } && decade.name.isNotEmpty()) {
                decade.listFiles()?.forEach { f ->
                    if (f.isFile && f.name.endsWith(".jsonl")) {
                        val month = f.name.removeSuffix(".jsonl")
                        readValues(f) { el ->
                            val tok = tokenForElement(el)
                            if (tok.isNotEmpty()) {
                                rawEvents.add(month to el.toString())
                                tokens.add(tok)
                            }
                        }
                    }
                }
            }
        }
    }

    // Токен дедупликации по уже разобранному значению.
    private fun tokenForElement(el: JsonElement): String = runCatching {
        when (el) {
            is JsonObject -> {
                val del = el["delete"]
                if (del != null) return "D|" + del.toString()
                if (el.containsKey("header")) return ""   // заголовок
                "X|$el"
            }
            is JsonArray -> {
                if (el.size >= 6) {
                    val edit = el[3].jsonPrimitive.content
                    val rn = el[4].jsonPrimitive.intOrNull ?: 0
                    val dn = el[5].jsonPrimitive.intOrNull ?: 0
                    "E|$edit|$rn|$dn"
                } else "X|$el"
            }
            else -> "X|$el"
        }
    }.getOrDefault("X|$el")

    private fun tokenForLine(line: String): String = runCatching {
        tokenForElement(json.parseToJsonElement(line))
    }.getOrDefault("X|$line")

    private fun rebuildState() {
        live.clear(); deleted.clear()
        // удаления
        for ((_, line) in rawEvents) {
            runCatching {
                val el = json.parseToJsonElement(line)
                if (el is JsonObject) {
                    (el["delete"] as? JsonArray)?.let { a ->
                        val edit = a[0].jsonPrimitive.content
                        val rn = a[1].jsonPrimitive.int
                        val dn = a[2].jsonPrimitive.int
                        deleted.add("$edit|$rn|$dn")
                    }
                }
            }
        }
        // записи
        for ((_, line) in rawEvents) {
            runCatching {
                val el = json.parseToJsonElement(line)
                if (el is JsonArray && el.size >= 6) {
                    val e = Event(
                        eventDatetime = el[0].jsonPrimitive.content,
                        subject = el[1].jsonPrimitive.content,
                        cost = el[2].jsonPrimitive.double,
                        editDatetime = el[3].jsonPrimitive.content,
                        recNo = el[4].jsonPrimitive.int,
                        devNo = el[5].jsonPrimitive.int,
                        people = if (el.size > 6 && el[6] is JsonPrimitive && (el[6] as JsonPrimitive).isString)
                            el[6].jsonPrimitive.content else null,
                        volume = if (el.size > 7 && el[7] is JsonPrimitive && (el[7] as JsonPrimitive).isString)
                            el[7].jsonPrimitive.content else null,
                    )
                    if (!deleted.contains(e.key())) live[e.key()] = e
                }
            }
        }
    }

    fun events(): List<Event> = live.values.sortedByDescending { it.eventDatetime }

    fun categoryOf(subject: String): String {
        for (c in catalogList) if (c.items.contains(subject)) return c.category
        return ""
    }

    // ---------- сериализация события ----------
    private fun costElement(c: Double): JsonElement =
        if (!c.isNaN() && !c.isInfinite() && c == floor(c)) JsonPrimitive(c.toLong()) else JsonPrimitive(c)

    private fun eventToLine(e: Event): String {
        val arr = buildJsonArray {
            add(e.eventDatetime)
            add(e.subject)
            add(costElement(e.cost))
            add(e.editDatetime)
            add(e.recNo)
            add(e.devNo)
            val hasPpl = !e.people.isNullOrEmpty()
            val hasVol = !e.volume.isNullOrEmpty()
            if (hasPpl || hasVol) {
                if (hasPpl) add(e.people!!) else add(JsonNull)
                if (hasVol) add(e.volume!!)
            }
        }
        return arr.toString()
    }

    private fun deleteToLine(edit: String, rn: Int, dn: Int): String =
        buildJsonObject {
            put("delete", buildJsonArray { add(edit); add(rn); add(dn) })
        }.toString()

    // Транслировать DN строки из пространства отправителя в наше (по карте dnMap).
    // null — заголовок (не запись).
    private fun translateLine(line: String, dnMap: Map<Int, Int>): String? {
        fun mapDn(dn: Int) = dnMap[dn] ?: dn
        return runCatching {
            when (val el = json.parseToJsonElement(line)) {
                is JsonObject -> {
                    val del = el["delete"] as? JsonArray
                    when {
                        del != null -> deleteToLine(
                            del[0].jsonPrimitive.content, del[1].jsonPrimitive.int, mapDn(del[2].jsonPrimitive.int)
                        )
                        el.containsKey("header") -> null
                        else -> line
                    }
                }
                is JsonArray -> if (el.size >= 6) {
                    eventToLine(
                        Event(
                            eventDatetime = el[0].jsonPrimitive.content,
                            subject = el[1].jsonPrimitive.content,
                            cost = el[2].jsonPrimitive.double,
                            editDatetime = el[3].jsonPrimitive.content,
                            recNo = el[4].jsonPrimitive.int,
                            devNo = mapDn(el[5].jsonPrimitive.int),
                            people = if (el.size > 6 && el[6] is JsonPrimitive && (el[6] as JsonPrimitive).isString)
                                el[6].jsonPrimitive.content else null,
                            volume = if (el.size > 7 && el[7] is JsonPrimitive && (el[7] as JsonPrimitive).isString)
                                el[7].jsonPrimitive.content else null,
                        )
                    )
                } else line
                else -> line
            }
        }.getOrDefault(line)
    }

    private fun nextRecNo(edit: String): Int {
        var rn = 0
        for ((_, line) in rawEvents) {
            runCatching {
                val el = json.parseToJsonElement(line)
                if (el is JsonArray && el.size >= 6 &&
                    el[3].jsonPrimitive.content == edit &&
                    el[5].jsonPrimitive.int == deviceNo) {
                    rn = max(rn, el[4].jsonPrimitive.int + 1)
                }
            }
        }
        return rn
    }

    private fun appendEventLine(monthFile: String, line: String) {
        val yy = monthFile.substring(0, 2).toIntOrNull() ?: 0
        val year = 2000 + yy
        val f = File(File(dbDir(), decadeFolder(year)), "$monthFile.jsonl")
        val fresh = !f.exists()
        if (fresh) {
            val header = buildJsonObject {
                put("header", buildJsonArray {
                    listOf("event_datetime", "subject", "cost", "edit_datetime",
                        "rec_no", "dev_no", "people", "volume").forEach { add(it) }
                })
            }
            appendLine(f, header.toString())
        }
        appendLine(f, line)
        rawEvents.add(monthFile to line)
        tokens.add(tokenForLine(line))
    }

    // ---------- мутации ----------
    fun addEvent(eventDatetime: String, subject: String, cost: Double,
                 people: String?, volume: String?): Event {
        val edit = nowStamp()
        val e = Event(
            eventDatetime = eventDatetime, subject = subject, cost = cost,
            editDatetime = edit, recNo = nextRecNo(edit), devNo = deviceNo,
            people = people?.ifBlank { null }, volume = volume?.ifBlank { null },
        )
        val (y, m) = ymOf(edit)
        appendEventLine(monthFileName(y, m), eventToLine(e))
        live[e.key()] = e
        return e
    }

    fun deleteEvent(e: Event) {
        val line = deleteToLine(e.editDatetime, e.recNo, e.devNo)
        val (y, m) = ymOf(nowStamp())
        appendEventLine(monthFileName(y, m), line)
        deleted.add(e.key())
        live.remove(e.key())
    }

    fun editEvent(old: Event, eventDatetime: String, subject: String, cost: Double,
                  people: String?, volume: String?): Event {
        deleteEvent(old)
        return addEvent(eventDatetime, subject, cost, people, volume)
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
        } else {
            catalogList.add(e)
        }
        saveCatalog()
    }

    // ---------- идентичность ----------
    fun ensureIdentity() {
        identityDir().mkdirs()
        val (pubkey, _) = Crypto.ensureIdentity(identityDir())
        myPubkey = pubkey

        val found = deviceList.firstOrNull { it.pubkey == myPubkey }
        if (found == null) {
            val maxNo = deviceList.maxOfOrNull { it.no } ?: 0
            var myNo = if (deviceNo > 0) deviceNo else maxNo + 1
            if (deviceList.any { it.no == myNo }) myNo = maxNo + 1
            deviceList.add(Device(myNo, myPubkey, "this"))
            saveDevices()
            deviceNo = myNo
        } else {
            deviceNo = found.no
        }
        saveConfig()
    }

    fun knowsDevice(pubkey: String): Boolean = deviceList.any { it.pubkey == pubkey }

    // Есть ли в базе данные: строки в любом из списков (события, люди, каталог),
    // для устройств — наличие устройств кроме текущего. Список баз не учитывается.
    fun hasData(): Boolean {
        if (rawEvents.isNotEmpty()) return true
        if (peopleList.isNotEmpty()) return true
        if (catalogList.isNotEmpty()) return true
        if (deviceList.any { it.pubkey != myPubkey }) return true
        return false
    }

    fun maxDeviceNo(): Int = deviceList.maxOfOrNull { it.no } ?: 0

    // Сменить собственный номер устройства (безопасно только без своих событий).
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

    // ---------- синхронизация ----------
    fun dump(): SyncDump = SyncDump(
        db = database,
        people = peopleList.toList(),
        catalog = catalogList.toList(),
        devices = deviceList.toList(),
        events = rawEvents.toList(),
    )

    // Слить чужой дамп; вернуть число добавленных событий.
    fun mergeDump(d: SyncDump): Int {
        var peopleChanged = false
        for (p in d.people) if (!peopleList.contains(p)) { peopleList.add(p); peopleChanged = true }
        if (peopleChanged) savePeople()

        for (e in d.catalog) upsertCatalog(e)

        // карта DN: пространство отправителя -> наше (по публичным ключам)
        val dnMap = HashMap<Int, Int>()
        for (dev in d.devices) {
            val local = deviceList.firstOrNull { it.pubkey == dev.pubkey }?.no
                ?: reserveDeviceNo(dev.pubkey, dev.no, dev.name)
            dnMap[dev.no] = local
        }

        var added = 0
        for ((mf, line) in d.events) {
            val tline = translateLine(line, dnMap) ?: continue   // null = заголовок
            val tok = tokenForLine(tline)
            if (tok.isEmpty() || tokens.contains(tok)) continue
            appendEventLine(mf, tline)
            added++
        }
        if (added > 0) rebuildState()
        return added
    }
}
