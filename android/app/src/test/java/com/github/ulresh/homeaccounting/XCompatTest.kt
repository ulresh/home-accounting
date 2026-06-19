package com.github.ulresh.homeaccounting

import com.github.ulresh.homeaccounting.model.CatalogEntry
import com.github.ulresh.homeaccounting.model.ListManifest
import com.github.ulresh.homeaccounting.model.Store
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
        val items = s.syncPlanOutgoing(ListManifest())     // партнёр без данных -> прислать всё
        file.outputStream().use { o ->
            for (it in items) {
                val header = if (it.kind == "event-tail")
                    "[\"event-tail\",${it.month},${it.offset},${it.frameSize()}]"
                else
                    "[\"${it.kind}\",${it.frameSize()}]"
                o.write((header + "\n").toByteArray(Charsets.UTF_8))
                if (it.prepend.isNotEmpty()) o.write(it.prepend.toByteArray(Charsets.UTF_8))
                if (it.fileLen > 0 && it.path.exists()) {
                    it.path.inputStream().use { ins ->
                        var toSkip = it.fileFrom.toLong()
                        while (toSkip > 0) { val k = ins.skip(toSkip); if (k <= 0) break; toSkip -= k }
                        val block = ByteArray(16384); var rem = it.fileLen
                        while (rem > 0) { val r = ins.read(block, 0, minOf(rem, block.size)); if (r < 0) break; o.write(block, 0, r); rem -= r }
                    }
                }
                o.write('\n'.code)
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
            var month = 0
            val size: Int
            if (kind == "event-tail") { month = a[1].jsonPrimitive.int; size = a[3].jsonPrimitive.int }
            else size = a[1].jsonPrimitive.int
            s.syncRecvBegin(kind, month, true)
            var rem = size
            while (rem > 0) {
                val chunk = minOf(rem, 4096)
                s.syncRecvFeed(all, pos, chunk)
                pos += chunk; rem -= chunk
            }
            s.syncRecvFinish()
            if (pos < all.size && all[pos].toInt() == '\n'.code) pos++
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
