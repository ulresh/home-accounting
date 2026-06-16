package com.github.ulresh.homeaccounting

import com.github.ulresh.homeaccounting.model.CatalogEntry
import com.github.ulresh.homeaccounting.model.Store
import com.github.ulresh.homeaccounting.model.SyncBlob
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import kotlin.math.abs

// Кросс-платформенная проверка совместимости Android <-> desktop.
// Управляется переменными окружения (прокидываются в test-воркер из build.gradle.kts):
//   XC_MODE=produce XC_DIR=<dir>  — создать эталонную базу + поток обмена
//   XC_MODE=verify  XC_DIR=<dir>  — прочитать эталон, созданный ДРУГОЙ платформой
// Без XC_MODE оба теста пропускаются (assumeTrue), чтобы обычный прогон не падал.
class XCompatTest {

    private fun buildScenario(s: Store) {
        s.addPerson("Мария")
        s.addPerson("Пётр")
        s.upsertCatalog(CatalogEntry("Бакалея", listOf("Сахар", "Соль")))
        s.upsertCatalog(CatalogEntry("Овощи", listOf("Помидоры")))
        val e1 = s.addEvent("2026-06-10", "Кофе", 250.0, null, "1 шт", "из кофейни")
        s.addEvent("2026-06-11", "Молоко", 12.5, null, "1 л", null)
        val e3 = s.addEvent("2026-06-12", "Сахар", 60.0, "Мария", null, null)
        s.editEvent(e3, "2026-06-12", "Сахар", 65.0, "Мария", null, null)
        s.deleteEvent(e1)
    }

    private fun assertScenario(s: Store, tag: String) {
        val evs = s.events()
        assertEquals("$tag: видимых событий 2", 2, evs.size)
        val milk = evs.firstOrNull { it.subject == "Молоко" }
        assertTrue("$tag: Молоко 12.5 / «1 л» (дробная цена через границу платформ)",
            milk != null && abs(milk.cost - 12.5) < 1e-9 && milk.volume == "1 л")
        val sugar = evs.firstOrNull { it.subject == "Сахар" }
        assertTrue("$tag: Сахар 65 / Мария (правка применилась)",
            sugar != null && abs(sugar.cost - 65.0) < 1e-9 && sugar.people == "Мария")
        assertEquals("$tag: Кофе удалён", 0, evs.count { it.subject == "Кофе" })
        assertTrue("$tag: люди Мария+Пётр", s.people().containsAll(listOf("Мария", "Пётр")))
        assertTrue("$tag: каталог Бакалея(Сахар,Соль)",
            s.catalog().any { it.category == "Бакалея" && it.items.containsAll(listOf("Сахар", "Соль")) })
        assertTrue("$tag: каталог Овощи(Помидоры)",
            s.catalog().any { it.category == "Овощи" && it.items.contains("Помидоры") })
    }

    private fun writeExchange(s: Store, file: File) {
        s.syncBegin(99)
        val blobs = s.syncBuildOutgoing(true)
        file.outputStream().use { o ->
            for (b in blobs) {
                val bytes = b.data.toByteArray(Charsets.UTF_8)
                val header = if (b.kind == "event-tail")
                    "[\"event-tail\",${b.month},${b.offset},${bytes.size}]"
                else
                    "[\"${b.kind}\",${bytes.size}]"
                o.write((header + "\n").toByteArray(Charsets.UTF_8))
                o.write(bytes); o.write('\n'.code)
            }
            o.write("[\"end\"]\n".toByteArray(Charsets.UTF_8))
        }
        s.syncEnd()
    }

    private fun applyExchange(s: Store, file: File) {
        val all = file.readBytes()
        var pos = 0
        fun line(): String {
            val sb = StringBuilder()
            while (pos < all.size) { val c = all[pos].toInt() and 0xFF; pos++; if (c == '\n'.code) break; sb.append(c.toChar()) }
            return sb.toString()
        }
        s.syncBegin(99)
        while (pos < all.size) {
            val hdr = line()
            if (hdr.isEmpty()) continue
            val a = Json.parseToJsonElement(hdr).jsonArray
            val kind = a[0].jsonPrimitive.content
            if (kind == "end") break
            val blob = if (kind == "event-tail") {
                val month = a[1].jsonPrimitive.int; val offset = a[2].jsonPrimitive.int; val size = a[3].jsonPrimitive.int
                val data = String(all, pos, size, Charsets.UTF_8); pos += size
                if (pos < all.size && all[pos].toInt() == '\n'.code) pos++
                SyncBlob("event-tail", month, offset, data)
            } else {
                val size = a[1].jsonPrimitive.int
                val data = String(all, pos, size, Charsets.UTF_8); pos += size
                if (pos < all.size && all[pos].toInt() == '\n'.code) pos++
                SyncBlob(kind, 0, 0, data)
            }
            s.syncApply(blob, true, null)
        }
        s.syncEnd()
    }

    @Test fun crossProduce() {
        assumeTrue(System.getenv("XC_MODE") == "produce")
        val dir = File(System.getenv("XC_DIR"))
        dir.deleteRecursively(); dir.mkdirs()
        val s = Store(dir); s.load(); s.ensureIdentity()
        buildScenario(s)
        writeExchange(s, File(dir, "exchange.bin"))
    }

    @Test fun crossVerify() {
        assumeTrue(System.getenv("XC_MODE") == "verify")
        val dir = File(System.getenv("XC_DIR"))

        // 1) Android читает базу, созданную ДРУГОЙ платформой.
        val s = Store(dir); s.load()
        assertScenario(s, "db")

        // 2) Android принимает проводной поток обмена ДРУГОЙ платформы.
        val cdir = File(System.getProperty("java.io.tmpdir"), "xc_and_consumer")
        cdir.deleteRecursively()
        val c = Store(cdir); c.load(); c.ensureIdentity()
        applyExchange(c, File(dir, "exchange.bin"))
        assertScenario(c, "exchange")
    }
}
