package com.github.ulresh.homeaccounting

import com.github.ulresh.homeaccounting.model.Store
import com.github.ulresh.homeaccounting.sync.SyncClient
import com.github.ulresh.homeaccounting.sync.SyncResult
import com.github.ulresh.homeaccounting.sync.SyncServer
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files

// Юнит-тесты нового формата событий и инкрементной синхронизации.
// Полностью на JVM (без эмулятора/Appium): два Store + реальные сокеты на localhost.
class StoreSyncTest {

    private fun tmpRoot(): File =
        File(Files.createTempDirectory("ha").toFile(), ".data/home-accounting")

    private fun freshStore(): Store {
        val s = Store(tmpRoot())
        s.load(); s.ensureIdentity()
        return s
    }

    private fun sync(a: Store, b: Store): Pair<SyncResult, SyncResult> {
        val server = SyncServer(a)
        val info = server.listen().copy(ip = "127.0.0.1")
        var ra: SyncResult? = null
        val t = Thread { ra = server.waitAndSync { true } }
        t.start()
        Thread.sleep(200)
        val rb = SyncClient(b).connect(info) { true }
        t.join()
        return ra!! to rb
    }

    private fun readAllMonths(store: Store): String {
        val sb = StringBuilder()
        val base = File(store.root, store.database)
        base.listFiles()?.forEach { dec ->
            if (dec.isDirectory && dec.name.isNotEmpty() && dec.name.all { it.isDigit() }) {
                dec.listFiles()?.forEach { f -> sb.append(f.readText()) }
            }
        }
        return sb.toString()
    }

    private fun countSubject(s: Store, subj: String) = s.events().count { it.subject == subj }

    // 1. comment + формат удаления/редактирования
    @Test fun commentAndDeleteFormat() {
        val s = freshStore()
        val e1 = s.addEvent("2026-06-10", "Кофе", 250.0, null, "1 шт", "из кофейни")
        val e2 = s.addEvent("2026-06-11", "Чай", 120.0, null, null, null)
        s.editEvent(e2, "2026-06-11", "Чай зелёный", 130.0, null, null, "акция")
        s.deleteEvent(e1)

        val content = readAllMonths(s)
        assertTrue("header c comment", content.contains("\"comment\""))
        assertTrue("header c reference", content.contains("\"reference\""))
        assertTrue("комментарий записан", content.contains("из кофейни"))
        assertTrue("удаление с this", content.contains("\"this\""))
        assertTrue("правка update:true", content.contains("\"update\":true"))

        val s2 = Store(s.root); s2.load()
        assertEquals("удалённое не видно", 0, countSubject(s2, "Кофе"))
        assertEquals("старая версия не видна", 0, countSubject(s2, "Чай"))
        assertEquals("новая версия видна", 1, countSubject(s2, "Чай зелёный"))
        assertTrue("комментарий читается",
            s2.events().any { it.subject == "Чай зелёный" && it.comment == "акция" })
    }

    // 2. чужой порядок/состав колонок и reference
    @Test fun foreignSchema() {
        val s = freshStore()
        s.addEvent("2026-06-01", "Молоко", 80.0, null, null, null)

        val odd = File(File(File(s.root, s.database), "2020"), "2503.jsonl")
        odd.parentFile?.mkdirs()
        odd.writeText(
            """{"header":["dev_no","rec_no","edit_datetime","cost","subject","event_datetime","comment"],"reference":["dev_no","rec_no","edit_datetime"]}""" + "\n" +
            """[9,0,"2025-03-01 00:00:00",55,"Хлеб","2025-03-01","свежий"]""" + "\n" +
            """[9,1,"2025-03-02 00:00:00",40,"Кефир","2025-03-02",null]""" + "\n" +
            """{"delete":[9,1,"2025-03-02 00:00:00"],"this":[9,2,"2025-03-03 00:00:00"]}""" + "\n"
        )
        val s2 = Store(s.root); s2.load()
        val bread = s2.events().firstOrNull { it.subject == "Хлеб" }
        assertTrue("событие из чужой схемы видно", bread != null)
        assertEquals("cost по схеме", 55.0, bread!!.cost, 0.001)
        assertEquals("dev_no по схеме", 9, bread.devNo)
        assertEquals("event_datetime по схеме", "2025-03-01", bread.eventDatetime)
        assertEquals("comment по схеме", "свежий", bread.comment)
        assertEquals("удаление по чужому reference", 0, countSubject(s2, "Кефир"))
        assertEquals("наша запись тоже видна", 1, countSubject(s2, "Молоко"))
    }

