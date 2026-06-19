package com.github.ulresh.homeaccounting.model

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import java.io.ByteArrayOutputStream

// Инкрементный разбор последовательности JSON-значений из ПОТОКА байт.
// Кормим блоками фиксированного размера (feed), целиком файл/блок в память не
// держим — накапливается только текущее значение. Границы значений определяются
// по структуре (учёт {}[] и строк) на уровне БАЙТ: все структурные символы JSON
// ('{' '}' '[' ']' '"' '\\' и пробелы) — ASCII (<0x80), а многобайтовый UTF-8
// (кириллица в строках) состоит из байт >=0x80, поэтому раскодировка не нужна до
// завершения значения — тогда декодируем его байты как UTF-8 и парсим.
internal class JsonlByteSplitter(private val onValue: (JsonElement) -> Unit) {
    private val json = Json { ignoreUnknownKeys = true }
    private val cur = ByteArrayOutputStream()
    private var mode = 0          // 0=между значениями, 1=объект/массив, 2=строка, 3=скаляр
    private var depth = 0
    private var inStr = false     // внутри строки (для mode 1)
    private var esc = false

    fun feed(data: ByteArray, off: Int, len: Int) {
        var i = off
        val end = off + len
        while (i < end) {
            val b = data[i].toInt() and 0xFF
            when (mode) {
                0 -> {
                    if (b <= ' '.code) { i++; continue }      // пропустить пробелы между значениями
                    cur.reset()
                    when (b) {
                        '{'.code, '['.code -> { mode = 1; depth = 1; inStr = false; esc = false; cur.write(b); i++ }
                        '"'.code -> { mode = 2; esc = false; cur.write(b); i++ }
                        else -> { mode = 3; cur.write(b); i++ }
                    }
                }
                1 -> {
                    cur.write(b); i++
                    if (inStr) {
                        if (esc) esc = false
                        else if (b == '\\'.code) esc = true
                        else if (b == '"'.code) inStr = false
                    } else when (b) {
                        '"'.code -> inStr = true
                        '{'.code, '['.code -> depth++
                        '}'.code, ']'.code -> { depth--; if (depth == 0) emit() }
                    }
                }
                2 -> {
                    cur.write(b); i++
                    if (esc) esc = false
                    else if (b == '\\'.code) esc = true
                    else if (b == '"'.code) emit()
                }
                3 -> {
                    if (b <= ' '.code) emit()                 // скаляр кончается пробелом (его не потребляем)
                    else { cur.write(b); i++ }
                }
            }
        }
    }

    fun finish() { if (mode == 3) emit() }                    // незавершённый хвостовой скаляр

    private fun emit() {
        val s = cur.toString("UTF-8")
        cur.reset(); mode = 0; depth = 0; inStr = false; esc = false
        if (s.isNotEmpty()) runCatching { onValue(json.parseToJsonElement(s)) }
    }
}
