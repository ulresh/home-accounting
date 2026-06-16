package com.github.ulresh.homeaccounting.model

// Событие. Идентичность записи: (editDatetime, recNo, devNo).
data class Event(
    val eventDatetime: String,            // "YYYY-MM-DD" или "YYYY-MM-DD HH:MM"
    val subject: String,
    val cost: Double,
    val editDatetime: String,             // "YYYY-MM-DD HH:MM:SS"
    val recNo: Int,
    val devNo: Int,
    val people: String? = null,
    val volume: String? = null,
    val comment: String? = null,
) {
    fun key(): String = "$editDatetime|$recNo|$devNo"
}

// Устройство сети: [DN, "<публичный ключ>"].
data class Device(
    val no: Int,
    val pubkey: String,
    val name: String = "",
)

// Строка каталога: категория + позиции.
data class CatalogEntry(
    val category: String,
    val items: List<String>,
)

// Схема событийной строки: порядок/состав колонок + состав «ссылки» (reference).
data class Schema(
    val columns: List<String>,
    val reference: List<String>,
)

// Состояние файла для инкрементной синхронизации.
data class FileState(
    val size: Int = 0,
    val sha1: String = "",
)

// Блок данных, передаваемый «без разбора» (хвост файла).
//   kind = "event-tail"  -> [event-tail, yyyymm, offset, size]\n<данные>\n
//   kind = "device-data" / "people-data" / "catalog-data" -> [kind, size]\n<данные>\n
data class SyncBlob(
    val kind: String,
    val month: Int = 0,    // yyyymm, только для event-tail
    val offset: Int = 0,   // только для event-tail
    val data: String = "",
)