    // 3. инкрементная синхронизация + индекс
    @Test fun incrementalSync() {
        val a = freshStore(); val b = freshStore()
        a.addEvent("2026-06-02", "Сахар", 60.0, null, null, null)
        a.addEvent("2026-06-03", "Соль", 30.0, null, null, null)
        a.addPerson("Мария")
        a.upsertCatalog(com.github.ulresh.homeaccounting.model.CatalogEntry("Бакалея", listOf("Сахар", "Соль")))

        val (r1a, r1b) = sync(a, b)
        assertTrue("первая синхронизация", r1a.ok && r1b.ok)
        assertTrue("DN различны", a.deviceNo != b.deviceNo)
        assertEquals(1, countSubject(b, "Сахар")); assertEquals(1, countSubject(b, "Соль"))
        assertEquals("люди слиты на сервере", 1, b.people().size)
        assertEquals("каталог получен", 1, b.catalog().size)

        assertTrue("sync-индекс у A",
            File(File(File(a.root, a.database), "sync"), "${b.deviceNo}.jsonl").exists())
        assertTrue("sync-индекс у B",
            File(File(File(b.root, b.database), "sync"), "${a.deviceNo}.jsonl").exists())

        val (r2a, r2b) = sync(a, b)
        assertEquals("повтор без изменений: 0 у A", 0, r2a.received)
        assertEquals("повтор без изменений: 0 у B", 0, r2b.received)

        a.addEvent("2026-06-04", "Перец", 90.0, null, null, null)
        val (_, r3b) = sync(a, b)
        assertEquals("инкремент: ровно 1", 1, r3b.received)
        assertEquals(1, countSubject(b, "Перец"))

        b.addPerson("Пётр")
        sync(a, b)
        assertTrue("A получил Петра", a.people().contains("Пётр"))
        assertTrue("у B Мария+Пётр", b.people().contains("Мария") && b.people().contains("Пётр"))
    }

    // 4. дедупликация одинаковых событий
    @Test fun dedupDuplicates() {
        val a = freshStore(); val b = freshStore()
        a.addEvent("2026-06-05", "Яблоки", 150.0, null, "1 кг", null)
        sync(a, b)
        assertEquals(1, countSubject(b, "Яблоки"))

        b.addEvent("2026-06-05", "Яблоки", 150.0, null, "1 кг", null)
        assertEquals("до синхронизации 2", 2, countSubject(b, "Яблоки"))

        sync(a, b)
        assertEquals("после дедупликации у A 1", 1, countSubject(a, "Яблоки"))
        assertEquals("после дедупликации у B 1", 1, countSubject(b, "Яблоки"))

        sync(a, b)
        assertEquals("повтор: у A 1", 1, countSubject(a, "Яблоки"))
        assertEquals("повтор: у B 1", 1, countSubject(b, "Яблоки"))
    }

    // 5. распространение удаления
    @Test fun deletePropagation() {
        val a = freshStore(); val b = freshStore()
        val ev = a.addEvent("2026-06-06", "Книга", 700.0, null, null, null)
        sync(a, b)
        assertEquals(1, countSubject(b, "Книга"))

        a.deleteEvent(ev)
        assertEquals(0, countSubject(a, "Книга"))
        sync(a, b)
        assertEquals("удаление распространилось", 0, countSubject(b, "Книга"))

        val b2 = Store(b.root); b2.load()
        assertEquals("после перезагрузки не воскресает", 0, countSubject(b2, "Книга"))
    }

    // 6. Прерывание синхронизации (cancel в любом месте)
    @Test fun cancelInterrupts() {
        val a = freshStore()
        val server = SyncServer(a)
        server.listen()
        var r: SyncResult? = null
        val t0 = System.currentTimeMillis()
        val t = Thread { r = server.waitAndSync { true } }
        t.start()
        Thread.sleep(150)
        server.cancel()                       // прервать ожидание подключения
        t.join(3000)
        val took = System.currentTimeMillis() - t0
        assertTrue("ожидание прервано (не выполнено)", r != null && !r!!.ok)
        assertTrue("прерывание сработало быстро, без зависания", took < 3000)
    }

}
