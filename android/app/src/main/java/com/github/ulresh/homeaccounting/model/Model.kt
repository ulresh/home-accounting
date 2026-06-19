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

// План отправки одного блока БЕЗ данных в памяти: заголовок + (опц.) prepend +
// содержимое файла [fileFrom, fileFrom+fileLen). Сеть читает файл блоками и сразу шлёт.
//   kind = "event-tail"  -> [event-tail, yyyymm, offset, size]\n<данные>\n
//   kind = "device-data" / "people-data" / "catalog-data" -> [kind, size]\n<данные>\n
data class SyncSendItem(
    val kind: String,
    val month: Int = 0,
    val offset: Int = 0,
    val prepend: String = "",
    val path: java.io.File,
    val fileFrom: Int = 0,
    val fileLen: Int = 0,
) {
    fun frameSize(): Int = prepend.toByteArray(Charsets.UTF_8).size + fileLen
}

// Манифест справочников (для обмена «состоянием» в начале сессии).
data class ListManifest(
    val people: FileState = FileState(),
    val catalog: FileState = FileState(),
    val device: FileState = FileState(),
)
