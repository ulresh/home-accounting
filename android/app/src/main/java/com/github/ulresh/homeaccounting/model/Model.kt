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

// Дамп базы для синхронизации.
data class SyncDump(
    val db: String,
    val people: List<String>,
    val catalog: List<CatalogEntry>,
    val devices: List<Device>,
    val events: List<Pair<String, String>>,   // (monthfile, raw jsonl line)
)
